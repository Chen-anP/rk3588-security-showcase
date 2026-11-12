// MPP 硬件 H.264 编码（第 8a、8b 步）。只在编译了 MPP、而且有 /dev/mpp_service 的板子上跑，其他地方跳过。
//
// 第 8b 步上板发现录像颜色错：输入是 NV16（4:2:2），编码器却按登记的 NV12 读，只读到上半幅的颜色。
// 这里造一帧“上半偏红、下半偏蓝”的画面，编码后用 libavcodec 解回来，看上下两半的颜色还对不对。
#include <gtest/gtest.h>

#ifdef RKSEC_HAVE_MPP

#include <unistd.h>

#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "media/dma_buffer.hpp"
#include "media/h264_stream.hpp"
#include "media/mpp_h264_encoder.hpp"

using namespace rksec::media;

namespace {

constexpr int kW = 1280;
constexpr int kH = 720;
constexpr std::uint8_t kRedU = 90;  // 偏红：U 小、V 大
constexpr std::uint8_t kRedV = 200;
constexpr std::uint8_t kBlueU = 200;  // 偏蓝：U 大、V 小
constexpr std::uint8_t kBlueV = 90;

bool have_device()
{
    return ::access("/dev/mpp_service", R_OK | W_OK) == 0 && ::access("/dev/dma_heap/system", R_OK | W_OK) == 0;
}

// 造一帧：Y 全 128；色度上半偏红、下半偏蓝
std::unique_ptr<DmaBuffer> make_frame(YuvFormat fmt, YuvLayout &layout, std::string &err)
{
    layout.format = fmt;
    layout.width = kW;
    layout.height = kH;
    layout.hstride = kW;
    layout.vstride = kH;
    auto buf = DmaBuffer::allocate(layout.total_bytes(), err);
    if (!buf) {
        return nullptr;
    }
    buf->begin_cpu_write();
    std::memset(buf->data(), 128, layout.y_bytes());
    const int uv_rows = fmt == YuvFormat::nv12 ? kH / 2 : kH;
    for (int r = 0; r < uv_rows; ++r) {
        std::uint8_t *row = buf->data() + layout.uv_offset() + static_cast<std::size_t>(r) * kW;
        const bool top = r < uv_rows / 2;
        for (int x = 0; x < kW; x += 2) {
            row[x] = top ? kRedU : kBlueU;
            row[x + 1] = top ? kRedV : kBlueV;
        }
    }
    buf->end_cpu_write();
    return buf;
}

struct Avg {
    double u_top = 0;
    double v_top = 0;
    double u_bottom = 0;
    double v_bottom = 0;
    bool ok = false;
};

// 用 libavcodec 解第一帧，取上四分之一和下四分之一中间一块的平均色度（避开上下交界）
Avg decode_and_measure(const std::vector<std::uint8_t> &stream)
{
    Avg a;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
    if (ctx == nullptr || avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return a;
    }
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    pkt->data = const_cast<std::uint8_t *>(stream.data());
    pkt->size = static_cast<int>(stream.size());
    avcodec_send_packet(ctx, pkt);
    avcodec_send_packet(ctx, nullptr);  // 没有更多数据：把手上的帧吐出来
    if (avcodec_receive_frame(ctx, frame) == 0 && frame->format == AV_PIX_FMT_YUV420P) {
        const int cw = frame->width / 2;
        const int ch = frame->height / 2;
        const auto mean = [&](int plane, int y0, int y1) {
            double sum = 0;
            int n = 0;
            for (int y = y0; y < y1; ++y) {
                const std::uint8_t *row = frame->data[plane] + static_cast<std::ptrdiff_t>(y) * frame->linesize[plane];
                for (int x = cw / 4; x < cw * 3 / 4; ++x) {
                    sum += row[x];
                    ++n;
                }
            }
            return n > 0 ? sum / n : 0.0;
        };
        a.u_top = mean(1, ch / 16, ch / 4);
        a.v_top = mean(2, ch / 16, ch / 4);
        a.u_bottom = mean(1, ch * 3 / 4, ch * 15 / 16);
        a.v_bottom = mean(2, ch * 3 / 4, ch * 15 / 16);
        a.ok = true;
    }
    pkt->data = nullptr;
    pkt->size = 0;
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return a;
}

void check_colors(YuvFormat fmt)
{
    std::string err;
    YuvLayout layout;
    auto buf = make_frame(fmt, layout, err);
    ASSERT_TRUE(buf) << err;
    MppH264Encoder enc;
    MppH264Encoder::Options o;
    o.width = kW;
    o.height = kH;
    ASSERT_TRUE(enc.open(o, err)) << err;
    EncodedPacket pkt;
    ASSERT_TRUE(enc.encode(YuvView{layout, buf->data(), buf->fd()}, 0, pkt, err)) << err;
    EXPECT_TRUE(pkt.keyframe);
    EXPECT_FALSE(extract_parameter_sets(pkt.data.data(), pkt.data.size()).empty()) << "第一个关键帧带 SPS/PPS";

    const Avg a = decode_and_measure(pkt.data);
    ASSERT_TRUE(a.ok) << "libavcodec 解不出来";
    constexpr double kTol = 20;
    EXPECT_NEAR(a.u_top, kRedU, kTol) << format_name(fmt) << " 上半";
    EXPECT_NEAR(a.v_top, kRedV, kTol) << format_name(fmt) << " 上半";
    EXPECT_NEAR(a.u_bottom, kBlueU, kTol) << format_name(fmt) << " 下半：按错格式读，会变成上半的颜色";
    EXPECT_NEAR(a.v_bottom, kBlueV, kTol) << format_name(fmt) << " 下半";
}

}  // namespace

TEST(MppH264Encoder, Nv16ColorsSurviveEncoding)
{
    if (!have_device()) {
        GTEST_SKIP() << "没有 /dev/mpp_service 或 /dev/dma_heap/system";
    }
    check_colors(YuvFormat::nv16);
}

TEST(MppH264Encoder, Nv12ColorsSurviveEncoding)
{
    if (!have_device()) {
        GTEST_SKIP() << "没有 /dev/mpp_service 或 /dev/dma_heap/system";
    }
    check_colors(YuvFormat::nv12);
}

#else

TEST(MppH264Encoder, NotBuiltWithoutMpp)
{
    GTEST_SKIP() << "这次编译没有 MPP（CI、电脑上正常）";
}

#endif
