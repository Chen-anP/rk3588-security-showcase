#include "core/config.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "detect/coco_names.hpp"

namespace rksec::config {

namespace {

// 读取一个整数项：没写就用默认值；写了就必须是范围内的整数
void read_int(const ini::Document &doc, std::string_view section, std::string_view key, int min, int max, int &out,
              std::vector<ini::Error> &errors)
{
    const ini::Value *v = doc.find(section, key);
    if (v == nullptr) {
        return;
    }
    int n = 0;
    const char *first = v->text.data();
    const char *last = first + v->text.size();
    const auto [ptr, ec] = std::from_chars(first, last, n);
    const std::string name = std::string{section} + "." + std::string{key};
    if (ec != std::errc{} || ptr != last) {
        errors.push_back({v->line, name + " 应为整数，实际是 \"" + v->text + "\""});
        return;
    }
    if (n < min || n > max) {
        errors.push_back({v->line, name + " = " + std::to_string(n) + " 超出范围 " + std::to_string(min) + "～" +
                                       std::to_string(max)});
        return;
    }
    out = n;
}

// 读取一个小数项：没写就用默认值；写了就必须是范围内的数
void read_float(const ini::Document &doc, std::string_view section, std::string_view key, float min, float max,
                float &out, std::vector<ini::Error> &errors)
{
    const ini::Value *v = doc.find(section, key);
    if (v == nullptr) {
        return;
    }
    const std::string name = std::string{section} + "." + std::string{key};
    char *end = nullptr;
    const double d = std::strtod(v->text.c_str(), &end);
    if (v->text.empty() || end != v->text.c_str() + v->text.size()) {
        errors.push_back({v->line, name + " 应为小数，实际是 \"" + v->text + "\""});
        return;
    }
    if (d < static_cast<double>(min) || d > static_cast<double>(max)) {
        std::array<char, 96> buf{};
        std::snprintf(buf.data(), buf.size(), " 超出范围 %g～%g", static_cast<double>(min), static_cast<double>(max));
        errors.push_back({v->line, name + " = " + v->text + buf.data()});
        return;
    }
    out = static_cast<float>(d);
}

std::string trim(std::string_view s)
{
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(" \t");
    return std::string{s.substr(b, e - b + 1)};
}

}  // namespace

std::optional<AppConfig> load(std::string_view text, std::vector<ini::Error> &errors)
{
    const std::size_t errors_before = errors.size();
    const ini::Document doc = ini::Document::parse(text, errors);
    AppConfig cfg;

    // [camera]
    if (const ini::Value *v = doc.find("camera", "device"); v == nullptr || v->text.empty()) {
        errors.push_back({v == nullptr ? 0 : v->line, "缺少必填项 camera.device"});
    } else {
        cfg.camera.device = v->text;
    }
    read_int(doc, "camera", "width", 160, 1920, cfg.camera.width, errors);
    read_int(doc, "camera", "height", 120, 1080, cfg.camera.height, errors);
    read_int(doc, "camera", "fps", 1, 60, cfg.camera.fps, errors);
    read_int(doc, "camera", "buffers", 2, 16, cfg.camera.buffers, errors);
    if (const ini::Value *v = doc.find("camera", "power_line_hz"); v != nullptr) {
        if (v->text == "50" || v->text == "60" || v->text == "0") {
            cfg.camera.power_line_hz = std::stoi(v->text);
        } else {
            errors.push_back({v->line, "camera.power_line_hz 只能是 50、60 或 0（关闭），实际是 \"" + v->text + "\""});
        }
    }

    // [display]
    read_int(doc, "display", "idle_timeout_seconds", 0, 3600, cfg.display.idle_timeout_seconds, errors);
    if (const ini::Value *v = doc.find("display", "device"); v != nullptr) {
        if (v->text == "auto" || v->text.rfind("/dev/dri/card", 0) == 0) {
            cfg.display.device = v->text;
        } else {
            errors.push_back({v->line, "display.device 只能是 auto 或 /dev/dri/cardN，实际是 \"" + v->text + "\""});
        }
    }
    if (const ini::Value *v = doc.find("display", "connector"); v != nullptr) {
        if (v->text.empty()) {
            errors.push_back({v->line, "display.connector 不能为空（自动选择写 auto）"});
        } else {
            cfg.display.connector = v->text;
        }
    }
    if (const ini::Value *v = doc.find("display", "touch"); v != nullptr) {
        if (v->text.empty() || v->text.rfind("/dev/input/", 0) == 0) {
            cfg.display.touch = v->text;
        } else {
            errors.push_back(
                {v->line, "display.touch 应为 /dev/input/ 下的路径（不用触摸就留空），实际是 \"" + v->text + "\""});
        }
    }

    // [detect]
    if (const ini::Value *v = doc.find("detect", "enabled"); v != nullptr) {
        if (v->text == "true" || v->text == "false") {
            cfg.detect.enabled = v->text == "true";
        } else {
            errors.push_back({v->line, "detect.enabled 只能是 true 或 false，实际是 \"" + v->text + "\""});
        }
    }
    if (const ini::Value *v = doc.find("detect", "model"); v != nullptr) {
        if (v->text.empty() || v->text[0] != '/') {
            errors.push_back({v->line, "detect.model 应为模型文件的绝对路径，实际是 \"" + v->text + "\""});
        } else {
            cfg.detect.model = v->text;
        }
    }
    read_int(doc, "detect", "workers", 1, 3, cfg.detect.workers, errors);
    read_float(doc, "detect", "score_threshold", 0.05F, 0.95F, cfg.detect.score_threshold, errors);
    read_float(doc, "detect", "nms_threshold", 0.1F, 0.9F, cfg.detect.nms_threshold, errors);
    if (const ini::Value *v = doc.find("detect", "classes"); v != nullptr) {
        std::vector<std::string> names;
        std::string_view rest = v->text;
        bool ok = true;
        while (ok) {
            const auto comma = rest.find(',');
            const std::string name = trim(rest.substr(0, comma));
            if (detect::coco_id(name) < 0) {
                errors.push_back({v->line, "detect.classes 里的 \"" + name +
                                               "\" 不是 COCO 类别名（英文小写，比如 person、car，逗号分隔）"});
                ok = false;
            } else if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
            if (comma == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(comma + 1);
        }
        if (ok) {
            cfg.detect.classes = names;
        }
    }

    // [face]
    if (const ini::Value *v = doc.find("face", "detect_model"); v != nullptr) {
        if (v->text.empty() || v->text[0] != '/') {
            errors.push_back({v->line, "face.detect_model 应为模型文件的绝对路径，实际是 \"" + v->text + "\""});
        } else {
            cfg.face.detect_model = v->text;
        }
    }
    read_float(doc, "face", "detect_threshold", 0.1F, 0.99F, cfg.face.detect_threshold, errors);
    if (const ini::Value *v = doc.find("face", "recognize"); v != nullptr) {
        if (v->text == "true" || v->text == "false") {
            cfg.face.recognize = v->text == "true";
        } else {
            errors.push_back({v->line, "face.recognize 只能是 true 或 false，实际是 \"" + v->text + "\""});
        }
    }
    if (const ini::Value *v = doc.find("face", "recognize_model"); v != nullptr) {
        if (v->text.empty() || v->text[0] != '/') {
            errors.push_back({v->line, "face.recognize_model 应为模型文件的绝对路径，实际是 \"" + v->text + "\""});
        } else {
            cfg.face.recognize_model = v->text;
        }
    }
    read_float(doc, "face", "match_threshold", 0.1F, 0.95F, cfg.face.match_threshold, errors);
    if (const ini::Value *v = doc.find("face", "database"); v != nullptr) {
        if (v->text.empty() || v->text[0] != '/') {
            errors.push_back({v->line, "face.database 应为人脸库文件的绝对路径，实际是 \"" + v->text + "\""});
        } else {
            cfg.face.database = v->text;
        }
    }

    // [encode]
    if (const ini::Value *v = doc.find("encode", "enabled"); v != nullptr) {
        if (v->text == "true" || v->text == "false") {
            cfg.encode.enabled = v->text == "true";
        } else {
            errors.push_back({v->line, "encode.enabled 只能是 true 或 false，实际是 \"" + v->text + "\""});
        }
    }
    read_int(doc, "encode", "bitrate_kbps", 200, 20000, cfg.encode.bitrate_kbps, errors);
    read_int(doc, "encode", "fps", 5, 60, cfg.encode.fps, errors);
    read_int(doc, "encode", "gop", 5, 300, cfg.encode.gop, errors);

    // [record]
    if (const ini::Value *v = doc.find("record", "enabled"); v != nullptr) {
        if (v->text == "true" || v->text == "false") {
            cfg.record.enabled = v->text == "true";
        } else {
            errors.push_back({v->line, "record.enabled 只能是 true 或 false，实际是 \"" + v->text + "\""});
        }
    }
    if (const ini::Value *v = doc.find("record", "dir"); v != nullptr) {
        if (v->text.empty() || v->text[0] != '/') {
            errors.push_back({v->line, "record.dir 应为录像目录的绝对路径，实际是 \"" + v->text + "\""});
        } else {
            cfg.record.dir = v->text;
        }
    }
    read_int(doc, "record", "pre_seconds", 1, 30, cfg.record.pre_seconds, errors);
    read_int(doc, "record", "post_seconds", 1, 120, cfg.record.post_seconds, errors);
    read_int(doc, "record", "max_seconds", 30, 3600, cfg.record.max_seconds, errors);
    read_int(doc, "record", "quota_mb", 64, 1000000, cfg.record.quota_mb, errors);

    // [rtsp]
    if (const ini::Value *v = doc.find("rtsp", "enabled"); v != nullptr) {
        if (v->text == "true" || v->text == "false") {
            cfg.rtsp.enabled = v->text == "true";
        } else {
            errors.push_back({v->line, "rtsp.enabled 只能是 true 或 false，实际是 \"" + v->text + "\""});
        }
    }
    if (const ini::Value *v = doc.find("rtsp", "address"); v != nullptr) {
        in_addr a{};
        if (::inet_pton(AF_INET, v->text.c_str(), &a) != 1) {
            errors.push_back({v->line, "rtsp.address 应为 IPv4 地址（比如 0.0.0.0），实际是 \"" + v->text + "\""});
        } else {
            cfg.rtsp.address = v->text;
        }
    }
    read_int(doc, "rtsp", "port", 1024, 65535, cfg.rtsp.port, errors);
    if (const ini::Value *v = doc.find("rtsp", "path"); v != nullptr) {
        const bool ok = v->text.size() >= 2 && v->text.size() <= 64 && v->text[0] == '/' &&
                        v->text.find_first_not_of(
                            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-/") == std::string::npos;
        if (!ok) {
            errors.push_back(
                {v->line, "rtsp.path 应以 / 开头，只能有字母、数字和 _ - /，比如 /live，实际是 \"" + v->text + "\""});
        } else {
            cfg.rtsp.path = v->text;
        }
    }
    if (const ini::Value *v = doc.find("rtsp", "credentials"); v != nullptr) {
        if (v->text.empty() || v->text[0] != '/') {
            errors.push_back({v->line, "rtsp.credentials 应为账号文件的绝对路径，实际是 \"" + v->text + "\""});
        } else {
            cfg.rtsp.credentials = v->text;
        }
    }

    // [pir]：只指定输入设备，不启用业务联动。
    if (const ini::Value *v = doc.find("pir", "chip"); v != nullptr) {
        if (v->text.rfind("/dev/gpiochip", 0) != 0 || v->text.size() <= 13 ||
            v->text.find_first_not_of("0123456789", 13) != std::string::npos) {
            errors.push_back({v->line, "pir.chip 应为 /dev/gpiochipN"});
        } else {
            cfg.pir.chip = v->text;
        }
    }
    read_int(doc, "pir", "line", 0, 1023, cfg.pir.line, errors);
    read_int(doc, "pir", "active_level", 0, 1, cfg.pir.active_level, errors);

    if (const ini::Value *v = doc.find("access", "enabled"); v != nullptr) {
        if (v->text != "true" && v->text != "false") {
            errors.push_back({v->line, "access.enabled 只能是 true 或 false"});
        } else {
            cfg.access.enabled = v->text == "true";
        }
    }
    if (cfg.access.enabled && (!cfg.detect.enabled || !cfg.face.recognize)) {
        errors.push_back({0, "门禁需要检测和人脸识别"});
    }
    if (const auto *v = doc.find("mqtt", "enabled")) {
        if (v->text != "true" && v->text != "false") {
            errors.push_back({v->line, "mqtt.enabled 只能是 true 或 false"});
        }
        cfg.mqtt.enabled = v->text == "true";
    }
    for (const auto &[key, target] : {std::pair{"host", &cfg.mqtt.host},
                                      {"device_id", &cfg.mqtt.device_id},
                                      {"credentials", &cfg.mqtt.credentials},
                                      {"ca_file", &cfg.mqtt.ca_file}}) {
        if (const auto *v = doc.find("mqtt", key)) {
            *target = v->text;
        }
    }
    read_int(doc, "mqtt", "port", 1, 65535, cfg.mqtt.port, errors);
    if (cfg.mqtt.enabled) {
        if (cfg.mqtt.device_id.empty() || cfg.mqtt.device_id.size() > 48 ||
            cfg.mqtt.device_id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
                std::string::npos) {
            errors.push_back({0, "mqtt.device_id 必须为 1～48 位字母、数字、下划线或短横线"});
        }
        if (cfg.mqtt.host.empty() || cfg.mqtt.host.size() > 253 ||
            cfg.mqtt.host.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-.:") !=
                std::string::npos ||
            cfg.mqtt.credentials.empty() || cfg.mqtt.credentials.front() != '/' || cfg.mqtt.ca_file.empty() ||
            cfg.mqtt.ca_file.front() != '/') {
            errors.push_back({0, "MQTT 需要 Broker 主机名或 IP、凭据文件及 CA 文件绝对路径"});
        }
    }
    // [log]
    if (const ini::Value *v = doc.find("log", "level"); v != nullptr) {
        if (const auto l = log::parse_level(v->text)) {
            cfg.log.level = *l;
        } else {
            errors.push_back({v->line, "log.level 只能是 debug、info、warn、error，实际是 \"" + v->text + "\""});
        }
    }

    // 没被读取过的键 = 拼错了或者不支持
    for (const auto &[name, value] : doc.unused()) {
        errors.push_back({value->line, "未知配置项 " + name + "（拼写错误？）"});
    }

    if (errors.size() != errors_before) {
        // 按行号排序，用户从上往下改；和具体行无关的错误（行号 0）排在最前
        std::stable_sort(errors.begin() + static_cast<std::ptrdiff_t>(errors_before), errors.end(),
                         [](const ini::Error &a, const ini::Error &b) { return a.line < b.line; });
        return std::nullopt;
    }
    return cfg;
}

std::optional<AppConfig> load_file(const std::string &path, std::vector<ini::Error> &errors)
{
    std::ifstream in(path);
    if (!in) {
        errors.push_back({0, "打不开配置文件 " + path});
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return load(ss.str(), errors);
}

std::string summary(const AppConfig &cfg)
{
    std::string s;
    s += "camera.device=" + cfg.camera.device;
    s += " " + std::to_string(cfg.camera.width) + "x" + std::to_string(cfg.camera.height);
    s += "@" + std::to_string(cfg.camera.fps) + "fps";
    s += " power_line=" + std::to_string(cfg.camera.power_line_hz) + "Hz";
    s += " buffers=" + std::to_string(cfg.camera.buffers);
    s += " display.device=" + cfg.display.device;
    s += " connector=" + cfg.display.connector;
    s += " display.idle_timeout_seconds=" + std::to_string(cfg.display.idle_timeout_seconds);
    s += " touch=" + (cfg.display.touch.empty() ? std::string{"无"} : cfg.display.touch);
    s += " detect=" + std::string{cfg.detect.enabled ? "开" : "关"};
    if (cfg.detect.enabled) {
        std::array<char, 64> buf{};
        std::snprintf(buf.data(), buf.size(), " workers=%d score>%.2f nms=%.2f", cfg.detect.workers,
                      static_cast<double>(cfg.detect.score_threshold), static_cast<double>(cfg.detect.nms_threshold));
        s += " model=" + cfg.detect.model + buf.data() + " classes=";
        for (std::size_t i = 0; i < cfg.detect.classes.size(); ++i) {
            s += (i == 0 ? "" : ",") + cfg.detect.classes[i];
        }
    }
    s += " face.detect_model=" + cfg.face.detect_model;
    s += " recognize=" + std::string{cfg.face.recognize ? "开" : "关"};
    if (cfg.face.recognize) {
        std::array<char, 32> buf{};
        std::snprintf(buf.data(), buf.size(), " match>=%.2f", static_cast<double>(cfg.face.match_threshold));
        s += " recognize_model=" + cfg.face.recognize_model + buf.data() + " database=" + cfg.face.database;
    }
    s += " encode=" + std::string{cfg.encode.enabled ? "开" : "关"};
    if (cfg.encode.enabled) {
        s += " " + std::to_string(cfg.encode.bitrate_kbps) + "kbps@" + std::to_string(cfg.encode.fps) +
             "fps gop=" + std::to_string(cfg.encode.gop);
    }
    s += " record=" + std::string{cfg.record.enabled ? "开" : "关"};
    if (cfg.record.enabled) {
        s += " dir=" + cfg.record.dir + " pre=" + std::to_string(cfg.record.pre_seconds) +
             "s post=" + std::to_string(cfg.record.post_seconds) + "s max=" + std::to_string(cfg.record.max_seconds) +
             "s quota=" + std::to_string(cfg.record.quota_mb) + "MB";
    }
    s += " rtsp=" + std::string{cfg.rtsp.enabled ? "开" : "关"};
    if (cfg.rtsp.enabled) {
        s += " " + cfg.rtsp.address + ":" + std::to_string(cfg.rtsp.port) + cfg.rtsp.path +
             " credentials=" + cfg.rtsp.credentials;
    }
    s += " log.level=" + std::string{log::level_name(cfg.log.level)};
    return s;
}

}  // namespace rksec::config
