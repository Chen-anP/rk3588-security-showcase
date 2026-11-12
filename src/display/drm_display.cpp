#include "display/drm_display.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string_view>
#include <vector>

#include "core/log.hpp"

namespace rksec::display {

namespace {

constexpr int kMaxCards = 16;
constexpr int kFlipTimeoutMs = 100;  // 60 Hz 一帧 16.7 ms；100 ms 还没翻完说明出问题了

// 输出口类型名，和内核、modetest 的叫法一致（libdrm 2.4.107 才有 drmModeGetConnectorTypeName，这里自己写一份）
const char *connector_type_name(std::uint32_t type)
{
    switch (type) {
    case 1:
        return "VGA";
    case 2:
        return "DVI-I";
    case 3:
        return "DVI-D";
    case 4:
        return "DVI-A";
    case 7:
        return "LVDS";
    case 10:
        return "DP";
    case 11:
        return "HDMI-A";
    case 12:
        return "HDMI-B";
    case 14:
        return "eDP";
    case 15:
        return "Virtual";
    case 16:
        return "DSI";
    case 17:
        return "DPI";
    case 18:
        return "Writeback";
    default:
        return "Unknown";
    }
}

ModeInfo to_info(const drmModeModeInfo &m, std::size_t index)
{
    ModeInfo i;
    i.name = m.name;
    i.clock_khz = m.clock;
    i.hdisplay = m.hdisplay;
    i.hsync_start = m.hsync_start;
    i.hsync_end = m.hsync_end;
    i.htotal = m.htotal;
    i.vdisplay = m.vdisplay;
    i.vsync_start = m.vsync_start;
    i.vsync_end = m.vsync_end;
    i.vtotal = m.vtotal;
    i.preferred = (m.type & DRM_MODE_TYPE_PREFERRED) != 0;
    i.index = index;
    return i;
}

std::string errno_text(int e)
{
    return std::string{std::strerror(e)} + "（errno " + std::to_string(e) + "）";
}

// 是不是 DRM master：做一件只有 master 能做的事（认证 magic 0，内核里是无效值），
// 不是 master 时内核返回 EACCES。新版 libdrm 的 drmIsMaster 也是这么判断的；这里自己写一份，不依赖 libdrm 版本
bool is_master(int fd)
{
    return drmAuthMagic(fd, 0) != -EACCES;
}

// 列出 /dev/dri/card*，问每张卡的驱动名
std::vector<CardInfo> list_cards(std::string &problems)
{
    std::vector<CardInfo> cards;
    for (int i = 0; i < kMaxCards; ++i) {
        const std::string path = "/dev/dri/card" + std::to_string(i);
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            if (errno != ENOENT) {
                problems +=
                    " " + path + " 打不开：" + errno_text(errno) + (errno == EACCES ? "，用户要在 video 组" : "");
            }
            continue;
        }
        CardInfo c{path, ""};
        if (drmVersionPtr v = drmGetVersion(fd)) {
            c.driver = v->name != nullptr ? v->name : "";
            drmFreeVersion(v);
        }
        ::close(fd);
        cards.push_back(c);
    }
    return cards;
}

}  // namespace

struct DrmDisplay::Impl {
    struct Buffer {
        std::uint32_t handle = 0;
        std::uint32_t fb = 0;
        std::uint32_t pitch = 0;
        std::uint64_t size = 0;
        std::uint8_t *map = nullptr;
    };

    int fd = -1;
    std::string card;
    std::string conn_name;
    std::uint32_t conn_id = 0;
    std::uint32_t crtc_id = 0;
    drmModeModeInfo mode{};
    ModeInfo info;
    std::string reason;
    std::uint32_t dpms_prop = 0;
    bool asleep = false;
    std::uint32_t color_prop = 0;  // 连接器属性 color_format 的 id；0 = 不用改
    std::uint64_t color_value = 0;
    std::string color_note;  // 写进 summary 的说明
    std::array<Buffer, 2> buf;
    std::size_t back = 0;  // 下一帧写哪一块
    bool mode_set = false;
    bool flip_pending = false;
    drmModeCrtcPtr saved = nullptr;  // 打开前 CRTC 的状态，关闭时恢复
    Stats stats;

    static void on_flip(int /*fd*/, unsigned /*seq*/, unsigned /*sec*/, unsigned /*usec*/, void *data)
    {
        static_cast<Impl *>(data)->flip_pending = false;
    }

    // 等翻页完成。超时返回 false
    bool wait_flip(int timeout_ms)
    {
        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        drmEventContext ev{};
        ev.version = 2;
        ev.page_flip_handler = &Impl::on_flip;
        while (flip_pending) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) {
                return false;
            }
            pollfd p{fd, POLLIN, 0};
            const int r = ::poll(&p, 1, static_cast<int>(left));
            if (r < 0 && errno != EINTR) {
                return false;
            }
            if (r > 0) {
                drmHandleEvent(fd, &ev);
            }
        }
        return true;
    }

    // 不等待，只把已经到达的翻页完成事件处理掉（翻页早就完成、只是还没读事件的情况）
    void drain_events()
    {
        if (!flip_pending) {
            return;
        }
        drmEventContext ev{};
        ev.version = 2;
        ev.page_flip_handler = &Impl::on_flip;
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 0) > 0) {
            drmHandleEvent(fd, &ev);
        }
    }

    // 找连接器的 color_format 属性，需要改成 rgb 时记下来，第一次设置模式前改（D16）
    void find_connector_properties()
    {
        drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
        if (props == nullptr) {
            return;
        }
        for (std::uint32_t i = 0; i < props->count_props; ++i) {
            drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
            if (p == nullptr) {
                continue;
            }
            if (std::string_view{p->name} == "DPMS" && (p->flags & DRM_MODE_PROP_ENUM) != 0) {
                dpms_prop = p->prop_id;
            }
            if (std::string_view{p->name} == "color_format" && (p->flags & DRM_MODE_PROP_ENUM) != 0) {
                std::vector<EnumValue> enums;
                std::string current_name = std::to_string(props->prop_values[i]);
                for (int k = 0; k < p->count_enums; ++k) {
                    enums.push_back({p->enums[k].name, p->enums[k].value});
                    if (p->enums[k].value == props->prop_values[i]) {
                        current_name = p->enums[k].name;
                    }
                }
                if (const auto v = want_rgb(enums, props->prop_values[i])) {
                    color_prop = p->prop_id;
                    color_value = *v;
                    color_note = "，输出格式 " + current_name + " → rgb";
                } else {
                    color_note = "，输出格式 " + current_name;
                }
            }
            drmModeFreeProperty(p);
        }
        drmModeFreeObjectProperties(props);
    }

    bool create_buffer(Buffer &b, std::string &err)
    {
        drm_mode_create_dumb create{};
        create.width = mode.hdisplay;
        create.height = mode.vdisplay;
        create.bpp = 32;
        if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
            err = "申请显示缓冲区失败：" + errno_text(errno);
            return false;
        }
        b.handle = create.handle;
        b.pitch = create.pitch;
        b.size = create.size;
        if (drmModeAddFB(fd, mode.hdisplay, mode.vdisplay, 24, 32, b.pitch, b.handle, &b.fb) != 0) {
            err = "注册 framebuffer 失败：" + errno_text(errno);
            return false;
        }
        drm_mode_map_dumb map{};
        map.handle = b.handle;
        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
            err = "映射显示缓冲区失败：" + errno_text(errno);
            return false;
        }
        void *p = ::mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(map.offset));
        if (p == MAP_FAILED) {
            err = "mmap 显示缓冲区失败：" + errno_text(errno);
            return false;
        }
        b.map = static_cast<std::uint8_t *>(p);
        std::memset(b.map, 0, b.size);  // 黑屏开始，不显示上一个程序留下的内容
        return true;
    }

    void destroy_buffer(Buffer &b)
    {
        if (b.map != nullptr) {
            ::munmap(b.map, b.size);
        }
        if (b.fb != 0) {
            drmModeRmFB(fd, b.fb);
        }
        if (b.handle != 0) {
            drm_mode_destroy_dumb d{};
            d.handle = b.handle;
            drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        }
        b = Buffer{};
    }
};

DrmDisplay::DrmDisplay() : impl_(std::make_unique<Impl>()) {}

DrmDisplay::~DrmDisplay()
{
    close();
}

bool DrmDisplay::open(const Options &opts, std::string &err)
{
    close();
    Impl &d = *impl_;

    std::string problems;
    const auto cards = list_cards(problems);
    std::string why;
    const auto card = choose_card(cards, opts.device, "rockchip", why);
    if (!card) {
        err = why + problems;
        return false;
    }
    d.card = *card;
    d.fd = ::open(d.card.c_str(), O_RDWR | O_CLOEXEC);
    if (d.fd < 0) {
        err = "打不开 " + d.card + "：" + errno_text(errno);
        return false;
    }
    // 第一个打开显示设备的程序自动成为 master（只有 master 能设置显示）。不是 master 说明别的程序
    // （桌面、另一个本程序）正在控制显示，继续下去会和它抢屏幕（D18），直接报错
    if (!is_master(d.fd)) {
        err = d.card + " 已经被别的程序控制（本程序不是 DRM master）：桌面、modetest 或另一个本程序还在运行？";
        close();
        return false;
    }

    drmModeResPtr res = drmModeGetResources(d.fd);
    if (res == nullptr) {
        err = d.card + " 不是显示设备（drmModeGetResources 失败）";
        close();
        return false;
    }

    // 收集所有输出口的信息，交给 choose_connector / choose_mode 判断
    std::vector<ConnectorInfo> infos;
    std::vector<std::vector<drmModeModeInfo>> raw_modes;
    std::vector<std::optional<drmModeModeInfo>> raw_current;
    std::vector<std::uint32_t> crtc_of;  // 每个输出口可用的 CRTC（当前在用的优先）
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnectorPtr c = drmModeGetConnector(d.fd, res->connectors[i]);
        if (c == nullptr) {
            continue;
        }
        ConnectorInfo info;
        info.id = c->connector_id;
        info.name = std::string{connector_type_name(c->connector_type)} + "-" + std::to_string(c->connector_type_id);
        info.connected = c->connection == DRM_MODE_CONNECTED;
        std::vector<drmModeModeInfo> modes;
        for (int m = 0; m < c->count_modes; ++m) {
            modes.push_back(c->modes[m]);
            info.modes.push_back(to_info(c->modes[m], static_cast<std::size_t>(m)));
        }

        std::uint32_t crtc = 0;
        std::optional<drmModeModeInfo> current;
        if (c->encoder_id != 0) {
            if (drmModeEncoderPtr e = drmModeGetEncoder(d.fd, c->encoder_id)) {
                crtc = e->crtc_id;
                drmModeFreeEncoder(e);
            }
        }
        if (crtc != 0) {
            if (drmModeCrtcPtr cr = drmModeGetCrtc(d.fd, crtc)) {
                if (cr->mode_valid != 0) {
                    current = cr->mode;
                    info.current_mode = to_info(cr->mode, 0);
                }
                drmModeFreeCrtc(cr);
            }
        } else {
            // 还没有 CRTC 在驱动它：从编码器允许的 CRTC 里挑第一个
            for (int e = 0; e < c->count_encoders && crtc == 0; ++e) {
                drmModeEncoderPtr enc = drmModeGetEncoder(d.fd, c->encoders[e]);
                if (enc == nullptr) {
                    continue;
                }
                for (int k = 0; k < res->count_crtcs; ++k) {
                    if ((enc->possible_crtcs & (1U << static_cast<unsigned>(k))) != 0) {
                        crtc = res->crtcs[k];
                        break;
                    }
                }
                drmModeFreeEncoder(enc);
            }
        }
        drmModeFreeConnector(c);
        infos.push_back(std::move(info));
        raw_modes.push_back(std::move(modes));
        raw_current.push_back(current);
        crtc_of.push_back(crtc);
    }
    drmModeFreeResources(res);

    const auto ci = choose_connector(infos, opts.connector, why);
    if (!ci) {
        err = why;
        close();
        return false;
    }
    const auto choice = choose_mode(infos[*ci]);
    if (!choice) {
        err = "输出口 " + infos[*ci].name + " 没有任何可用模式";
        close();
        return false;
    }
    if (crtc_of[*ci] == 0) {
        err = "输出口 " + infos[*ci].name + " 没有可用的 CRTC";
        close();
        return false;
    }
    d.conn_id = infos[*ci].id;
    d.conn_name = infos[*ci].name;
    d.crtc_id = crtc_of[*ci];
    d.mode = infos[*ci].current_mode ? *raw_current[*ci] : raw_modes[*ci][choice->mode.index];
    d.info = choice->mode;
    d.reason = choice->reason;
    d.saved = drmModeGetCrtc(d.fd, d.crtc_id);
    d.find_connector_properties();

    for (auto &b : d.buf) {
        if (!d.create_buffer(b, err)) {
            close();
            return false;
        }
    }
    return true;
}

int DrmDisplay::width() const
{
    return impl_->mode.hdisplay;
}

int DrmDisplay::height() const
{
    return impl_->mode.vdisplay;
}

const ModeInfo &DrmDisplay::mode() const
{
    return impl_->info;
}

std::string DrmDisplay::summary() const
{
    const Impl &d = *impl_;
    return d.card + " " + d.conn_name + " CRTC " + std::to_string(d.crtc_id) + "：" + describe(d.info) + "，" +
           d.reason + d.color_note;
}

bool DrmDisplay::present(const std::uint8_t *pixels, std::size_t stride, std::string &err)
{
    Impl &d = *impl_;
    if (d.fd < 0) {
        err = "显示设备没有打开";
        return false;
    }
    if (d.asleep) {
        return true;  // LVGL 继续处理输入，停掉送往 HDMI 的翻页。
    }
    d.drain_events();  // 翻页完成的事件要读了才知道；先读掉已经到的，剩下的才是真要等的
    if (d.flip_pending) {
        ++d.stats.flip_waits;
        if (!d.wait_flip(kFlipTimeoutMs)) {
            ++d.stats.flip_timeouts;
            d.flip_pending = false;  // 不再等这次翻页，避免之后一直卡住
            err = "等待翻页超时（" + std::to_string(kFlipTimeoutMs) + " ms）";
            return false;
        }
    }

    Impl::Buffer &b = d.buf[d.back];
    const std::size_t row = static_cast<std::size_t>(d.mode.hdisplay) * 4;
    for (std::size_t y = 0; y < d.mode.vdisplay; ++y) {
        std::memcpy(b.map + y * b.pitch, pixels + y * stride, row);
    }

    if (!d.mode_set) {
        // 输出格式改成 RGB（D16）。改了要下一次设置模式才生效，正好在下面的 SetCrtc。
        // 退出时不改回去：原来的 YCbCr 在这块屏上本来就是错的，改回去控制台又会发蓝
        if (d.color_prop != 0 &&
            drmModeObjectSetProperty(d.fd, d.conn_id, DRM_MODE_OBJECT_CONNECTOR, d.color_prop, d.color_value) != 0) {
            LOG_W("display", "把 HDMI 输出格式改成 RGB 失败：%s（颜色可能不对）", std::strerror(errno));
        }
        // 第一帧：设置模式并显示（同步完成，没有翻页事件）
        if (drmModeSetCrtc(d.fd, d.crtc_id, b.fb, 0, 0, &d.conn_id, 1, &d.mode) != 0) {
            const int e = errno;
            err = "设置显示模式失败：" + errno_text(e);
            if (e == EACCES || e == EPERM) {
                err += "。另一个程序正在控制显示（桌面、另一个本程序实例？），同一时间只能有一个";
            }
            return false;
        }
        d.mode_set = true;
    } else if (drmModePageFlip(d.fd, d.crtc_id, b.fb, DRM_MODE_PAGE_FLIP_EVENT, &d) != 0) {
        err = "翻页失败：" + errno_text(errno);
        return false;
    } else {
        d.flip_pending = true;
    }
    d.back ^= 1U;
    ++d.stats.presented;
    return true;
}

bool DrmDisplay::supports_power_control() const
{
    return impl_->dpms_prop != 0;
}

bool DrmDisplay::sleeping() const
{
    return impl_->asleep;
}

bool DrmDisplay::set_sleeping(bool asleep, std::string &err)
{
    auto &d = *impl_;
    if (d.asleep == asleep) {
        return true;
    }
    if (d.fd < 0 || d.dpms_prop == 0 || !d.mode_set) {
        err = "当前 HDMI 输出未就绪或不支持 DPMS";
        return false;
    }
    if (!d.wait_flip(kFlipTimeoutMs)) {
        err = "切换显示电源前等待翻页超时";
        return false;
    }
    const auto power = static_cast<std::uint64_t>(asleep ? DRM_MODE_DPMS_OFF : DRM_MODE_DPMS_ON);
    if (drmModeConnectorSetProperty(d.fd, d.conn_id, d.dpms_prop, power) != 0) {
        err = "切换 HDMI 电源失败：" + errno_text(errno);
        return false;
    }
    d.asleep = asleep;
    return true;
}

void DrmDisplay::wait_idle()
{
    if (impl_->flip_pending && !impl_->wait_flip(kFlipTimeoutMs)) {
        impl_->flip_pending = false;
    }
}

DrmDisplay::Stats DrmDisplay::stats() const
{
    return impl_->stats;
}

void DrmDisplay::close()
{
    Impl &d = *impl_;
    if (d.fd < 0) {
        return;
    }
    std::string power_error;
    if (d.asleep && !set_sleeping(false, power_error)) {
        LOG_W("display", "%s", power_error.c_str());
    }
    wait_idle();
    if (d.mode_set && d.saved != nullptr && d.saved->mode_valid != 0 && d.saved->buffer_id != 0) {
        // 恢复打开前的画面（一般是控制台）
        if (drmModeSetCrtc(d.fd, d.saved->crtc_id, d.saved->buffer_id, d.saved->x, d.saved->y, &d.conn_id, 1,
                           &d.saved->mode) != 0) {
            LOG_W("display", "恢复原来的显示失败：%s", std::strerror(errno));
        }
    }
    if (d.saved != nullptr) {
        drmModeFreeCrtc(d.saved);
    }
    for (auto &b : d.buf) {
        d.destroy_buffer(b);
    }
    ::close(d.fd);
    const Stats keep = d.stats;
    d = Impl{};
    d.stats = keep;
}

}  // namespace rksec::display
