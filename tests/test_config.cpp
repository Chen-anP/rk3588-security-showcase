#include <gtest/gtest.h>

#include <string>

#include "core/config.hpp"

using rksec::ini::Error;
namespace config = rksec::config;

namespace {

const char *const kMinimal = "[camera]\ndevice = /dev/v4l/by-id/cam\n";

// 把所有错误拼成一个字符串，断言失败时方便看
std::string dump(const std::vector<Error> &errors)
{
    std::string s;
    for (const auto &e : errors) {
        s += "  第 " + std::to_string(e.line) + " 行: " + e.message + "\n";
    }
    return s;
}

}  // namespace

TEST(Config, MinimalFileUsesDefaults)
{
    std::vector<Error> errors;
    const auto cfg = config::load(kMinimal, errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_EQ(cfg->camera.device, "/dev/v4l/by-id/cam");
    EXPECT_EQ(cfg->camera.width, 1280);
    EXPECT_EQ(cfg->camera.height, 720);
    EXPECT_EQ(cfg->camera.fps, 30);
    EXPECT_EQ(cfg->camera.power_line_hz, 50);
    EXPECT_EQ(cfg->camera.buffers, 4);
    EXPECT_EQ(cfg->log.level, rksec::log::Level::info);
}

TEST(Config, ReadsAllFields)
{
    std::vector<Error> errors;
    const auto cfg = config::load("[camera]\ndevice = /dev/x\nwidth = 640\nheight = 480\nfps = 15\n"
                                  "power_line_hz = 0\n[log]\nlevel = debug\n",
                                  errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_EQ(cfg->camera.width, 640);
    EXPECT_EQ(cfg->camera.height, 480);
    EXPECT_EQ(cfg->camera.fps, 15);
    EXPECT_EQ(cfg->camera.power_line_hz, 0);
    EXPECT_EQ(cfg->log.level, rksec::log::Level::debug);
}

TEST(Config, DeviceIsRequired)
{
    std::vector<Error> errors;
    EXPECT_FALSE(config::load("[camera]\nwidth = 640\n", errors));
    ASSERT_EQ(errors.size(), 1U) << dump(errors);
    EXPECT_NE(errors[0].message.find("camera.device"), std::string::npos);

    errors.clear();
    EXPECT_FALSE(config::load("[camera]\ndevice =\n", errors)) << "写了但是空的也不行";
    ASSERT_EQ(errors.size(), 1U) << dump(errors);
    EXPECT_EQ(errors[0].line, 2);
}

TEST(Config, RejectsNonIntegerAndTrailingGarbage)
{
    for (const char *bad : {"abc", "30fps", "3.5", "", " "}) {
        std::vector<Error> errors;
        const std::string text = std::string{kMinimal} + "fps = " + bad + "\n";
        EXPECT_FALSE(config::load(text, errors)) << "fps = \"" << bad << "\" 应该被拒绝";
    }
}

TEST(Config, RejectsOutOfRange)
{
    std::vector<Error> errors;
    EXPECT_FALSE(config::load(std::string{kMinimal} + "fps = 0\nwidth = 99999\n", errors));
    ASSERT_EQ(errors.size(), 2U) << dump(errors);
    EXPECT_EQ(errors[0].line, 3);
    EXPECT_NE(errors[0].message.find("1～60"), std::string::npos) << errors[0].message;
    EXPECT_EQ(errors[1].line, 4);
}

TEST(Config, BoundaryValuesAreAccepted)
{
    std::vector<Error> errors;
    const auto cfg = config::load(std::string{kMinimal} + "fps = 60\nwidth = 160\nheight = 1080\n", errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_EQ(cfg->camera.fps, 60);
}

TEST(Config, PowerLineMustBe50Or60Or0)
{
    std::vector<Error> errors;
    EXPECT_FALSE(config::load(std::string{kMinimal} + "power_line_hz = 55\n", errors));
    ASSERT_EQ(errors.size(), 1U) << dump(errors);
}

TEST(Config, UnknownLogLevel)
{
    std::vector<Error> errors;
    EXPECT_FALSE(config::load(std::string{kMinimal} + "[log]\nlevel = verbose\n", errors));
    ASSERT_EQ(errors.size(), 1U) << dump(errors);
    EXPECT_EQ(errors[0].line, 4);
}

TEST(Config, MisspelledKeyIsAnError)
{
    std::vector<Error> errors;
    EXPECT_FALSE(config::load(std::string{kMinimal} + "fsp = 30\n[logs]\nlevel = info\n", errors));
    ASSERT_EQ(errors.size(), 2U) << dump(errors);
    EXPECT_NE(errors[0].message.find("camera.fsp"), std::string::npos) << errors[0].message;
    EXPECT_NE(errors[1].message.find("logs.level"), std::string::npos) << errors[1].message;
}

TEST(Config, ReportsAllErrorsAtOnce)
{
    std::vector<Error> errors;
    EXPECT_FALSE(config::load("[camera]\nfps = abc\nwidth = 1\noops\n[log]\nlevel = x\n", errors));
    // 缺 device、fps 不是整数、width 越界、第 4 行格式错、level 不认识
    EXPECT_EQ(errors.size(), 5U) << dump(errors);
}

TEST(Config, MissingFileIsAnError)
{
    std::vector<Error> errors;
    EXPECT_FALSE(config::load_file("/nonexistent/rk3588-security.conf", errors));
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].line, 0);
}

TEST(Config, ShippedConfigFileIsValid)
{
    // 仓库里自带的配置文件必须能通过校验
    std::vector<Error> errors;
    const auto cfg = config::load_file(RKSEC_SOURCE_DIR "/config/rk3588-security.conf", errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_EQ(cfg->camera.device.rfind("/dev/v4l/by-id/", 0), 0U) << "应使用固定路径（D04）";
    EXPECT_EQ(cfg->camera.power_line_hz, 50) << "中国电网 50 Hz（D03）";
}

TEST(Config, BuffersRange)
{
    std::vector<Error> errors;
    const auto ok = config::load("[camera]\ndevice = /dev/x\nbuffers = 6\n", errors);
    ASSERT_TRUE(ok) << dump(errors);
    EXPECT_EQ(ok->camera.buffers, 6);
    EXPECT_FALSE(config::load("[camera]\ndevice = /dev/x\nbuffers = 1\n", errors));
    EXPECT_NE(dump(errors).find("camera.buffers = 1 超出范围 2～16"), std::string::npos) << dump(errors);
}

TEST(Config, DisplayDefaults)
{
    std::vector<Error> errors;
    const auto cfg = config::load(kMinimal, errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_EQ(cfg->display.device, "auto");
    EXPECT_EQ(cfg->display.connector, "auto");
    EXPECT_EQ(cfg->display.touch, "") << "没写触摸 = 不用触摸";
}

TEST(Config, DisplayFields)
{
    std::vector<Error> errors;
    const auto cfg = config::load(std::string{kMinimal} + "[display]\ndevice = /dev/dri/card0\nconnector = HDMI-A-1\n"
                                                          "touch = /dev/input/by-id/usb-touch-event-if00\n",
                                  errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_EQ(cfg->display.device, "/dev/dri/card0");
    EXPECT_EQ(cfg->display.connector, "HDMI-A-1");
    EXPECT_EQ(cfg->display.touch, "/dev/input/by-id/usb-touch-event-if00");

    const auto empty_touch = config::load(std::string{kMinimal} + "[display]\ntouch =\n", errors);
    ASSERT_TRUE(empty_touch) << dump(errors);
    EXPECT_EQ(empty_touch->display.touch, "");
}

TEST(Config, DisplayRejectsBadValues)
{
    std::vector<Error> errors;
    EXPECT_FALSE(
        config::load(std::string{kMinimal} + "[display]\ndevice = card0\nconnector =\ntouch = /dev/video0\n", errors));
    const std::string all = dump(errors);
    EXPECT_EQ(errors.size(), 3U) << all;
    EXPECT_NE(all.find("display.device 只能是 auto 或 /dev/dri/cardN"), std::string::npos) << all;
    EXPECT_NE(all.find("display.connector 不能为空"), std::string::npos) << all;
    EXPECT_NE(all.find("display.touch 应为 /dev/input/ 下的路径"), std::string::npos) << all;
}

TEST(Config, ShippedConfigUsesStableTouchPath)
{
    std::vector<Error> errors;
    const auto cfg = config::load_file(RKSEC_SOURCE_DIR "/config/rk3588-security.conf", errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_EQ(cfg->display.device, "auto") << "卡号会变，按驱动名选";
    EXPECT_EQ(cfg->display.touch.rfind("/dev/input/by-id/", 0), 0U) << "eventN 编号会变，用固定路径";
}

// [detect]（第 5d、6 步）
TEST(Config, DetectDefaults)
{
    std::vector<Error> errors;
    const auto cfg = config::load(kMinimal, errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_TRUE(cfg->detect.enabled);
    EXPECT_EQ(cfg->detect.workers, 1);
    EXPECT_FLOAT_EQ(cfg->detect.score_threshold, 0.25F);
    EXPECT_FLOAT_EQ(cfg->detect.nms_threshold, 0.45F);
    ASSERT_EQ(cfg->detect.classes.size(), 1U);
    EXPECT_EQ(cfg->detect.classes[0], "person");
}

TEST(Config, DetectFields)
{
    std::vector<Error> errors;
    const auto cfg =
        config::load(std::string{kMinimal} + "[detect]\nenabled = false\nmodel = /opt/m.rknn\nworkers = 3\n"
                                             "score_threshold = 0.5\nnms_threshold = 0.6\n"
                                             "classes = person, car ,person\n",
                     errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_FALSE(cfg->detect.enabled);
    EXPECT_EQ(cfg->detect.model, "/opt/m.rknn");
    EXPECT_EQ(cfg->detect.workers, 3);
    EXPECT_FLOAT_EQ(cfg->detect.score_threshold, 0.5F);
    EXPECT_FLOAT_EQ(cfg->detect.nms_threshold, 0.6F);
    ASSERT_EQ(cfg->detect.classes.size(), 2U) << "去掉空格、去掉重复";
    EXPECT_EQ(cfg->detect.classes[1], "car");
}

TEST(Config, DetectRejectsBadValues)
{
    std::vector<Error> errors;
    EXPECT_FALSE(config::load(std::string{kMinimal} + "[detect]\nenabled = yes\nmodel = models/m.rknn\nworkers = 4\n"
                                                      "score_threshold = 0.25x\nnms_threshold = 1.5\n"
                                                      "classes = person,human\n",
                              errors));
    EXPECT_EQ(errors.size(), 6U) << dump(errors);
    const std::string all = dump(errors);
    for (const char *key : {"detect.enabled", "detect.model", "detect.workers", "detect.score_threshold",
                            "detect.nms_threshold", "human"}) {
        EXPECT_NE(all.find(key), std::string::npos) << key;
    }
}

// [face]（第 7b 步）
TEST(Config, FaceFields)
{
    std::vector<Error> errors;
    const auto def = config::load(kMinimal, errors);
    ASSERT_TRUE(def) << dump(errors);
    EXPECT_FLOAT_EQ(def->face.detect_threshold, 0.5F);
    const auto cfg =
        config::load(std::string{kMinimal} + "[face]\ndetect_model = /opt/f.rknn\ndetect_threshold = 0.7\n", errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_EQ(cfg->face.detect_model, "/opt/f.rknn");
    EXPECT_FLOAT_EQ(cfg->face.detect_threshold, 0.7F);
    EXPECT_FALSE(config::load(std::string{kMinimal} + "[face]\ndetect_model = f.rknn\ndetect_threshold = 1\n", errors));
    EXPECT_EQ(errors.size(), 2U) << dump(errors);
}

// [face] 人脸识别（第 7c 步）
TEST(Config, FaceRecognitionFields)
{
    std::vector<Error> errors;
    const auto def = config::load(kMinimal, errors);
    ASSERT_TRUE(def) << dump(errors);
    EXPECT_TRUE(def->face.recognize);
    EXPECT_FLOAT_EQ(def->face.match_threshold, 0.4F);
    EXPECT_EQ(def->face.database, "/var/lib/rk3588-security/faces.db");
    const auto cfg = config::load(std::string{kMinimal} + "[face]\nrecognize = false\nrecognize_model = /opt/m.rknn\n"
                                                          "match_threshold = 0.55\ndatabase = /tmp/f.db\n",
                                  errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_FALSE(cfg->face.recognize);
    EXPECT_EQ(cfg->face.recognize_model, "/opt/m.rknn");
    EXPECT_FLOAT_EQ(cfg->face.match_threshold, 0.55F);
    EXPECT_EQ(cfg->face.database, "/tmp/f.db");
    errors.clear();
    EXPECT_FALSE(config::load(std::string{kMinimal} + "[face]\nrecognize = yes\nrecognize_model = m.rknn\n"
                                                      "match_threshold = 0.99\ndatabase = faces.db\n",
                              errors));
    EXPECT_EQ(errors.size(), 4U) << dump(errors);
    errors.clear();
    EXPECT_FALSE(config::load(std::string{kMinimal} + "[face]\ndatabase =\n", errors)) << "人脸库不能留空";
}

TEST(Config, EncodeDefaultsAndFields)
{
    std::vector<Error> errors;
    const auto def = config::load(kMinimal, errors);
    ASSERT_TRUE(def) << dump(errors);
    EXPECT_TRUE(def->encode.enabled);
    EXPECT_EQ(def->encode.bitrate_kbps, 4000);
    EXPECT_EQ(def->encode.fps, 25);
    EXPECT_EQ(def->encode.gop, 50);

    const auto cfg = config::load(
        std::string{kMinimal} + "[encode]\nenabled = false\nbitrate_kbps = 2000\nfps = 15\ngop = 30\n", errors);
    ASSERT_TRUE(cfg);
    EXPECT_FALSE(cfg->encode.enabled);
    EXPECT_EQ(cfg->encode.bitrate_kbps, 2000);
    EXPECT_EQ(cfg->encode.fps, 15);
    EXPECT_EQ(cfg->encode.gop, 30);
}

TEST(Config, EncodeRejectsBadValues)
{
    for (const char *bad : {"bitrate_kbps = 100", "bitrate_kbps = 30000", "fps = 2", "fps = 120", "gop = 1",
                            "gop = 1000", "enabled = yes", "bitrate = 4000"}) {
        std::vector<Error> errors;
        EXPECT_FALSE(config::load(std::string{kMinimal} + "[encode]\n" + bad + "\n", errors)) << bad;
        EXPECT_FALSE(errors.empty()) << bad;
    }
}

TEST(Config, RecordDefaultsAndFields)
{
    std::vector<Error> errors;
    const auto def = config::load(kMinimal, errors);
    ASSERT_TRUE(def) << dump(errors);
    EXPECT_TRUE(def->record.enabled);
    EXPECT_EQ(def->record.dir, "/var/lib/rk3588-security/events");
    EXPECT_EQ(def->record.pre_seconds, 5);
    EXPECT_EQ(def->record.post_seconds, 10);
    EXPECT_EQ(def->record.max_seconds, 300);
    EXPECT_EQ(def->record.quota_mb, 2048);

    const auto cfg = config::load(std::string{kMinimal} +
                                      "[record]\nenabled = false\ndir = /tmp/ev\npre_seconds = 3\npost_seconds = 20\n"
                                      "max_seconds = 60\nquota_mb = 100\n",
                                  errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_FALSE(cfg->record.enabled);
    EXPECT_EQ(cfg->record.dir, "/tmp/ev");
    EXPECT_EQ(cfg->record.pre_seconds, 3);
    EXPECT_EQ(cfg->record.post_seconds, 20);
    EXPECT_EQ(cfg->record.max_seconds, 60);
    EXPECT_EQ(cfg->record.quota_mb, 100);
}

TEST(Config, RecordRejectsBadValues)
{
    for (const char *bad : {"dir = events", "dir =", "pre_seconds = 0", "pre_seconds = 31", "post_seconds = 0",
                            "max_seconds = 10", "quota_mb = 10", "enabled = 1"}) {
        std::vector<Error> errors;
        EXPECT_FALSE(config::load(std::string{kMinimal} + "[record]\n" + bad + "\n", errors)) << bad;
    }
}

TEST(Config, RtspDefaultsAndFields)
{
    std::vector<Error> errors;
    const auto def = config::load(kMinimal, errors);
    ASSERT_TRUE(def) << dump(errors);
    EXPECT_TRUE(def->rtsp.enabled);
    EXPECT_EQ(def->rtsp.address, "0.0.0.0");
    EXPECT_EQ(def->rtsp.port, 8554);
    EXPECT_EQ(def->rtsp.path, "/live");
    EXPECT_EQ(def->rtsp.credentials, "/etc/rk3588-security/rtsp_credentials");

    const auto cfg = config::load(std::string{kMinimal} + "[rtsp]\nenabled = false\naddress = 127.0.0.1\nport = 9554\n"
                                                          "path = /cam/door-1\ncredentials = /tmp/c\n",
                                  errors);
    ASSERT_TRUE(cfg) << dump(errors);
    EXPECT_FALSE(cfg->rtsp.enabled);
    EXPECT_EQ(cfg->rtsp.address, "127.0.0.1");
    EXPECT_EQ(cfg->rtsp.port, 9554);
    EXPECT_EQ(cfg->rtsp.path, "/cam/door-1");
    EXPECT_EQ(cfg->rtsp.credentials, "/tmp/c");
}

TEST(Config, RtspRejectsBadValues)
{
    for (const char *bad :
         {"address = localhost", "address = 1.2.3", "port = 554", "port = 70000", "path = live", "path = /",
          "path = /a b", "path = /直播", "credentials = rtsp_credentials", "enabled = on"}) {
        std::vector<Error> errors;
        EXPECT_FALSE(config::load(std::string{kMinimal} + "[rtsp]\n" + bad + "\n", errors)) << bad;
    }
}

TEST(Config, DisplayIdleTimeout)
{
    std::vector<Error> errors;
    const auto defaults = config::load(kMinimal, errors);
    ASSERT_TRUE(defaults) << dump(errors);
    EXPECT_EQ(defaults->display.idle_timeout_seconds, 20);
    const auto disabled = config::load("[camera]\ndevice=/dev/video0\n[display]\nidle_timeout_seconds=0\n", errors);
    ASSERT_TRUE(disabled) << dump(errors);
    EXPECT_EQ(disabled->display.idle_timeout_seconds, 0);
    EXPECT_FALSE(config::load("[camera]\ndevice=/dev/video0\n[display]\nidle_timeout_seconds=-1\n", errors));
}
