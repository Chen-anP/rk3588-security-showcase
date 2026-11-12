#include "media/mpp_h264_encoder.hpp"

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>

#include "media/h264_stream.hpp"

namespace rksec::media {

namespace {
constexpr const char *kTag = "rksec";
constexpr int kProfileHigh = 100;
constexpr int kLevel41 = 41;  // 1080p30 以内
// 码率上下限（CBR 允许的波动范围），写法参照 MPP 自带的 mpi_enc_test：目标的 ±1/16
constexpr int kBpsMaxNum = 17;
constexpr int kBpsMinNum = 15;
constexpr int kBpsDen = 16;
constexpr std::size_t kMinPacketBytes = std::size_t{256} * 1024;

MppFrameFormat mpp_format(YuvFormat f)
{
    return f == YuvFormat::nv12 ? MPP_FMT_YUV420SP : MPP_FMT_YUV422SP;
}
}  // namespace

struct MppH264Encoder::Core {
    MppCtx ctx = nullptr;
    MppApi *api = nullptr;
    MppEncCfg cfg = nullptr;
    MppBufferGroup group = nullptr;
    MppBuffer pkt_buf = nullptr;  // 输出码流缓冲区，每帧重复使用
    Options opts;
    std::vector<std::uint8_t> sps_pps;
    std::uint64_t headers_added = 0;  // 自己补了几次 SPS/PPS（日志用）
    // 编码器现在按什么格式、行宽读输入（prep 参数）。open() 时先按 NV12 + 16 对齐登记，
    // 第一帧来了按它的实际值改（apply_input）
    YuvFormat in_format = YuvFormat::nv12;
    int in_hstride = 0;
    int in_vstride = 0;
    std::uint64_t reconfigs = 0;

    ~Core()
    {
        if (ctx != nullptr) {
            api->reset(ctx);
            mpp_destroy(ctx);
        }
        if (cfg != nullptr) {
            mpp_enc_cfg_deinit(cfg);
        }
        if (pkt_buf != nullptr) {
            mpp_buffer_put(pkt_buf);
        }
        if (group != nullptr) {
            mpp_buffer_group_put(group);
        }
    }

    bool set_s32(const char *name, int v, std::string &err) const
    {
        if (mpp_enc_cfg_set_s32(cfg, name, v) != MPP_OK) {
            err = std::string{"MPP 编码参数设置失败："} + name + " = " + std::to_string(v);
            return false;
        }
        return true;
    }

    // 输入格式、行宽和登记的不一样时重新下发 prep 参数。
    // 第 8b 步上板发现：MPP 读输入用的是这里登记的 prep:format，每一帧 mpp_frame_set_fmt 写的格式它不看。
    // 登记成 NV12、实际给 NV16（4:2:2）时，它只读了一半的色度（上半幅画面的颜色），再拉伸铺满整幅：
    // 录像里人脸上半是灰的、只有下巴有颜色。所以按实际输入登记
    bool apply_input(const YuvLayout &l, std::string &err)
    {
        if (l.format == in_format && l.hstride == in_hstride && l.vstride == in_vstride) {
            return true;
        }
        if (!set_s32("prep:format", l.format == YuvFormat::nv12 ? MPP_FMT_YUV420SP : MPP_FMT_YUV422SP, err) ||
            !set_s32("prep:hor_stride", l.hstride, err) || !set_s32("prep:ver_stride", l.vstride, err)) {
            return false;
        }
        if (api->control(ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
            err = std::string{"MPP 不接受输入格式 "} + format_name(l.format) + "（行宽 " + std::to_string(l.hstride) +
                  "、行数 " + std::to_string(l.vstride) + "）";
            return false;
        }
        in_format = l.format;
        in_hstride = l.hstride;
        in_vstride = l.vstride;
        ++reconfigs;
        return true;
    }
};

MppH264Encoder::MppH264Encoder() = default;
MppH264Encoder::~MppH264Encoder() = default;

bool MppH264Encoder::open(const Options &opts, std::string &err)
{
    auto c = std::make_unique<Core>();
    c->opts = opts;

    if (mpp_buffer_group_get(&c->group, MPP_BUFFER_TYPE_ION, MPP_BUFFER_INTERNAL, kTag, __func__) != MPP_OK) {
        err = "MPP 缓冲区组创建失败（/dev/mpp_service、/dev/dri 有权限吗？）";
        return false;
    }
    // 码流缓冲区：一帧压缩后的上限。IDR 最大，按未压缩大小的一半留，至少 256 KB
    const std::size_t raw = static_cast<std::size_t>(opts.width) * static_cast<std::size_t>(opts.height) * 2;
    const std::size_t pkt_bytes = raw / 2 > kMinPacketBytes ? raw / 2 : kMinPacketBytes;
    if (mpp_buffer_get(c->group, &c->pkt_buf, pkt_bytes) != MPP_OK) {
        err = "MPP 码流缓冲区分配失败";
        return false;
    }

    if (mpp_create(&c->ctx, &c->api) != MPP_OK) {
        c->ctx = nullptr;
        err = "mpp_create 失败";
        return false;
    }
    if (mpp_init(c->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        err = "MPP 初始化 H.264 编码器失败";
        return false;
    }
    const RK_S64 timeout = opts.timeout_ms;
    if (c->api->control(c->ctx, MPP_SET_OUTPUT_TIMEOUT, const_cast<RK_S64 *>(&timeout)) != MPP_OK) {
        err = "设置 MPP 输出超时失败";
        return false;
    }

    if (mpp_enc_cfg_init(&c->cfg) != MPP_OK || c->api->control(c->ctx, MPP_ENC_GET_CFG, c->cfg) != MPP_OK) {
        err = "MPP 编码参数初始化失败";
        return false;
    }
    // 输入帧的格式在每一帧里写（NV12 / NV16），这里先按 NV12 登记；步长按 16 对齐（硬件要求）
    const int bps = opts.bitrate_kbps * 1000;
    const bool ok =
        c->set_s32("prep:width", opts.width, err) && c->set_s32("prep:height", opts.height, err) &&
        c->set_s32("prep:hor_stride", (opts.width + 15) & ~15, err) &&
        c->set_s32("prep:ver_stride", (opts.height + 15) & ~15, err) &&
        c->set_s32("prep:format", MPP_FMT_YUV420SP, err) && c->set_s32("rc:mode", MPP_ENC_RC_MODE_CBR, err) &&
        c->set_s32("rc:bps_target", bps, err) && c->set_s32("rc:bps_max", bps / kBpsDen * kBpsMaxNum, err) &&
        c->set_s32("rc:bps_min", bps / kBpsDen * kBpsMinNum, err) && c->set_s32("rc:fps_in_flex", 0, err) &&
        c->set_s32("rc:fps_in_num", opts.fps, err) && c->set_s32("rc:fps_in_denorm", 1, err) &&
        c->set_s32("rc:fps_out_flex", 0, err) && c->set_s32("rc:fps_out_num", opts.fps, err) &&
        c->set_s32("rc:fps_out_denorm", 1, err) && c->set_s32("rc:gop", opts.gop, err) &&
        c->set_s32("codec:type", MPP_VIDEO_CodingAVC, err) && c->set_s32("h264:profile", kProfileHigh, err) &&
        c->set_s32("h264:level", kLevel41, err) && c->set_s32("h264:cabac_en", 1, err) &&
        c->set_s32("h264:cabac_idc", 0, err) && c->set_s32("h264:trans8x8", 1, err);
    if (!ok) {
        return false;
    }
    if (c->api->control(c->ctx, MPP_ENC_SET_CFG, c->cfg) != MPP_OK) {
        err = "MPP 编码参数下发失败（宽高、码率、帧率是否在硬件范围内？）";
        return false;
    }
    c->in_format = YuvFormat::nv12;
    c->in_hstride = (opts.width + 15) & ~15;
    c->in_vstride = (opts.height + 15) & ~15;
    MppEncHeaderMode mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    if (c->api->control(c->ctx, MPP_ENC_SET_HEADER_MODE, &mode) != MPP_OK) {
        err = "设置“每个 IDR 前带 SPS/PPS”失败";
        return false;
    }

    // 取一份 SPS/PPS 存着（RTSP、MP4 开头要用）
    MppPacket hdr = nullptr;
    mpp_packet_init_with_buffer(&hdr, c->pkt_buf);
    mpp_packet_set_length(hdr, 0);  // 必须清零：长度表示“已经写入的字节数”
    if (c->api->control(c->ctx, MPP_ENC_GET_HDR_SYNC, hdr) != MPP_OK) {
        mpp_packet_deinit(&hdr);
        err = "取 SPS/PPS 失败";
        return false;
    }
    const auto *p = static_cast<const std::uint8_t *>(mpp_packet_get_pos(hdr));
    c->sps_pps = extract_parameter_sets(p, mpp_packet_get_length(hdr));
    mpp_packet_deinit(&hdr);
    if (c->sps_pps.empty()) {
        err = "编码器给出的头里没有 SPS/PPS";
        return false;
    }

    core_ = std::move(c);
    return true;
}

std::uint64_t MppH264Encoder::input_reconfigs() const
{
    return core_ ? core_->reconfigs : 0;
}

std::uint64_t MppH264Encoder::headers_added() const
{
    return core_ ? core_->headers_added : 0;
}

std::vector<std::uint8_t> MppH264Encoder::parameter_sets() const
{
    return core_ ? core_->sps_pps : std::vector<std::uint8_t>{};
}

bool MppH264Encoder::encode(const YuvView &frame, std::int64_t pts_us, EncodedPacket &out, std::string &err)
{
    Core *c = core_.get();
    if (c == nullptr) {
        err = "编码器没有打开";
        return false;
    }
    const YuvLayout &l = frame.layout;
    if (frame.fd < 0 || l.width != c->opts.width || l.height != c->opts.height) {
        err = "输入帧不对：" + std::to_string(l.width) + "x" + std::to_string(l.height) + " fd " +
              std::to_string(frame.fd) + "（编码器是 " + std::to_string(c->opts.width) + "x" +
              std::to_string(c->opts.height) + "）";
        return false;
    }

    if (!c->apply_input(l, err)) {
        return false;
    }

    // 把解码器输出的 DMA-BUF 导入 MPP（不拷贝，只是登记 fd）
    MppBufferInfo info{};
    info.type = MPP_BUFFER_TYPE_EXT_DMA;
    info.size = l.total_bytes();
    info.fd = frame.fd;
    MppBuffer in_buf = nullptr;
    if (mpp_buffer_import(&in_buf, &info) != MPP_OK || in_buf == nullptr) {
        err = "导入输入 DMA-BUF 失败";
        return false;
    }

    MppFrame mf = nullptr;
    if (mpp_frame_init(&mf) != MPP_OK) {
        mpp_buffer_put(in_buf);
        err = "mpp_frame_init 失败";
        return false;
    }
    mpp_frame_set_width(mf, static_cast<RK_U32>(l.width));
    mpp_frame_set_height(mf, static_cast<RK_U32>(l.height));
    mpp_frame_set_hor_stride(mf, static_cast<RK_U32>(l.hstride));
    mpp_frame_set_ver_stride(mf, static_cast<RK_U32>(l.vstride));
    mpp_frame_set_fmt(mf, mpp_format(l.format));
    mpp_frame_set_pts(mf, pts_us);
    mpp_frame_set_buffer(mf, in_buf);

    MppPacket pkt = nullptr;
    mpp_packet_init_with_buffer(&pkt, c->pkt_buf);
    mpp_packet_set_length(pkt, 0);  // 必须清零
    mpp_meta_set_packet(mpp_frame_get_meta(mf), KEY_OUTPUT_PACKET, pkt);

    bool ok = true;
    if (c->api->encode_put_frame(c->ctx, mf) != MPP_OK) {
        err = "提交编码帧失败";
        ok = false;
    }
    mpp_frame_deinit(&mf);

    MppPacket got = nullptr;
    if (ok) {
        if (c->api->encode_get_packet(c->ctx, &got) != MPP_OK || got == nullptr) {
            err = "等硬件编码超时（" + std::to_string(c->opts.timeout_ms) + " ms）";
            ok = false;
        }
    }
    if (ok) {
        const auto *p = static_cast<const std::uint8_t *>(mpp_packet_get_pos(got));
        const std::size_t n = mpp_packet_get_length(got);
        out.data.assign(p, p + n);
        out.keyframe = has_idr(out.data.data(), out.data.size());
        // 关键帧前面要有 SPS/PPS（RTSP 中途加入、录像从这里开始才能解码）。板子上的 MPP 1.0.x 设了
        // “每个 IDR 都带头”也不是每次都加（它内部的“已加过”标志没清掉时就跳过），没有就自己补上 open() 时取到的那份
        if (out.keyframe && extract_parameter_sets(out.data.data(), out.data.size()).empty()) {
            out.data.insert(out.data.begin(), c->sps_pps.begin(), c->sps_pps.end());
            ++c->headers_added;
        }
        out.pts_us = pts_us;
        mpp_packet_deinit(&got);
    } else {
        mpp_packet_deinit(&pkt);
    }
    mpp_buffer_put(in_buf);  // 输入 DMA-BUF 的登记释放；底层内存还属于解码器
    return ok;
}

}  // namespace rksec::media
