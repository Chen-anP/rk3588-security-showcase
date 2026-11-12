#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "app/access_run.hpp"
#include "app/capture_run.hpp"
#include "app/capture_test.hpp"
#include "app/decode_bench.hpp"
#include "app/detect_bench.hpp"
#include "app/encode_run.hpp"
#include "app/event_admin.hpp"
#include "app/face_admin.hpp"
#include "app/face_bench.hpp"
#include "app/face_tool.hpp"
#include "app/hw_test.hpp"
#include "app/pass_admin.hpp"
#include "app/record_run.hpp"
#include "app/rga_bench.hpp"
#include "app/ui_run.hpp"
#include "app/ui_sim.hpp"
#include "core/config.hpp"
#include "core/instance_lock.hpp"
#include "core/log.hpp"
#include "core/version.hpp"

namespace {

// 退出码：脚本和 systemd 靠它判断失败原因
constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;   // 命令行参数错误
constexpr int kExitConfig = 3;  // 配置文件错误
// 1：做不了（人脸登记不合格、事件不存在或状态不对……），见 face_admin.cpp、event_admin.cpp
// 4：设备错误（摄像头打不开、取不到帧、显示设备打不开），见 capture_test.cpp、ui_run.cpp
constexpr int kExitBusy = 5;  // 已经有一个实例在运行（D18）

constexpr const char *kLockFile = "/run/lock/rk3588-security.lock";

constexpr const char *kDefaultConfig = "/etc/rk3588-security/rk3588-security.conf";

void print_usage(const char *prog)
{
    std::printf(
        "用法: %s [选项]\n"
        "  -c, --config 文件   配置文件（默认 %s）\n"
        "      --check-config  只检查配置文件，通过返回 0，有错误返回 %d\n"
        "      --capture-test N  抓 N 帧，报告帧率、帧间隔、延迟、丢帧；设备出错返回 4\n"
        "      --save 文件     和 --capture-test 一起用：把中间一帧存成 JPEG；\n"
        "                      和 --decode-bench 一起用：把第一帧的硬件解码结果存成 PPM；\n"
        "                      和 --rga-bench 一起用：是目录，存 RGA 输出和 CPU 参考图\n"
        "      --decode-bench N  取 N 帧 MJPG，比较 MPP 硬件解码和 CPU 解码的耗时、CPU 占用，检查结果对不对\n"
        "      --rga-bench N   取 N 帧，MPP 解码后用 RGA 做 NPU 输入（letterbox）和预览，测耗时、检查结果\n"
        "      --detect-image 图片  检测一张 JPEG 图片，打印全部框（和官方 demo 对照）；--save 存画了框的 PPM\n"
        "      --detect-bench N  取 N 帧，测解码、RGA、NPU 推理（1 核 / 3 核联合）、后处理的耗时，对比 CPU 预处理\n"
        "      --detect-run 秒数  不接屏幕跑完整的预览 + 检测流水线，报告端到端延迟、每个 NPU 线程的帧数\n"
        "      --workers N     和 --detect-run 一起用：推理线程个数 1～3（覆盖配置文件）\n"
        "      --face-image 图片  人脸检测一张 JPEG 图片，打印框、分数、5 个关键点；--save 存画了框和关键点的 PPM\n"
        "      --face-bench N  取 N 帧，测整幅画面上人脸检测的耗时、每帧几张脸\n"
        "      --face-embed 图片…  每张 JPEG 里最大的脸算特征，打印两两的相似度（定阈值用）；\n"
        "                      --save 目录：存对齐后的脸和特征（和电脑上的 onnxruntime 对照）\n"
        "      --enroll 名字 [照片…]  登记一个人到人脸库：不给照片就用摄像头拍（看着镜头 3 秒），给了照片就用照片\n"
        "      --faces         列出人脸库里登记的人\n"
        "      --unenroll 名字  从人脸库删除一个人\n"
        "      --face-reembed  换了识别模型以后，用库里存的小图重新算所有特征\n"
        "      --passes [条数]  只读查询最近通行记录（默认 20，最多 1000）\n"
        "      --access-run 秒数  无屏运行门禁联动（模拟开门）\n"
        "      --hw-test 秒数  测试红外 GPIO 输入（2～100000 秒），设备错误返回 4\n"
        "      --capture-run 秒数  长时间采集（断线自动重连），每 10 秒报告一次，查泄漏用\n"
        "      --encode-run 秒数 文件  不接屏幕：摄像头 → 硬件解码 → 硬件 H.264 "
        "编码，码流写进文件（.h264），报告编码耗时、码率、关键帧\n"
        "      --record-run 秒数  不接屏幕跑完整流水线 + 事件录像：陌生人出现时录 MP4，结束时列出这次的事件\n"
        "      --events [条数]  列出最新的事件录像（默认 20 条）和录像占用\n"
        "      --event-export 编号 目录  把一个事件的录像（转成普通 MP4，任何播放器都能拖动）和截图导出到目录\n"
        "      --event-delete 编号  删除一个事件的录像和截图（记录保留，标成“已删除”）\n"
        "      --ui            在屏幕上运行界面（摄像头预览、触摸），Ctrl+C 结束；显示设备打不开返回 4\n"
        "      --ui-bench 秒数  不接摄像头和屏幕，只渲染界面（假画面、自动切换页面），报告界面一帧的耗时\n"
        "      --busy N        和 --ui-bench 一起用：开 N 个线程占满 CPU，模拟推理很慢时界面受不受影响\n"
        "  "
        "用摄像头或屏幕的模式（--capture-test、--capture-run、--decode-bench、--rga-bench、--detect-bench、"
        "--detect-run、--encode-run、--record-run、--face-bench、--ui、不给照片的 --enroll）同一时间只能运行一个，\n"
        "  已有实例在运行时返回 %d\n"
        "  -v, --version       显示版本信息\n"
        "  -h, --help          显示本帮助\n",
        prog, kDefaultConfig, kExitConfig, kExitBusy);
}

}  // namespace

int main(int argc, char **argv)
{
    std::string config_path = kDefaultConfig;
    bool check_only = false;
    int hw_seconds = 0;
    int access_seconds = 0;
    int pass_limit = 0;
    int capture_frames = 0;
    int run_seconds = 0;
    std::string save_path;
    bool ui = false;
    int bench_seconds = 0;
    int busy_threads = 0;
    int bench_frames = 0;
    int rga_frames = 0;
    int detect_frames = 0;
    int detect_seconds = 0;
    int workers = 0;
    std::string detect_image_path;
    std::string face_image_path;
    int face_frames = 0;
    std::vector<std::string> embed_paths;
    std::string enroll_name;
    std::vector<std::string> enroll_photos;
    std::string unenroll_name;
    bool list_faces = false;
    bool reembed = false;
    int encode_seconds = 0;
    int record_seconds = 0;
    std::string encode_out;
    int events_limit = 0;  // --events
    std::int64_t export_id = 0;
    std::string export_dir;
    std::int64_t delete_id = 0;

    // 事件编号：正整数
    const auto parse_id = [](const char *text, std::int64_t &id) {
        const std::string_view v{text};
        const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), id);
        return !v.empty() && ec == std::errc{} && ptr == v.data() + v.size() && id > 0;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "-v" || arg == "--version") {
            std::printf("%s\n", rksec::version::full().c_str());
            return kExitOk;
        }
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return kExitOk;
        }
        if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 后面要跟配置文件路径\n", argv[i]);
                return kExitUsage;
            }
            config_path = argv[++i];
            continue;
        }
        if (arg == "--ui") {
            ui = true;
            continue;
        }
        if (arg == "--check-config") {
            check_only = true;
            continue;
        }
        if (arg == "--ui-bench" || arg == "--busy") {
            int n = 0;
            const std::string_view val = i + 1 < argc ? std::string_view{argv[i + 1]} : std::string_view{};
            const auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), n);
            const int lo = arg == "--busy" ? 0 : 1;
            if (val.empty() || ec != std::errc{} || ptr != val.data() + val.size() || n < lo || n > 86400) {
                std::fprintf(stderr, "%s 后面要跟 %d 以上的整数\n", argv[i], lo);
                return kExitUsage;
            }
            ++i;
            (arg == "--busy" ? busy_threads : bench_seconds) = n;
            continue;
        }
        if (arg == "--detect-image" || arg == "--face-image") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 后面要跟图片路径\n", argv[i]);
                return kExitUsage;
            }
            (arg == "--face-image" ? face_image_path : detect_image_path) = argv[i + 1];
            ++i;
            continue;
        }
        if (arg == "--face-embed") {
            // 后面跟一张或几张图片，直到下一个 - 开头的选项
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                embed_paths.emplace_back(argv[++i]);
            }
            if (embed_paths.empty()) {
                std::fprintf(stderr, "--face-embed 后面要跟至少一张图片\n");
                return kExitUsage;
            }
            continue;
        }
        if (arg == "--enroll" || arg == "--unenroll") {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                std::fprintf(stderr, "%s 后面要跟名字\n", argv[i]);
                return kExitUsage;
            }
            if (arg == "--unenroll") {
                unenroll_name = argv[++i];
                continue;
            }
            enroll_name = argv[++i];
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                enroll_photos.emplace_back(argv[++i]);
            }
            continue;
        }
        if (arg == "--faces" || arg == "--face-reembed") {
            (arg == "--faces" ? list_faces : reembed) = true;
            continue;
        }
        if (arg == "--passes") {
            pass_limit = 20;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const std::string_view v{argv[++i]};
                const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), pass_limit);
                if (ec != std::errc{} || ptr != v.data() + v.size() || pass_limit < 1 || pass_limit > 1000) {
                    std::fprintf(stderr, "--passes 条数必须为 1～1000\n");
                    return kExitUsage;
                }
            }
            continue;
        }
        if (arg == "--encode-run") {
            // --encode-run 秒数 输出文件
            int n = 0;
            const std::string_view val = i + 1 < argc ? std::string_view{argv[i + 1]} : std::string_view{};
            const auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), n);
            if (val.empty() || ec != std::errc{} || ptr != val.data() + val.size() || n < 2 || n > 86400 ||
                i + 2 >= argc || argv[i + 2][0] == '-') {
                std::fprintf(stderr,
                             "--encode-run 后面要跟秒数（2～86400）和输出文件，比如 --encode-run 30 /tmp/a.h264\n");
                return kExitUsage;
            }
            encode_seconds = n;
            encode_out = argv[i + 2];
            i += 2;
            continue;
        }
        if (arg == "--events") {
            events_limit = 20;  // 后面的数可以不给
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const std::string_view val{argv[++i]};
                const auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), events_limit);
                if (ec != std::errc{} || ptr != val.data() + val.size() || events_limit < 1 || events_limit > 10000) {
                    std::fprintf(stderr, "--events 后面的条数要是 1～10000\n");
                    return kExitUsage;
                }
            }
            continue;
        }
        if (arg == "--event-export") {
            if (i + 2 >= argc || !parse_id(argv[i + 1], export_id) || argv[i + 2][0] == '-') {
                std::fprintf(stderr, "--event-export 后面要跟事件编号和导出目录，比如 --event-export 12 ~/export\n");
                return kExitUsage;
            }
            export_dir = argv[i + 2];
            i += 2;
            continue;
        }
        if (arg == "--event-delete") {
            if (i + 1 >= argc || !parse_id(argv[i + 1], delete_id)) {
                std::fprintf(stderr, "--event-delete 后面要跟事件编号（--events 里 # 后面的数）\n");
                return kExitUsage;
            }
            ++i;
            continue;
        }
        if (arg == "--workers") {
            int n = 0;
            const std::string_view val = i + 1 < argc ? std::string_view{argv[i + 1]} : std::string_view{};
            const auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), n);
            if (val.empty() || ec != std::errc{} || ptr != val.data() + val.size() || n < 1 || n > 3) {
                std::fprintf(stderr, "--workers 后面要跟 1～3\n");
                return kExitUsage;
            }
            ++i;
            workers = n;
            continue;
        }
        if (arg == "--capture-test" || arg == "--save" || arg == "--capture-run" || arg == "--decode-bench" ||
            arg == "--rga-bench" || arg == "--detect-bench" || arg == "--detect-run" || arg == "--face-bench" ||
            arg == "--record-run" || arg == "--hw-test" || arg == "--access-run") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 后面要跟参数\n", argv[i]);
                return kExitUsage;
            }
            const std::string_view val{argv[++i]};
            if (arg == "--save") {
                save_path = std::string{val};
                continue;
            }
            int n = 0;
            const auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), n);
            if (ec != std::errc{} || ptr != val.data() + val.size() || n < 2 || n > 100000) {
                std::fprintf(stderr, "%s 要跟 2～100000 之间的数，实际是 %s\n", argv[i - 1], argv[i]);
                return kExitUsage;
            }
            if (arg == "--access-run") {
                access_seconds = n;
            } else if (arg == "--hw-test") {
                hw_seconds = n;
            } else if (arg == "--capture-run") {
                run_seconds = n;
            } else if (arg == "--decode-bench") {
                bench_frames = n;
            } else if (arg == "--rga-bench") {
                rga_frames = n;
            } else if (arg == "--detect-bench") {
                detect_frames = n;
            } else if (arg == "--detect-run") {
                detect_seconds = n;
            } else if (arg == "--face-bench") {
                face_frames = n;
            } else if (arg == "--record-run" || arg == "--hw-test") {
                record_seconds = n;
            } else {
                capture_frames = n;
            }
            continue;
        }
        std::fprintf(stderr, "未知选项: %s\n", argv[i]);
        print_usage(argv[0]);
        return kExitUsage;
    }

    if (hw_seconds > 0 &&
        (access_seconds || pass_limit || check_only || ui || capture_frames || run_seconds || bench_seconds ||
         busy_threads || bench_frames || rga_frames || detect_frames || detect_seconds || workers || face_frames ||
         encode_seconds || record_seconds || events_limit || export_id || delete_id || list_faces || reembed ||
         !save_path.empty() || !detect_image_path.empty() || !face_image_path.empty() || !embed_paths.empty() ||
         !enroll_name.empty() || !unenroll_name.empty())) {
        std::fprintf(stderr, "--hw-test 不能与其他运行模式组合\n");
        return kExitUsage;
    }

    if (pass_limit > 0 &&
        (access_seconds || check_only || ui || capture_frames || run_seconds || bench_seconds || busy_threads ||
         bench_frames || rga_frames || detect_frames || detect_seconds || workers || face_frames || encode_seconds ||
         record_seconds || events_limit || export_id || delete_id || list_faces || reembed || !save_path.empty() ||
         !detect_image_path.empty() || !face_image_path.empty() || !embed_paths.empty() || !enroll_name.empty() ||
         !unenroll_name.empty())) {
        std::fprintf(stderr, "--passes 不能与其他运行模式组合\n");
        return kExitUsage;
    }
    if (access_seconds > 0 &&
        (hw_seconds || pass_limit || check_only || ui || capture_frames || run_seconds || bench_seconds ||
         busy_threads || bench_frames || rga_frames || detect_frames || detect_seconds || workers || face_frames ||
         encode_seconds || record_seconds || events_limit || export_id || delete_id || list_faces || reembed ||
         !save_path.empty() || !detect_image_path.empty() || !face_image_path.empty() || !embed_paths.empty() ||
         !enroll_name.empty() || !unenroll_name.empty())) {
        std::fprintf(stderr, "--access-run 不能与其他模式组合\n");
        return kExitUsage;
    }
    if (busy_threads > 0 && bench_seconds == 0) {
        std::fprintf(stderr, "--busy 只能和 --ui-bench 一起用\n");
        return kExitUsage;
    }
    if (workers > 0 && detect_seconds == 0) {
        std::fprintf(stderr,
                     "--workers 只能和 --detect-run 一起用（--ui 的推理线程数在配置文件 detect.workers 里改）\n");
        return kExitUsage;
    }

    std::vector<rksec::ini::Error> errors;
    const auto cfg = rksec::config::load_file(config_path, errors);
    if (!cfg) {
        for (const auto &e : errors) {
            if (e.line > 0) {
                std::fprintf(stderr, "配置错误 %s:%d: %s\n", config_path.c_str(), e.line, e.message.c_str());
            } else {
                std::fprintf(stderr, "配置错误 %s: %s\n", config_path.c_str(), e.message.c_str());
            }
        }
        std::fprintf(stderr, "共 %zu 处错误\n", errors.size());
        return kExitConfig;
    }
    if (check_only) {
        std::printf("配置检查通过: %s\n%s\n", config_path.c_str(), rksec::config::summary(*cfg).c_str());
        return kExitOk;
    }

    if (hw_seconds > 0) {
        return rksec::app::hw_test({cfg->pir.chip, cfg->pir.line, cfg->pir.active_level == 1}, hw_seconds);
    }

    rksec::log::set_level(cfg->log.level);

    if (bench_seconds > 0) {
        // 不碰摄像头和屏幕，所以不拿单实例锁，可以和 --ui 同时跑
        rksec::app::SimOptions o;
        o.seconds = bench_seconds;
        o.busy_threads = busy_threads;
        o.auto_nav = true;
        const auto backend = rksec::app::headless_backend();
        rksec::app::ui_sim(o, *backend);
        return kExitOk;
    }

    if (!detect_image_path.empty()) {
        // 只用 NPU，不碰摄像头和屏幕，不拿单实例锁
        return rksec::app::detect_image(*cfg, detect_image_path, save_path);
    }
    if (!face_image_path.empty()) {
        return rksec::app::face_image(*cfg, face_image_path, save_path);
    }
    if (!embed_paths.empty()) {
        return rksec::app::face_embed(*cfg, embed_paths, save_path);
    }
    if (pass_limit > 0) {
        return rksec::app::pass_list(*cfg, pass_limit);
    }

    // 人脸库的命令：不碰摄像头（用摄像头登记的除外，见下面），不拿单实例锁
    if (list_faces) {
        return rksec::app::face_list(*cfg);
    }
    if (!unenroll_name.empty()) {
        return rksec::app::face_unenroll(*cfg, unenroll_name);
    }
    if (reembed) {
        return rksec::app::face_reembed(*cfg);
    }
    if (!enroll_name.empty() && !enroll_photos.empty()) {
        return rksec::app::face_enroll(*cfg, enroll_name, enroll_photos);
    }

    // 事件录像的命令：只用数据库和录像目录，不拿单实例锁（界面开着时也能用）
    if (events_limit > 0) {
        return rksec::app::event_list(*cfg, events_limit);
    }
    if (export_id > 0) {
        return rksec::app::event_export(*cfg, export_id, export_dir);
    }
    if (delete_id > 0) {
        return rksec::app::event_delete(*cfg, delete_id);
    }

    // 用摄像头或屏幕的模式先拿单实例锁（D18）。锁对象活到 main 结束，进程退出时自动释放
    rksec::InstanceLock lock;
    if (access_seconds > 0 || ui || run_seconds > 0 || capture_frames > 0 || bench_frames > 0 || rga_frames > 0 ||
        detect_frames > 0 || detect_seconds > 0 || face_frames > 0 || encode_seconds > 0 || record_seconds > 0 ||
        !enroll_name.empty()) {
        std::string err;
        if (!lock.acquire(kLockFile, err)) {
            LOG_E("main", "%s", err.c_str());
            return kExitBusy;
        }
    }
    if (access_seconds > 0) {
        return rksec::app::access_run(*cfg, access_seconds);
    }
    if (ui) {
        return rksec::app::ui_run(*cfg);
    }
    if (run_seconds > 0) {
        return rksec::app::capture_run(*cfg, run_seconds);
    }
    if (bench_frames > 0) {
        return rksec::app::decode_bench(*cfg, bench_frames, save_path);
    }
    if (rga_frames > 0) {
        return rksec::app::rga_bench(*cfg, rga_frames, save_path);
    }
    if (detect_frames > 0) {
        return rksec::app::detect_bench(*cfg, detect_frames);
    }
    if (detect_seconds > 0) {
        return rksec::app::detect_run(*cfg, detect_seconds, workers);
    }
    if (record_seconds > 0) {
        return rksec::app::record_run(*cfg, record_seconds);
    }
    if (encode_seconds > 0) {
        return rksec::app::encode_run(*cfg, encode_seconds, encode_out);
    }
    if (face_frames > 0) {
        return rksec::app::face_bench(*cfg, face_frames);
    }
    if (!enroll_name.empty()) {
        return rksec::app::face_enroll(*cfg, enroll_name, {});  // 用摄像头登记
    }
    if (capture_frames > 0) {
        return rksec::app::capture_test(*cfg, capture_frames, save_path);
    }
    LOG_I("main", "%s 启动", rksec::version::full().c_str());
    LOG_I("main", "配置 %s: %s", config_path.c_str(), rksec::config::summary(*cfg).c_str());
    LOG_W("main", "采集和检测流程尚未实现");
    return kExitOk;
}
