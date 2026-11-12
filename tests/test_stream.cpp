// RTSP 实时流（第 8c 步）：账号文件、关键帧门，以及一个真的回环测试——
// 程序里起 RTSP 服务，再用 GStreamer 的 rtspsrc 连上去收几帧；密码错要被拒绝。
// 回环测试要 GStreamer 插件（app、rtp、rtsp、videoparsers），没有就跳过并说明。
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

#include "data/tiny_h264.hpp"
#include "media/h264_stream.hpp"
#include "stream/credentials.hpp"
#include "stream/keyframe_gate.hpp"
#include "stream/rtsp_server.hpp"

using rksec::stream::Credentials;
using rksec::stream::KeyframeGate;
using rksec::stream::RtspServer;

namespace fs = std::filesystem;

// ---------------------------------------------------------------- 账号文件

TEST(RtspCredentials, ParsesOneAccount)
{
    Credentials c;
    std::string err;
    ASSERT_TRUE(rksec::stream::parse_credentials("# 注释\n\nviewer:secret-123\n", c, err)) << err;
    EXPECT_EQ(c.user, "viewer");
    EXPECT_EQ(c.password, "secret-123");
    ASSERT_TRUE(rksec::stream::parse_credentials("a.b_c-d:pa:ss:word\n", c, err)) << "密码里可以有冒号";
    EXPECT_EQ(c.password, "pa:ss:word");
}

TEST(RtspCredentials, RejectsBadContent)
{
    Credentials c;
    std::string err;
    for (const char *bad : {"", "# 只有注释\n", "viewer\n", "viewer:short\n", ":password123\n", "vi ewer:password123\n",
                            "viewer:pass word123\n", "a:password123\nb:password123\n",
                            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:password123\n"}) {
        EXPECT_FALSE(rksec::stream::parse_credentials(bad, c, err)) << "应该拒绝：" << bad;
    }
}

TEST(RtspCredentials, FileMustBeOwnerOnly)
{
    const fs::path dir = fs::temp_directory_path() / ("rksec_cred_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const std::string path = (dir / "cred").string();
    {
        std::ofstream(path) << "viewer:password123\n";
    }
    Credentials c;
    std::string err;
    ::chmod(path.c_str(), 0644);
    EXPECT_FALSE(rksec::stream::load_credentials(path, c, err));
    EXPECT_NE(err.find("chmod 600"), std::string::npos) << err;
    ::chmod(path.c_str(), 0600);
    EXPECT_TRUE(rksec::stream::load_credentials(path, c, err)) << err;
    EXPECT_EQ(c.user, "viewer");
    EXPECT_FALSE(rksec::stream::load_credentials((dir / "nope").string(), c, err));
    EXPECT_FALSE(rksec::stream::load_credentials(dir.string(), c, err)) << "目录不是普通文件";
    fs::remove_all(dir);
}

// ---------------------------------------------------------------- 关键帧门

TEST(KeyframeGate, WaitsForKeyframeAfterReset)
{
    KeyframeGate g;
    EXPECT_FALSE(g.pass(false));
    EXPECT_FALSE(g.pass(false));
    EXPECT_TRUE(g.pass(true));
    EXPECT_TRUE(g.pass(false)) << "开了以后 P 帧也放行";
    g.reset();
    EXPECT_FALSE(g.pass(false));
    EXPECT_TRUE(g.pass(true));
    EXPECT_EQ(g.skipped(), 3U);
}

// ---------------------------------------------------------------- 回环

namespace {

bool have_plugins(std::string &missing)
{
    // 测试里的 RTSP 客户端（rtspsrc）连本机，不需要查系统代理。不设的话 GIO 会临时加载 libproxy 的后端、用完卸载，
    // TSan 不允许忽略清单里的库被卸载（会直接退出）
    ::setenv("GIO_USE_PROXY_RESOLVER", "dummy", 1);
    gst_init(nullptr, nullptr);
    for (const char *name : {"appsrc", "h264parse", "rtph264pay", "rtspsrc", "rtph264depay", "appsink"}) {
        GstElementFactory *f = gst_element_factory_find(name);
        if (f == nullptr) {
            missing = name;
            return false;
        }
        gst_object_unref(f);
    }
    return true;
}

std::vector<rksec::media::EncodedPacket> tiny_frames()
{
    const std::uint8_t *d = rksec::testdata::kTinyH264;
    const std::size_t n = sizeof(rksec::testdata::kTinyH264);
    const auto nals = rksec::media::split_annexb(d, n);
    std::vector<rksec::media::EncodedPacket> out;
    std::size_t begin = 0;
    for (std::size_t i = 0; i < nals.size(); ++i) {
        if (nals[i].type != rksec::media::kNalSlice && nals[i].type != rksec::media::kNalIdr) {
            continue;
        }
        std::size_t e = i + 1 < nals.size() ? nals[i + 1].start - 3 : n;
        while (e > begin && d[e - 1] == 0) {
            --e;
        }
        rksec::media::EncodedPacket p;
        p.data.assign(d + begin, d + e);
        p.keyframe = nals[i].type == rksec::media::kNalIdr;
        out.push_back(std::move(p));
        begin = e;
    }
    return out;
}

// 像编码线程一样，按 25 fps 一直推测试码流
class Pusher {
  public:
    explicit Pusher(RtspServer &s)
        : thread_([this, &s] {
              const auto frames = tiny_frames();
              for (std::size_t i = 0; !stop_; ++i) {
                  s.push(frames[i % frames.size()]);
                  std::this_thread::sleep_for(std::chrono::milliseconds(40));
              }
          })
    {
    }
    ~Pusher()
    {
        stop_ = true;
        thread_.join();
    }
    Pusher(const Pusher &) = delete;
    Pusher &operator=(const Pusher &) = delete;

  private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

struct ClientResult {
    int samples = 0;
    bool error = false;
    std::string message;
};

// 连上去收最多 want 帧，最多等 seconds 秒
ClientResult watch(int port, const std::string &user, const std::string &password, int want, int seconds)
{
    ClientResult r;
    const std::string launch = "rtspsrc location=rtsp://127.0.0.1:" + std::to_string(port) +
                               "/live protocols=tcp latency=0 user-id=" + user + " user-pw=" + password +
                               " ! rtph264depay ! h264parse ! appsink name=sink sync=false";
    GError *gerr = nullptr;
    GstElement *pipe = gst_parse_launch(launch.c_str(), &gerr);
    if (pipe == nullptr) {
        r.error = true;
        r.message = gerr != nullptr ? gerr->message : "gst_parse_launch 失败";
        g_clear_error(&gerr);
        return r;
    }
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");
    GstBus *bus = gst_element_get_bus(pipe);
    gst_element_set_state(pipe, GST_STATE_PLAYING);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (r.samples < want && std::chrono::steady_clock::now() < deadline) {
        if (GstMessage *m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR); m != nullptr) {
            GError *e = nullptr;
            gst_message_parse_error(m, &e, nullptr);
            r.error = true;
            r.message = e != nullptr ? e->message : "";
            g_clear_error(&e);
            gst_message_unref(m);
            break;
        }
        if (GstSample *s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 200 * GST_MSECOND); s != nullptr) {
            ++r.samples;
            gst_sample_unref(s);
        }
    }
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(sink);
    gst_object_unref(pipe);
    return r;
}

std::unique_ptr<RtspServer> start_server(std::string &err)
{
    rksec::stream::RtspOptions o;
    o.address = "127.0.0.1";
    o.port = 0;  // 系统随便给一个空闲端口
    o.credentials = {"viewer", "password123"};
    return RtspServer::start(o, err);
}

bool wait_for(const std::function<bool()> &cond, int seconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return cond();
}

}  // namespace

TEST(RtspServer, RefusesToStartWithoutPassword)
{
    std::string missing;
    if (!have_plugins(missing)) {
        GTEST_SKIP() << "没有 GStreamer 插件 " << missing;
    }
    rksec::stream::RtspOptions o;
    o.address = "127.0.0.1";
    o.port = 0;
    std::string err;
    EXPECT_FALSE(RtspServer::start(o, err));
    EXPECT_NE(err.find("账号"), std::string::npos) << err;
}

TEST(RtspServer, ViewerReceivesFramesStartingAtKeyframe)
{
    std::string missing;
    if (!have_plugins(missing)) {
        GTEST_SKIP() << "没有 GStreamer 插件 " << missing;
    }
    std::string err;
    auto server = start_server(err);
    ASSERT_TRUE(server) << err;
    ASSERT_GT(server->bound_port(), 0);
    EXPECT_EQ(server->clients(), 0);
    Pusher pusher{*server};
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(server->stats().pushed, 0U) << "没人看时不推";
    EXPECT_GT(server->stats().no_viewer, 0U);

    const ClientResult r = watch(server->bound_port(), "viewer", "password123", 10, 15);
    EXPECT_FALSE(r.error) << r.message;
    EXPECT_GE(r.samples, 10) << "应该收到画面";
    const auto st = server->stats();
    EXPECT_GE(st.pushed, 10U);
    EXPECT_EQ(st.sessions, 1U);
    EXPECT_GE(st.clients_total, 1U);
    EXPECT_TRUE(wait_for([&] { return server->clients() == 0; }, 10)) << "客户端走了，计数回到 0";
}

TEST(RtspServer, WrongPasswordIsRejected)
{
    std::string missing;
    if (!have_plugins(missing)) {
        GTEST_SKIP() << "没有 GStreamer 插件 " << missing;
    }
    std::string err;
    auto server = start_server(err);
    ASSERT_TRUE(server) << err;
    Pusher pusher{*server};
    const ClientResult r = watch(server->bound_port(), "viewer", "wrong-password", 1, 10);
    EXPECT_EQ(r.samples, 0) << "密码错不能看到画面";
    EXPECT_TRUE(r.error) << "客户端应该收到错误（401 Unauthorized）";
    EXPECT_EQ(server->stats().pushed, 0U) << "没通过认证，流水线都不应该建";
}

TEST(RtspServer, TwoViewersShareOnePipeline)
{
    // 第二个人在第一个人已经在看的时候加入，两个人共用一条流水线。
    // 已知问题（待查，见未完成清单）：两个客户端在几十毫秒内同时连上、机器又很忙（ASan + CPU 占满）时，
    // 偶发（约 1%）两个都只收到一两帧就卡住，服务端一直在推，卡在 rtsp-server 内部。所以这里让第二个人等
    // 第一个人的流水线开始推流以后再连。复现：ASan 构建，CPU 占满，并行跑上百次“同时连接”的版本
    std::string missing;
    if (!have_plugins(missing)) {
        GTEST_SKIP() << "没有 GStreamer 插件 " << missing;
    }
    std::string err;
    auto server = start_server(err);
    ASSERT_TRUE(server) << err;
    Pusher pusher{*server};
    ClientResult a;
    ClientResult b;
    // 第一个人收 75 帧（约 3 秒），保证第二个人连上时他还在看
    std::thread ta([&] { a = watch(server->bound_port(), "viewer", "password123", 75, 20); });
    ASSERT_TRUE(wait_for([&] { return server->stats().pushed >= 5; }, 15)) << "第一个人的流水线没开始推流";
    std::thread tb([&] { b = watch(server->bound_port(), "viewer", "password123", 10, 15); });
    ta.join();
    tb.join();
    const auto st = server->stats();
    const std::string diag = "（服务端：推出 " + std::to_string(st.pushed) + "，没人看 " +
                             std::to_string(st.no_viewer) + "，等关键帧 " + std::to_string(st.waiting_key) +
                             "，积压扔 " + std::to_string(st.congested) + "，流水线 " + std::to_string(st.sessions) +
                             " 次）";
    EXPECT_GE(a.samples, 75) << a.message << diag;
    EXPECT_GE(b.samples, 10) << b.message << diag;
    EXPECT_EQ(st.sessions, 1U) << "共用一条流水线（shared）" << diag;
    EXPECT_GE(st.clients_total, 2U) << diag;
}
