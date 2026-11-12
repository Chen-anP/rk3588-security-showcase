// ASan / TSan 的忽略清单（第 8c 步）。只在 sanitizer 构建里起作用（运行库启动时来取），平时这两个函数没人调用。
//
// 只忽略第三方库自己的问题，不忽略“调用栈里经过第三方库”的报告——
// 我们自己的回调也是从 GLib 里被调用的，按调用栈忽略会把自己的错误也藏起来（第 8c 步 TSan 抓到的回调竞争就是这样）。
//
// - LSan：rtspsrc（测试里的 RTSP 客户端）的抖动缓冲在 GStreamer 1.20 里销毁时漏几十字节，库内部的问题
// - TSan：glib / gio / gobject / GStreamer 没有用 TSan 编译，它们内部用自己的原子操作和锁，TSan 看不见，
//   会把库内部的访问报成竞争。called_from_lib：这些库里发起的访问不检查（我们代码里的访问照样检查）
extern "C" {

// ASan 默认用帧指针快速记录“内存是在哪分配的”。Ubuntu 的 glib 编译时去掉了帧指针，调用栈在 glib 那里就断了
// （CI 上只剩 malloc ← g_malloc 两层），下面按 ld-linux 写的忽略规则就匹配不上。改成按 DWARF 完整展开
// （fast_unwind_on_malloc=0）。容器里实测全部 ASan 测试的耗时没有变化
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)：名字是 sanitizer 运行库规定的
__attribute__((used)) const char *__asan_default_options()
{
    return "fast_unwind_on_malloc=0";
}

// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
__attribute__((used)) const char *__lsan_default_suppressions()
{
    return "leak:gst_rtp_jitter_buffer_finalize\n"
           // 共享库加载时（动态链接器调库的构造函数）一次性的全局分配，比如板子上 glib 2.72 加载时的 16 KB。
           // 只匹配调用栈里有动态链接器的：我们的代码直接链进测试程序，不会在它下面运行
           "leak:ld-linux-aarch64.so.1\n"
           "leak:ld-linux-x86-64.so.2\n";
}

// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
__attribute__((used)) const char *__tsan_default_suppressions()
{
    return "called_from_lib:libglib-2.0.so\n"
           "called_from_lib:libgobject-2.0.so\n"
           "called_from_lib:libgio-2.0.so\n"
           "called_from_lib:libgstreamer-1.0.so\n"
           "called_from_lib:libgstbase-1.0.so\n"
           "called_from_lib:libgstapp-1.0.so\n"
           "called_from_lib:libgstrtsp-1.0.so\n"
           "called_from_lib:libgstrtp-1.0.so\n"
           "called_from_lib:libgstsdp-1.0.so\n"
           "called_from_lib:libgstnet-1.0.so\n"
           "called_from_lib:libgstrtspserver-1.0.so\n"
           "called_from_lib:libgstvideo-1.0.so\n"
           "called_from_lib:libgstcodecparsers-1.0.so\n"
           "called_from_lib:libgstpbutils-1.0.so\n"
           "called_from_lib:libgstapp.so\n"
           "called_from_lib:libgstcoreelements.so\n"
           "called_from_lib:libgstrtp.so\n"
           "called_from_lib:libgstrtpmanager.so\n"
           "called_from_lib:libgstrtsp.so\n"
           "called_from_lib:libgstudp.so\n"
           "called_from_lib:libgstvideoparsersbad.so\n";
}

}  // extern "C"
