#pragma once

#include <string>
#include <vector>

#include "core/config.hpp"
#include "face/gallery.hpp"
#include "face/retinaface.hpp"
#include "media/jpeg_decoder.hpp"

// 人脸识别的工具（第 7c 步）：单张图片算特征、--face-embed。
namespace rksec::face {
class FaceEngine;
}

namespace rksec::app {

// 一张图里最大的那张脸
struct ImageFace {
    int faces = 0;                      // 检测到几张脸（0 = 没有，其他字段无效）
    face::Face face;                    // 最大的那张（原图坐标）
    std::vector<float> embedding;       // 归一化的特征
    std::vector<std::uint8_t> aligned;  // 对齐后的 112×112 RGB
    float raw_norm = 0;                 // 特征归一化之前的长度
};

// 读 JPEG 文件、CPU 全尺寸解码（XRGB）
bool decode_jpeg_file(const std::string &path, media::Image &img, std::string &err);

#ifdef RKSEC_HAVE_RKNN
// 整张图 CPU letterbox 到 320 找脸，取最大的，在原图上对齐（原图分辨率高）、算特征。
// 返回 false 是出错（err）；没有脸返回 true、out.faces = 0
bool embed_image(face::FaceEngine &engine, const media::Image &img, ImageFace &out, std::string &err);

#endif

// --face-embed 图片…：每张图找最大的脸、算特征，打印分数、特征长度；两张以上时打印两两的相似度
// （同一个人应该高、不同人应该低——用来定阈值）。
// save_dir 非空时每张图存“名字.aligned.ppm”（对齐后的 112×112）和“名字.emb.txt”（512 个数），
// 拿到电脑上用 tools/model-convert/check_mobilefacenet.py 和 onnxruntime 的结果对照。
// 返回 0 成功；4 模型或图片打不开；2 这次编译没有 RKNN
int face_embed(const config::AppConfig &cfg, const std::vector<std::string> &paths, const std::string &save_dir);

}  // namespace rksec::app
