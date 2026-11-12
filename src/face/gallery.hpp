#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// 人脸特征比对（第 7c 步）。纯 CPU 计算，CI 上能测。
//
// 识别模型把一张对齐好的脸变成 512 个数（特征向量，embedding）。同一个人的两张脸，向量方向接近；
// 不同人的方向差得远。比较两个向量用余弦相似度（两个向量夹角的余弦，-1～1）：
// 先把向量长度归一化成 1，余弦相似度就是对应元素乘起来再相加（点积）。
//
// 底库（gallery）：登记过的人，每人一个或几张照片的特征。来一张脸，和底库里每个人比，
// 取最像的那个人；分数达到阈值才算认出来，否则是陌生人。阈值要用实际的照片测出来
// （同一个人的分数大致在多少、不同人的分数大致在多少，见第 7c 步上板记录）。
//
// 底库在启动时从人脸库（SQLite，FaceDb）读出来，放在内存里，识别时只读。
namespace rksec::face {

inline constexpr std::size_t kEmbeddingSize = 512;

// 归一化成长度 1，返回原来的长度（全 0 时返回 0，向量不变）
float l2_normalize(std::vector<float> &v);

// 两个已经归一化的向量的余弦相似度（点积）。长度不同返回 -1
float cosine(const std::vector<float> &a, const std::vector<float> &b);

struct Match {
    std::string name;  // 最像的人（底库为空时是空串）
    float score = -1;  // 和这个人的相似度（这个人有几张照片时取最高的一张）
    float second = -1;  // 第二像的人的相似度（只有一个人时是 -1）。两个分数很接近说明不可靠
    std::int64_t person_id = 0;
    bool matched = false;  // score ≥ 阈值
};

class Gallery {
  public:
    // 登记一张照片的特征（会归一化）。同名的可以登记多张（不同角度、光线）
    void add(const std::string &name, std::vector<float> embedding, std::int64_t person_id = 0);

    // 和底库里每个人比，embedding 要已经归一化
    Match match(const std::vector<float> &embedding, float threshold) const;

    // 每个人的分数（他所有照片里最高的一张），登记时查“是不是和别人很像”用
    std::map<std::string, float> scores(const std::vector<float> &embedding) const;

    std::size_t people() const;                           // 人数（同名算一个）
    std::size_t size() const { return entries_.size(); }  // 照片张数
    bool empty() const { return entries_.empty(); }

  private:
    struct Entry {
        std::string name;
        std::vector<float> embedding;
        std::int64_t person_id = 0;
    };
    std::vector<Entry> entries_;
};

// 照片文件名 → 人名：去掉目录和扩展名，再去掉结尾的“_数字”。
// “赵小明.jpg”“赵小明_2.jpg”都是“赵小明”；“demo_user.jpg”是“demo_user”
std::string name_from_file(const std::string &path);

}  // namespace rksec::face
