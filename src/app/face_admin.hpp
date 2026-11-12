#pragma once

#include <string>
#include <vector>

#include "core/config.hpp"
#include "face/gallery.hpp"

// 人脸库的命令（第 7d 步）：登记、列出、删除、换模型后重新算特征。
// 人脸库是 face.database 指向的 SQLite 文件（face/face_db.hpp）。
//
// 退出码：0 成功；1 不能登记（照片不合格、像别人、名字不行），原因已打印；
// 2 这次编译没有 RKNN；4 摄像头、模型或人脸库打不开
namespace rksec::app {

// 打开人脸库，把当前识别模型算的特征读进 gallery（流水线启动时用）。写日志：几个人、几张照片、
// 有几张是别的模型算的（要 --face-reembed）
bool load_gallery(const config::AppConfig &cfg, face::Gallery &gallery, std::string &err);

// --enroll 名字 [照片…]：
// - 没给照片：用摄像头取 3 秒左右的画面，挑 3 张（只有一个人、脸够大、角度不同）登记。要拿单实例锁
// - 给了照片：每张照片里只能有一张脸，全部合格才登记
// 登记前检查：名字的字在界面字体里；几张像同一个人；不像已登记的别人
int face_enroll(const config::AppConfig &cfg, const std::string &name, const std::vector<std::string> &photos);

// --faces：列出登记的人、照片张数、登记时间
int face_list(const config::AppConfig &cfg);

// --unenroll 名字：删除这个人和他的所有照片
int face_unenroll(const config::AppConfig &cfg, const std::string &name);

// --face-reembed：换了识别模型以后，用库里存的对齐小图重新算所有特征
int face_reembed(const config::AppConfig &cfg);

}  // namespace rksec::app
