#include "record/event_admin.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace rksec::record {

namespace {

namespace fs = std::filesystem;

void fsync_dir(const std::string &dir)
{
    const int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

// 新建文件写进去（O_EXCL：已经有了就失败，不覆盖），0600，fsync
bool write_new_file(const std::string &path, const std::vector<std::uint8_t> &data, std::string &err)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        err = errno == EEXIST ? path + " 已经存在，不覆盖" : "建不了 " + path + "：" + std::strerror(errno);
        return false;
    }
    std::size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ok = false;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    ok = ok && ::fsync(fd) == 0;
    if (!ok) {
        err = "写 " + path + " 失败：" + std::strerror(errno);
    }
    ::close(fd);
    if (!ok) {
        ::unlink(path.c_str());
    }
    return ok;
}

bool load(EventStore &store, std::int64_t id, EventRow &row, std::string &err)
{
    bool found = false;
    if (!store.get(id, row, found, err)) {
        return false;
    }
    if (!found) {
        err = "没有事件 #" + std::to_string(id) + "（用 --events 看有哪些）";
        return false;
    }
    return true;
}

}  // namespace

const char *status_text(const std::string &status)
{
    if (status == "complete") {
        return "完整";
    }
    if (status == "incomplete") {
        return "不完整";
    }
    if (status == "recording") {
        return "正在录";
    }
    if (status == "deleted") {
        return "已删除";
    }
    if (status == "lost") {
        return "丢失";
    }
    return "未知";
}

std::string event_line(const EventRow &r)
{
    std::array<char, 96> head{};
    std::snprintf(head.data(), head.size(), "#%-4lld %s  %6.1f 秒  %6.1f MB  ", static_cast<long long>(r.id),
                  r.started.c_str(), static_cast<double>(r.duration_ms) / 1000.0,
                  static_cast<double>(r.bytes) / (1024.0 * 1024.0));
    std::string s = head.data();
    s += status_text(r.status);
    s += r.has_snapshot ? "  有截图" : "  无截图";
    s += "  陌生人 " + (r.tracks.empty() ? std::string{"-"} : r.tracks);
    if (r.kind.find("（续）") != std::string::npos) {
        s += "  （上一段的续录）";
    }
    if (!r.file.empty() && r.status != "deleted" && r.status != "lost") {
        s += "  " + fs::path{r.file}.filename().string();
    }
    return s;
}

bool export_event(EventStore &store, std::int64_t id, const std::string &dir, ExportResult &out, std::string &err)
{
    out = {};
    EventRow row;
    if (!load(store, id, row, err)) {
        return false;
    }
    const std::string name = "#" + std::to_string(id);
    if (row.status == "deleted") {
        err = "事件 " + name + " 的录像已经删除了（超过配额或手动删除），没法导出";
        return false;
    }
    if (row.status == "lost") {
        err = "事件 " + name + " 的录像文件丢失了（程序中断时还没来得及建文件），没法导出";
        return false;
    }
    if (row.status == "recording") {
        err = "事件 " + name + " 还在录（或者程序中途被杀了，下次启动时会恢复），录完再导出";
        return false;
    }
    std::error_code ec;
    if (row.file.empty() || !fs::is_regular_file(row.file, ec)) {
        err = "事件 " + name + " 的录像文件不在了：" + row.file;
        return false;
    }
    out.incomplete = row.status == "incomplete";

    if (!fs::exists(dir, ec)) {
        fs::create_directories(dir, ec);
        if (ec) {
            err = "建不了导出目录 " + dir + "：" + ec.message();
            return false;
        }
        ::chmod(dir.c_str(), S_IRWXU);  // 新建的目录：只有自己能进
    } else if (!fs::is_directory(dir, ec)) {
        err = dir + " 不是目录";
        return false;
    }

    const fs::path stem = fs::path{dir} / fs::path{row.file}.stem();
    const std::string video = stem.string() + ".mp4";
    if (!remux_to_faststart(row.file, video, out.info, err)) {
        return false;
    }
    out.video = video;
    out.video_bytes = static_cast<std::int64_t>(fs::file_size(video, ec));

    if (row.has_snapshot) {
        std::vector<std::uint8_t> jpeg;
        const std::string snap = stem.string() + ".jpg";
        if (!store.snapshot(id, jpeg, err) || !write_new_file(snap, jpeg, err)) {
            fs::remove(video, ec);  // 要么全导出，要么什么都不留
            out.video.clear();
            fsync_dir(dir);
            return false;
        }
        out.snapshot = snap;
    }
    fsync_dir(dir);
    return true;
}

bool delete_event(EventStore &store, std::int64_t id, EventRow &row, std::string &err)
{
    if (!load(store, id, row, err)) {
        return false;
    }
    const std::string name = "#" + std::to_string(id);
    if (row.status == "deleted") {
        err = "事件 " + name + " 已经删过了";
        return false;
    }
    if (row.status == "recording") {
        err = "事件 " + name + " 还在录（或者程序中途被杀了，下次启动时会恢复），录完再删";
        return false;
    }
    // 先删文件、再改记录：改记录失败时文件已经没了，下次再删一遍就行（文件不在不算错）
    std::error_code ec;
    if (!row.file.empty()) {
        fs::remove(row.file, ec);
        if (ec) {
            err = "删不了 " + row.file + "：" + ec.message();
            return false;
        }
        fsync_dir(fs::path{row.file}.parent_path().string());
    }
    return store.mark(id, "deleted", err);
}

}  // namespace rksec::record
