#include "record/mp4_writer.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

namespace rksec::record {

namespace {

constexpr AVRational kMicro{1, 1000000};
constexpr std::int64_t kDefaultFrameUs = 40000;  // 还不知道帧间隔时按 25 fps 算最后一帧的时长

std::string av_error(int code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
    av_strerror(code, buf.data(), buf.size());
    return buf.data();
}

}  // namespace

struct Mp4Writer::Impl {
    AVFormatContext *ctx = nullptr;
    AVStream *stream = nullptr;
    AVPacket *pkt = nullptr;
    std::string path;
    bool header_written = false;
    bool closed = false;
    std::int64_t first_pts_us = -1;
    std::int64_t last_pts_us = -1;
    std::int64_t frame_us = kDefaultFrameUs;  // 最近两帧的间隔，给最后一帧当时长

    ~Impl()
    {
        if (!closed && ctx != nullptr) {
            if (header_written) {
                av_write_trailer(ctx);
            }
            if (ctx->pb != nullptr) {
                avio_closep(&ctx->pb);
            }
        }
        av_packet_free(&pkt);
        avformat_free_context(ctx);
    }
};

Mp4Writer::Mp4Writer() : impl_(std::make_unique<Impl>()) {}
Mp4Writer::~Mp4Writer() = default;

std::unique_ptr<Mp4Writer> Mp4Writer::open(const std::string &path, const Options &opts, std::string &err)
{
    std::unique_ptr<Mp4Writer> w{new Mp4Writer()};
    Impl &m = *w->impl_;
    m.path = path;
    if (opts.sps_pps.empty() || opts.width <= 0 || opts.height <= 0) {
        err = "MP4：缺少 SPS/PPS 或画面尺寸";
        return nullptr;
    }
    int rc = avformat_alloc_output_context2(&m.ctx, nullptr, "mp4", path.c_str());
    if (rc < 0 || m.ctx == nullptr) {
        err = "MP4：创建封装器失败：" + av_error(rc);
        return nullptr;
    }
    m.stream = avformat_new_stream(m.ctx, nullptr);
    m.pkt = av_packet_alloc();
    if (m.stream == nullptr || m.pkt == nullptr) {
        err = "MP4：内存不够";
        return nullptr;
    }
    AVCodecParameters *par = m.stream->codecpar;
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = AV_CODEC_ID_H264;
    par->width = opts.width;
    par->height = opts.height;
    par->format = AV_PIX_FMT_YUV420P;
    par->extradata = static_cast<std::uint8_t *>(av_mallocz(opts.sps_pps.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (par->extradata == nullptr) {
        err = "MP4：内存不够";
        return nullptr;
    }
    std::memcpy(par->extradata, opts.sps_pps.data(), opts.sps_pps.size());
    par->extradata_size = static_cast<int>(opts.sps_pps.size());
    m.stream->time_base = AVRational{1, 90000};

    rc = avio_open(&m.ctx->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (rc < 0) {
        err = "MP4：打不开 " + path + "：" + av_error(rc);
        return nullptr;
    }
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);  // 录像里有人：只有自己能读写

    // 每写一个包就把 libavformat 自己的写缓冲（32 KB）交给内核。不这样做，进程被杀时缓冲里的内容丢掉，
    // 文件开头的索引都可能没写出去，整个文件打不开（第 8b 步测试发现）。交给内核以后，进程没了数据还在；
    // 断电要靠 close() 里的 fsync
    m.ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    AVDictionary *mux_opts = nullptr;
    av_dict_set(&mux_opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    rc = avformat_write_header(m.ctx, &mux_opts);
    av_dict_free(&mux_opts);
    if (rc < 0) {
        err = "MP4：写文件头失败：" + av_error(rc);
        return nullptr;
    }
    m.header_written = true;  // 写完头以后 stream->time_base 可能被封装器改掉，之后都按它换算
    return w;
}

bool Mp4Writer::write(const media::EncodedPacket &p, std::string &err)
{
    Impl &m = *impl_;
    if (m.closed) {
        err = "MP4：已经关闭";
        return false;
    }
    if (m.first_pts_us < 0) {
        if (!p.keyframe) {
            ++skipped_;
            return true;  // 等第一个关键帧
        }
        m.first_pts_us = p.pts_us;
    }
    std::int64_t rel = p.pts_us - m.first_pts_us;
    const std::int64_t last_rel = m.last_pts_us < 0 ? -1 : m.last_pts_us - m.first_pts_us;
    if (rel <= last_rel) {
        rel = last_rel + 1;  // 时间戳必须递增（采集时间偶尔一样或倒退时顺延 1 微秒）
    }
    if (last_rel >= 0) {
        m.frame_us = rel - last_rel;
    }
    m.last_pts_us = m.first_pts_us + rel;

    AVPacket *pkt = m.pkt;
    av_packet_unref(pkt);
    pkt->data = const_cast<std::uint8_t *>(p.data.data());  // 不带引用计数：libavformat 自己拷一份
    pkt->size = static_cast<int>(p.data.size());
    pkt->stream_index = m.stream->index;
    pkt->pts = av_rescale_q(rel, kMicro, m.stream->time_base);
    pkt->dts = pkt->pts;
    pkt->duration = av_rescale_q(m.frame_us, kMicro, m.stream->time_base);
    pkt->flags = p.keyframe ? AV_PKT_FLAG_KEY : 0;
    const int rc = av_write_frame(m.ctx, pkt);
    pkt->data = nullptr;
    pkt->size = 0;
    if (rc < 0) {
        err = "MP4：写入失败：" + av_error(rc);
        return false;
    }
    ++packets_;
    return true;
}

bool Mp4Writer::close(std::string &err)
{
    Impl &m = *impl_;
    if (m.closed) {
        return true;
    }
    m.closed = true;
    bool ok = true;
    int rc = av_write_trailer(m.ctx);
    if (rc < 0) {
        err = "MP4：写文件尾失败：" + av_error(rc);
        ok = false;
    }
    rc = avio_closep(&m.ctx->pb);
    if (rc < 0 && ok) {
        err = "MP4：关闭文件失败：" + av_error(rc);
        ok = false;
    }
    // 写进磁盘再返回：之后改名、写数据库，断电时数据库说“录完了”的文件一定是完整的
    const int fd = ::open(m.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (::fsync(fd) != 0 && ok) {
            err = "MP4：fsync 失败：" + std::string{std::strerror(errno)};
            ok = false;
        }
        ::close(fd);
    }
    return ok;
}

std::int64_t Mp4Writer::duration_us() const
{
    const Impl &m = *impl_;
    return m.first_pts_us < 0 ? 0 : m.last_pts_us - m.first_pts_us + m.frame_us;
}

}  // namespace rksec::record
