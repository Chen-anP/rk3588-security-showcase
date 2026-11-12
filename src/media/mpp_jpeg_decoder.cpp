#include "media/mpp_jpeg_decoder.hpp"

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_task.h>
#include <rockchip/rk_mpi.h>

#include <cstring>
#include <mutex>
#include <vector>

namespace rksec::media {

namespace {
// MPP 的调试信息里用来标出是谁分配的缓冲区。
// 不用头文件里的 mpp_buffer_group_get_internal 宏：它是可变参数宏，不传参数时 clang 报警告
constexpr const char *kTag = "rksec";
}  // namespace

// 写法参照 MPP 自带的 mpi_dec_test 处理 MJPEG 的 “advanced” 模式（MPP 1.0.x，2023 年，和板子上的版本一致）：
// 每一帧把输入 packet 和一个预先分配好的输出 frame 一起放进任务，硬件把结果直接写进这个 frame 的缓冲区。
struct MppJpegDecoder::Core {
    struct Slot {
        MppBuffer buf = nullptr;
        MppFrame frame = nullptr;
        bool busy = false;
    };

    MppCtx ctx = nullptr;
    MppApi *api = nullptr;
    MppBufferGroup pkt_group = nullptr;
    MppBufferGroup frm_group = nullptr;
    MppBuffer pkt_buf = nullptr;
    std::size_t pkt_size = 0;
    int timeout_ms = 1000;
    std::vector<Slot> slots;

    mutable std::mutex mutex;  // 保护 slots[i].busy 和 stats（Frame 可能在别的线程放手）
    Stats stats;

    ~Core()
    {
        if (ctx != nullptr) {
            api->reset(ctx);
            mpp_destroy(ctx);
        }
        for (auto &s : slots) {
            if (s.frame != nullptr) {
                mpp_frame_deinit(&s.frame);
            }
            if (s.buf != nullptr) {
                mpp_buffer_put(s.buf);
            }
        }
        if (pkt_buf != nullptr) {
            mpp_buffer_put(pkt_buf);
        }
        if (frm_group != nullptr) {
            mpp_buffer_group_put(frm_group);
        }
        if (pkt_group != nullptr) {
            mpp_buffer_group_put(pkt_group);
        }
    }

    void release(std::size_t i)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        slots[i].busy = false;
    }

    // 把一帧送进硬件，等它解完。成功返回 true
    bool run(MppPacket pkt, MppFrame frame, std::string &err)
    {
        const auto wait = static_cast<MppPollType>(timeout_ms);  // 正数 = 最多等这么多毫秒
        MppTask task = nullptr;
        if (api->poll(ctx, MPP_PORT_INPUT, wait) != MPP_OK || api->dequeue(ctx, MPP_PORT_INPUT, &task) != MPP_OK ||
            task == nullptr) {
            err = "MPP 输入队列取任务失败";
            return false;
        }
        mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, pkt);
        mpp_task_meta_set_frame(task, KEY_OUTPUT_FRAME, frame);
        if (api->enqueue(ctx, MPP_PORT_INPUT, task) != MPP_OK) {
            err = "MPP 提交任务失败";
            return false;
        }

        bool ok = true;
        task = nullptr;
        if (api->poll(ctx, MPP_PORT_OUTPUT, wait) != MPP_OK || api->dequeue(ctx, MPP_PORT_OUTPUT, &task) != MPP_OK ||
            task == nullptr) {
            err = "等硬件解码超时（" + std::to_string(timeout_ms) + " ms）";
            ok = false;
        } else {
            api->enqueue(ctx, MPP_PORT_OUTPUT, task);
        }

        // 把输入任务取回来、还给 MPP，输入 packet 才能释放（mpi_dec_test 同样的做法）
        task = nullptr;
        if (api->dequeue(ctx, MPP_PORT_INPUT, &task) == MPP_OK && task != nullptr) {
            api->enqueue(ctx, MPP_PORT_INPUT, task);
        }
        return ok;
    }
};

MppJpegDecoder::MppJpegDecoder() = default;
MppJpegDecoder::~MppJpegDecoder() = default;

bool MppJpegDecoder::open(const Options &opts, std::string &err)
{
    auto c = std::make_shared<Core>();
    c->timeout_ms = opts.timeout_ms;

    // 缓冲区类型写 ION：和 mpi_dec_test 一样。新内核没有 ION，MPP 会自动换成 DRM / dma-heap 分配
    if (mpp_buffer_group_get(&c->pkt_group, MPP_BUFFER_TYPE_ION, MPP_BUFFER_INTERNAL, kTag, __func__) != MPP_OK ||
        mpp_buffer_group_get(&c->frm_group, MPP_BUFFER_TYPE_ION, MPP_BUFFER_INTERNAL, kTag, __func__) != MPP_OK) {
        err = "MPP 缓冲区组创建失败（/dev/mpp_service、/dev/dri 有权限吗？）";
        return false;
    }
    c->pkt_size = opts.max_packet;
    if (mpp_buffer_get(c->pkt_group, &c->pkt_buf, c->pkt_size) != MPP_OK) {
        err = "MPP 输入缓冲区分配失败";
        return false;
    }

    // 输出缓冲区：宽高按 16 对齐；按 4:2:2 留够（每像素 2 字节），再 ×2 留余量（mpi_dec_test 同样留了 4 倍）
    const auto align16 = [](int v) { return static_cast<std::size_t>((v + 15) & ~15); };
    const std::size_t frame_bytes = align16(opts.max_width) * align16(opts.max_height) * 4;
    c->slots.resize(static_cast<std::size_t>(opts.out_buffers));
    for (auto &s : c->slots) {
        if (mpp_buffer_get(c->frm_group, &s.buf, frame_bytes) != MPP_OK || mpp_frame_init(&s.frame) != MPP_OK) {
            err = "MPP 输出缓冲区分配失败（" + std::to_string(opts.out_buffers) + " × " + std::to_string(frame_bytes) +
                  " 字节）";
            return false;
        }
        mpp_frame_set_buffer(s.frame, s.buf);
    }

    if (mpp_create(&c->ctx, &c->api) != MPP_OK) {
        c->ctx = nullptr;
        err = "mpp_create 失败";
        return false;
    }
    if (mpp_init(c->ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG) != MPP_OK) {
        err = "MPP 初始化 MJPEG 解码器失败";
        return false;
    }
    // 不设置解码器参数（mpp_dec_cfg_*）：板子上的 MPP 1.5.0 里，MPP_DEC_SET_CFG 之后解码线程的 mpp_dec_proc_cfg
    // 会从 mpp_dec_cfg_init 分配的 160 字节的末尾开始再写 152 字节——越界写，破坏别的堆内存
    // （ASan：heap-buffer-overflow，D19）。
    // 原来设的 split_parse 是给“从文件读的连续码流”切帧用的；摄像头每次给的就是完整的一帧 JPEG，用默认值即可

    core_ = std::move(c);
    return true;
}

MppJpegDecoder::Frame MppJpegDecoder::decode(const std::uint8_t *data, std::size_t size, std::string &err)
{
    Core *c = core_.get();
    if (c == nullptr) {
        err = "解码器没有打开";
        return nullptr;
    }
    if (data == nullptr || size < 4 || size > c->pkt_size) {
        err = "输入大小不对：" + std::to_string(size) + " 字节（上限 " + std::to_string(c->pkt_size) + "）";
        const std::lock_guard<std::mutex> lock(c->mutex);
        ++c->stats.failed;
        return nullptr;
    }

    std::size_t idx = c->slots.size();
    {
        const std::lock_guard<std::mutex> lock(c->mutex);
        for (std::size_t i = 0; i < c->slots.size(); ++i) {
            if (!c->slots[i].busy) {
                c->slots[i].busy = true;
                idx = i;
                break;
            }
        }
        if (idx == c->slots.size()) {
            ++c->stats.no_buffer;
            err = "输出缓冲区都被占着（下游还没放手）";
            return nullptr;
        }
    }
    Core::Slot &slot = c->slots[idx];

    std::memcpy(mpp_buffer_get_ptr(c->pkt_buf), data, size);
    MppPacket pkt = nullptr;
    mpp_packet_init_with_buffer(&pkt, c->pkt_buf);
    mpp_packet_set_length(pkt, size);  // 缓冲区是最大尺寸，告诉 MPP 实际有多少字节
    const bool ran = c->run(pkt, slot.frame, err);
    mpp_packet_deinit(&pkt);

    const auto fmt = static_cast<MppFrameFormat>(mpp_frame_get_fmt(slot.frame) & MPP_FRAME_FMT_MASK);
    const bool bad = !ran || mpp_frame_get_errinfo(slot.frame) != 0 || mpp_frame_get_discard(slot.frame) != 0;
    YuvLayout l;
    l.width = static_cast<int>(mpp_frame_get_width(slot.frame));
    l.height = static_cast<int>(mpp_frame_get_height(slot.frame));
    l.hstride = static_cast<int>(mpp_frame_get_hor_stride(slot.frame));
    l.vstride = static_cast<int>(mpp_frame_get_ver_stride(slot.frame));
    bool known = true;
    if (fmt == MPP_FMT_YUV420SP) {
        l.format = YuvFormat::nv12;
    } else if (fmt == MPP_FMT_YUV422SP) {
        l.format = YuvFormat::nv16;
    } else {
        known = false;
    }

    if (bad || !known || !l.valid()) {
        if (ran && err.empty()) {
            err = !known ? "不支持的输出格式 0x" + std::to_string(static_cast<unsigned>(fmt))
                         : "硬件解码出错（数据损坏？）";
        }
        c->api->reset(c->ctx);  // 出错后复位，下一帧从干净状态开始
        const std::lock_guard<std::mutex> lock(c->mutex);
        slot.busy = false;
        ++c->stats.failed;
        return nullptr;
    }

    auto *view =
        new YuvView{l, static_cast<const std::uint8_t *>(mpp_buffer_get_ptr(slot.buf)), mpp_buffer_get_fd(slot.buf)};
    {
        const std::lock_guard<std::mutex> lock(c->mutex);
        ++c->stats.ok;
    }
    // 放手时把这块输出缓冲区还回池子。捕获 core_（shared_ptr）：解码器先销毁，底层资源也活到这一帧放手
    std::shared_ptr<Core> keep = core_;
    return Frame{view, [keep, idx](const YuvView *v) {
                     keep->release(idx);
                     delete v;
                 }};
}

MppJpegDecoder::Stats MppJpegDecoder::stats() const
{
    if (!core_) {
        return {};
    }
    const std::lock_guard<std::mutex> lock(core_->mutex);
    return core_->stats;
}

}  // namespace rksec::media
