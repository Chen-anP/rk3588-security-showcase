#include "record/mp4_remux.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
}

namespace rksec::record {

namespace {

namespace fs = std::filesystem;

constexpr AVRational kMicro{1, 1000000};

std::string av_error(int code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
    av_strerror(code, buf.data(), buf.size());
    return buf.data();
}

bool fsync_path(const std::string &path, int flags)
{
    const int fd = ::open(path.c_str(), flags | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

// 输入、输出的 libavformat 对象：出错从哪里返回都能收拾干净
struct Contexts {
    AVFormatContext *in = nullptr;
    AVFormatContext *out = nullptr;
    AVPacket *pkt = nullptr;

    ~Contexts()
    {
        av_packet_free(&pkt);
        avformat_close_input(&in);
        if (out != nullptr) {
            if (out->pb != nullptr) {
                avio_closep(&out->pb);
            }
            avformat_free_context(out);
        }
    }
};

// 真正的转换：写 part。成功返回 true
bool remux(const std::string &in_path, const std::string &part, RemuxInfo &info, std::string &err)
{
    Contexts c;
    int rc = avformat_open_input(&c.in, in_path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        err = "打不开录像 " + in_path + "：" + av_error(rc);
        return false;
    }
    rc = avformat_find_stream_info(c.in, nullptr);
    if (rc < 0) {
        err = "读不出录像的格式：" + av_error(rc);
        return false;
    }
    const int video = av_find_best_stream(c.in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0) {
        err = "录像里没有视频";
        return false;
    }
    const AVStream *src = c.in->streams[video];

    rc = avformat_alloc_output_context2(&c.out, nullptr, "mp4", part.c_str());
    if (rc < 0 || c.out == nullptr) {
        err = "创建封装器失败：" + av_error(rc);
        return false;
    }
    AVStream *dst = avformat_new_stream(c.out, nullptr);
    c.pkt = av_packet_alloc();
    if (dst == nullptr || c.pkt == nullptr) {
        err = "内存不够";
        return false;
    }
    rc = avcodec_parameters_copy(dst->codecpar, src->codecpar);
    if (rc < 0) {
        err = "复制视频参数失败：" + av_error(rc);
        return false;
    }
    dst->codecpar->codec_tag = 0;  // 让 MP4 封装器自己填（avc1）
    dst->time_base = src->time_base;
    info.width = src->codecpar->width;
    info.height = src->codecpar->height;

    // O_EXCL 先建出文件、设好权限，再交给 libavformat 打开：文件从出现那一刻就是 0600
    const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        err = "建不了 " + part + "：" + std::strerror(errno);
        return false;
    }
    ::close(fd);
    rc = avio_open(&c.out->pb, part.c_str(), AVIO_FLAG_WRITE);
    if (rc < 0) {
        err = "打不开 " + part + "：" + av_error(rc);
        return false;
    }
    // faststart：写完以后把索引（moov）挪到文件开头（封装器会重新读一遍文件，所以 url 要是真实路径）
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "movflags", "faststart", 0);
    rc = avformat_write_header(c.out, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        err = "写文件头失败：" + av_error(rc);
        return false;
    }

    std::int64_t first = AV_NOPTS_VALUE;
    std::int64_t end_us = 0;
    while (true) {
        rc = av_read_frame(c.in, c.pkt);
        if (rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            if (info.packets == 0) {
                err = "录像里读不出画面：" + av_error(rc);
                return false;
            }
            info.truncated = true;  // 不完整的录像：坏掉的尾巴之前的都导出
            break;
        }
        if (c.pkt->stream_index != video) {
            av_packet_unref(c.pkt);
            continue;
        }
        if (c.pkt->dts == AV_NOPTS_VALUE) {
            c.pkt->dts = c.pkt->pts;
        }
        if (first == AV_NOPTS_VALUE) {
            first = c.pkt->dts;
        }
        const std::int64_t pkt_end = c.pkt->pts + c.pkt->duration - first;
        end_us = std::max(end_us, av_rescale_q(pkt_end, src->time_base, kMicro));
        info.keyframes += (c.pkt->flags & AV_PKT_FLAG_KEY) != 0 ? 1 : 0;
        c.pkt->stream_index = dst->index;
        av_packet_rescale_ts(c.pkt, src->time_base, dst->time_base);
        c.pkt->pos = -1;
        rc = av_interleaved_write_frame(c.out, c.pkt);  // 会 unref
        if (rc < 0) {
            err = "写入失败（磁盘满了？）：" + av_error(rc);
            return false;
        }
        ++info.packets;
    }
    if (info.packets == 0) {
        err = "录像里没有画面";
        return false;
    }
    rc = av_write_trailer(c.out);
    if (rc < 0) {
        err = "写文件尾失败：" + av_error(rc);
        return false;
    }
    rc = avio_closep(&c.out->pb);
    if (rc < 0) {
        err = "关闭文件失败：" + av_error(rc);
        return false;
    }
    info.duration_us = end_us;
    if (!fsync_path(part, O_RDONLY)) {
        err = "fsync 失败：" + std::string{std::strerror(errno)};
        return false;
    }
    return true;
}

}  // namespace

bool remux_to_faststart(const std::string &in, const std::string &out, RemuxInfo &info, std::string &err)
{
    info = {};
    // faststart 会打印一行 “Starting second pass…” 的提示（info 级别），命令行输出里不要它
    if (av_log_get_level() > AV_LOG_WARNING) {
        av_log_set_level(AV_LOG_WARNING);
    }
    std::error_code ec;
    if (fs::exists(out, ec)) {
        err = out + " 已经存在，不覆盖";
        return false;
    }
    const std::string part = out + ".part";
    if (fs::exists(part, ec)) {
        fs::remove(part, ec);  // 上次导出到一半失败留下的
    }
    if (!remux(in, part, info, err)) {
        fs::remove(part, ec);
        return false;
    }
    // link 而不是 rename：目标在这期间被别人建出来时失败，不会悄悄覆盖
    if (::link(part.c_str(), out.c_str()) != 0) {
        err = "改名成 " + out + " 失败：" + std::strerror(errno);
        fs::remove(part, ec);
        return false;
    }
    fs::remove(part, ec);
    const std::string dir = fs::path{out}.parent_path().string();
    fsync_path(dir.empty() ? "." : dir, O_RDONLY | O_DIRECTORY);
    return true;
}

}  // namespace rksec::record
