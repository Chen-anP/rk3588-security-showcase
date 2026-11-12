#include "stream/rtsp_server.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "core/log.hpp"
#include "stream/keyframe_gate.hpp"

namespace rksec::stream {

namespace {

constexpr guint64 kMaxQueuedBytes = guint64{2} * 1024 * 1024;  // appsrc 里最多积压多少（4 Mbps 约 4 秒）

// rtph264pay 的 config-interval = -1：每个关键帧前都带 SPS/PPS（我们的码流本来就带，这里保险）
constexpr const char *kLaunch = "( appsrc name=src is-live=true format=time do-timestamp=true min-latency=0 "
                                "caps=video/x-h264,stream-format=byte-stream,alignment=au "
                                "! h264parse config-interval=-1 ! rtph264pay name=pay0 pt=96 config-interval=-1 )";

}  // namespace

namespace {

// GStreamer 回调要用的状态。回调不只在我们的事件循环线程里跑：rtsp-server 把每个客户端放在自己线程池的线程里，
// “客户端断开”可能在服务器对象析构之后才到（第 8c 步 TSan 抓到：析构释放了内存，池里的线程还在 --clients）。
// 所以回调拿的是 shared_ptr 的一份拷贝：最后一个回调结束前，这块内存一直在
struct Shared {
    std::mutex mutex;              // 保护 appsrc、gate、counts
    GstElement *appsrc = nullptr;  // 有客户端时才有（我们持有一个引用）
    KeyframeGate gate;
    RtspServer::Stats counts;
    std::atomic<int> clients{0};

    ~Shared()
    {
        if (appsrc != nullptr) {
            gst_object_unref(appsrc);
        }
    }
};

using SharedPtr = std::shared_ptr<Shared>;

// TSan 标注。回调的数据在一个线程里准备好，经 GLib 的信号交给 rtsp-server 线程池里的另一个线程用；
// 中间的同步在 GLib 里（它的锁直接用 futex），TSan 看不见，会误报竞争。在交接的两头告诉它这里有同步。
// 不是 TSan 构建时这两个函数什么都不做
#if defined(__SANITIZE_THREAD__)
extern "C" void __tsan_acquire(void *addr);  // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern "C" void __tsan_release(void *addr);  // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
void handoff_release(void *p)
{
    __tsan_release(p);
}
void handoff_acquire(void *p)
{
    __tsan_acquire(p);
}
#else
void handoff_release(void * /*p*/) {}
void handoff_acquire(void * /*p*/) {}
#endif

// 信号回调的 user_data：堆上的一份 shared_ptr，信号断开时（对象销毁）由 GLib 调 release 删掉
gpointer hold(const SharedPtr &s)
{
    auto *p = new SharedPtr(s);
    handoff_release(p);
    handoff_release(s.get());
    return p;
}

void release(gpointer data, GClosure * /*closure*/)
{
    handoff_acquire(data);
    delete static_cast<SharedPtr *>(data);
}

Shared &state(gpointer data)
{
    handoff_acquire(data);
    Shared &s = **static_cast<SharedPtr *>(data);
    handoff_release(data);  // 读完了：之后 GLib 在别的线程里删它（release()）要排在这之后
    handoff_acquire(&s);
    return s;
}

// 流水线不用了（最后一个客户端走了）
void on_unprepared(GstRTSPMedia * /*media*/, gpointer data)
{
    Shared &s = state(data);
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (s.appsrc != nullptr) {
        gst_object_unref(s.appsrc);
        s.appsrc = nullptr;
    }
}

// 第一个客户端连上，流水线建好了：拿到 appsrc
void on_media_configure(GstRTSPMediaFactory * /*factory*/, GstRTSPMedia *media, gpointer data)
{
    Shared &s = state(data);
    GstElement *element = gst_rtsp_media_get_element(media);
    GstElement *src = gst_bin_get_by_name(GST_BIN(element), "src");
    gst_object_unref(element);
    if (src == nullptr) {
        LOG_E("rtsp", "流水线里找不到 appsrc");
        return;
    }
    // 实时源不要“按时间戳控制发送速度”（rate-control，默认开）。开着时 TCP 客户端的发送端会按时间戳等到“该发的时刻”；
    // 两个客户端几乎同时连上时，流水线在 PAUSED / PLAYING 之间切换，appsrc 按推入时刻打的时间戳和时钟对不上，
    // 发送端一直等，客户端只收到一两帧就卡住（第 8c 步板子上 ASan 偶发失败，容器里加压复现）。UDP 的发送端本来就不等
    for (guint i = 0; i < gst_rtsp_media_n_streams(media); ++i) {
        gst_rtsp_stream_set_rate_control(gst_rtsp_media_get_stream(media, i), FALSE);
    }
    g_signal_connect_data(media, "unprepared", G_CALLBACK(on_unprepared), hold(*static_cast<SharedPtr *>(data)),
                          release, static_cast<GConnectFlags>(0));
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (s.appsrc != nullptr) {
        gst_object_unref(s.appsrc);
    }
    s.appsrc = src;  // gst_bin_get_by_name 给的引用归我们
    s.gate.reset();  // 新流水线从关键帧开始
    ++s.counts.sessions;
}

void on_client_closed(GstRTSPClient * /*client*/, gpointer data)
{
    Shared &s = state(data);
    const int now = --s.clients;
    LOG_I("rtsp", "客户端断开，现在 %d 个", now);
}

void on_client_connected(GstRTSPServer * /*server*/, GstRTSPClient *client, gpointer data)
{
    Shared &s = state(data);
    const int now = ++s.clients;
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        ++s.counts.clients_total;
    }
    GstRTSPConnection *conn = gst_rtsp_client_get_connection(client);
    const gchar *ip = conn != nullptr ? gst_rtsp_connection_get_ip(conn) : nullptr;
    LOG_I("rtsp", "客户端连上：%s，现在 %d 个", ip != nullptr ? ip : "?", now);
    g_signal_connect_data(client, "closed", G_CALLBACK(on_client_closed), hold(*static_cast<SharedPtr *>(data)),
                          release, static_cast<GConnectFlags>(0));
}

}  // namespace

struct RtspServer::Impl {
    RtspOptions opts;
    SharedPtr shared = std::make_shared<Shared>();
    GMainContext *context = nullptr;
    GMainLoop *loop = nullptr;
    GstRTSPServer *server = nullptr;
    guint source_id = 0;
    std::thread thread;
    int port = 0;

    ~Impl()
    {
        if (loop != nullptr) {
            g_main_loop_quit(loop);
        }
        if (thread.joinable()) {
            thread.join();
        }
        if (server != nullptr) {
            // 断开所有客户端，再拆服务器
            gst_rtsp_server_client_filter(
                server,
                [](GstRTSPServer *, GstRTSPClient *, gpointer) -> GstRTSPFilterResult {
                    return GST_RTSP_FILTER_REMOVE;
                },
                nullptr);
            g_object_unref(server);
        }
        if (source_id != 0 && context != nullptr) {
            if (GSource *src = g_main_context_find_source_by_id(context, source_id); src != nullptr) {
                g_source_destroy(src);
            }
        }
        // 等 rtsp-server 线程池里处理客户端的线程都结束（之后不会再有回调）
        gst_rtsp_thread_pool_cleanup();
        if (loop != nullptr) {
            g_main_loop_unref(loop);
        }
        if (context != nullptr) {
            g_main_context_unref(context);
        }
        const std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->appsrc != nullptr) {
            gst_object_unref(shared->appsrc);
            shared->appsrc = nullptr;
        }
    }
};

RtspServer::RtspServer() : impl_(std::make_unique<Impl>()) {}
RtspServer::~RtspServer() = default;

std::unique_ptr<RtspServer> RtspServer::start(const RtspOptions &opts, std::string &err)
{
    static std::once_flag init;
    std::call_once(init, [] { gst_init(nullptr, nullptr); });
    for (const char *name : {"appsrc", "h264parse", "rtph264pay"}) {
        GstElementFactory *f = gst_element_factory_find(name);
        if (f == nullptr) {
            err = std::string{"缺少 GStreamer 插件 "} + name +
                  "（sudo apt install gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad）";
            return nullptr;
        }
        gst_object_unref(f);
    }
    if (opts.credentials.user.empty() || opts.credentials.password.empty()) {
        err = "RTSP 没有账号密码，不开（不提供不要密码的流）";
        return nullptr;
    }

    std::unique_ptr<RtspServer> s{new RtspServer()};
    Impl &m = *s->impl_;
    m.opts = opts;
    m.context = g_main_context_new();
    m.loop = g_main_loop_new(m.context, FALSE);
    m.server = gst_rtsp_server_new();
    gst_rtsp_server_set_address(m.server, opts.address.c_str());
    gst_rtsp_server_set_service(m.server, std::to_string(opts.port).c_str());

    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_launch(factory, kLaunch);
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    g_signal_connect_data(factory, "media-configure", G_CALLBACK(on_media_configure), hold(m.shared), release,
                          static_cast<GConnectFlags>(0));

    // Digest 认证：只有这个账号能看
    GstRTSPAuth *auth = gst_rtsp_auth_new();
    gst_rtsp_auth_set_supported_methods(auth, GST_RTSP_AUTH_DIGEST);
    GstRTSPToken *token =
        gst_rtsp_token_new(GST_RTSP_TOKEN_MEDIA_FACTORY_ROLE, G_TYPE_STRING, "viewer", static_cast<char *>(nullptr));
    gst_rtsp_auth_add_digest(auth, opts.credentials.user.c_str(), opts.credentials.password.c_str(), token);
    gst_rtsp_token_unref(token);
    gst_rtsp_server_set_auth(m.server, auth);
    g_object_unref(auth);
    gst_rtsp_media_factory_add_role(factory, "viewer", GST_RTSP_PERM_MEDIA_FACTORY_ACCESS, G_TYPE_BOOLEAN, TRUE,
                                    GST_RTSP_PERM_MEDIA_FACTORY_CONSTRUCT, G_TYPE_BOOLEAN, TRUE,
                                    static_cast<char *>(nullptr));

    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(m.server);
    gst_rtsp_mount_points_add_factory(mounts, opts.path.c_str(), factory);  // factory 的引用交给 mounts
    g_object_unref(mounts);
    g_signal_connect_data(m.server, "client-connected", G_CALLBACK(on_client_connected), hold(m.shared), release,
                          static_cast<GConnectFlags>(0));

    m.source_id = gst_rtsp_server_attach(m.server, m.context);
    if (m.source_id == 0) {
        err = "RTSP 端口 " + opts.address + ":" + std::to_string(opts.port) + " 打不开（被占用？）";
        return nullptr;
    }
    m.port = gst_rtsp_server_get_bound_port(m.server);
    m.thread = std::thread([&m] {
        pthread_setname_np(pthread_self(), "rtsp");
        g_main_context_push_thread_default(m.context);
        g_main_loop_run(m.loop);
        g_main_context_pop_thread_default(m.context);
    });
    return s;
}

void RtspServer::push(const media::EncodedPacket &p)
{
    Shared &m = *impl_->shared;
    const std::lock_guard<std::mutex> lock(m.mutex);
    if (m.appsrc == nullptr) {
        ++m.counts.no_viewer;
        return;
    }
    auto *src = GST_APP_SRC(m.appsrc);
    if (gst_app_src_get_current_level_bytes(src) > kMaxQueuedBytes) {
        // 网络跟不上：这一段不推了，等下一个关键帧从头来（不花屏，也不越积越多）
        ++m.counts.congested;
        m.gate.reset();
        return;
    }
    if (!m.gate.pass(p.keyframe)) {
        ++m.counts.waiting_key;
        return;
    }
    GstBuffer *buf = gst_buffer_new_allocate(nullptr, p.data.size(), nullptr);
    gst_buffer_fill(buf, 0, p.data.data(), p.data.size());
    if (!p.keyframe) {
        GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
    }
    gst_app_src_push_buffer(src, buf);  // buf 归 appsrc
    ++m.counts.pushed;
}

int RtspServer::bound_port() const
{
    return impl_->port;
}

std::string RtspServer::url() const
{
    return "rtsp://" + impl_->opts.address + ":" + std::to_string(impl_->port) + impl_->opts.path;
}

int RtspServer::clients() const
{
    return impl_->shared->clients.load();
}

RtspServer::Stats RtspServer::stats() const
{
    const std::lock_guard<std::mutex> lock(impl_->shared->mutex);
    return impl_->shared->counts;
}

}  // namespace rksec::stream
