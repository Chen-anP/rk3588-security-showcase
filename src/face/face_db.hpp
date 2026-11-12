#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "face/gallery.hpp"

struct sqlite3;

// 人脸库（第 7d 步）：登记的人和他们的人脸特征，存在一个 SQLite 文件里。纯 CPU，CI 上测。
//
// 表结构（PRAGMA user_version）：
//   版本 1（第 7d 步）：person(id, name UNIQUE, created)
//                      face(id, person_id → person，删人时一起删, model, embedding, aligned, source, created)
//   版本 2（第 8b 步）：加 event 表（事件录像，见 record/event_store.hpp）。人脸库和事件在同一个文件里：
//                      一个数据文件好备份、好管权限
//   版本 3（第 9d-2 步）：加 passage 表（通行记录，见 access/pass_store.hpp）
// 老版本的文件打开时自动升级（只加表，不动已有数据）。
//
// - embedding：512 个 float32，小端（板子和电脑都是小端）直接存成 BLOB
// - model：算这个特征用的识别模型（模型文件名）。换了模型，旧特征不能和新特征比，加载时跳过并提示
//   运行 --face-reembed，用 aligned 重新算
// - aligned：对齐后的 112×112 人脸（JPEG，约 5 KB）。不存原图：个人数据只留必要的；
//   有它就能在换模型时重新算特征，以后界面的人员管理页也能显示头像
//
// 文件新建时权限设为 0600（只有运行程序的用户能读）：人脸特征属于生物特征数据。
// 几个进程同时用（界面在跑，另一个终端在登记）时，SQLite 自己加锁；写的一方最多等 2 秒。
// 界面只在启动时读一次，登记以后要重启界面才生效（以后做界面登记时改成通知刷新）。
namespace rksec::face {

// 名字的规则：1～16 个字，不能有控制字符和 / \ " 这些符号（会出现在日志、命令行、文件名里）。
// 字体里有没有这些字由调用方另外检查（界面库）
bool check_name(const std::string &name, std::string &err);

// 数据库文件现在的格式版本
constexpr int kDatabaseVersion = 3;

// 打开程序的数据库文件：没有就新建（连同上级目录，文件权限 0600），建表，老格式升级到 kDatabaseVersion。
// 更新版本的程序建的文件（版本号更大）拒绝打开。成功返回连接（调用方 sqlite3_close），失败返回 nullptr。
// 人脸库（FaceDb）和事件表（EventStore）都用它打开，格式只在这一个地方管
sqlite3 *open_database(const std::string &path, std::string &err);

// 识别模型的标识：模型文件名（不含目录）。文件名里有模型、输入尺寸、精度、工具版本（ADR-05），
// 换模型一定换文件名
std::string model_key(const std::string &model_path);

struct NewFace {
    std::vector<float> embedding;       // 归一化的特征
    std::vector<std::uint8_t> aligned;  // 对齐后的人脸 JPEG
    std::string source;                 // 从哪来的（照片路径、camera），查问题用
};

struct PersonInfo {
    std::int64_t id = 0;
    std::string name;
    std::string created;
    int faces = 0;        // 照片张数
    int other_model = 0;  // 其中特征是别的模型算的张数（要重新算）
};

struct StoredFace {
    std::int64_t id = 0;
    std::string name;
    std::string model;
    std::vector<std::uint8_t> aligned;
};

class FaceDb {
  public:
    // 打开（没有就新建，连同上级目录）。文件是更新版本的程序建的（user_version 更大）时拒绝打开
    static std::unique_ptr<FaceDb> open(const std::string &path, std::string &err);
    ~FaceDb();
    FaceDb(const FaceDb &) = delete;
    FaceDb &operator=(const FaceDb &) = delete;

    // 给 name 加几张脸（人不存在就新建），在一个事务里：要么都加上，要么都不加
    bool add(const std::string &name, const std::vector<NewFace> &faces, const std::string &model, std::string &err);

    // 删除一个人和他的所有照片。removed：删掉的照片张数，人不存在时是 -1（也返回 true）
    bool remove(const std::string &name, int &removed, std::string &err);

    // 所有人，按名字排序。model：当前的识别模型，用来数“要重新算的”
    bool people(const std::string &model, std::vector<PersonInfo> &out, std::string &err);

    // 把 model 算的特征都加进底库；别的模型算的跳过，个数记在 skipped
    bool load(const std::string &model, Gallery &gallery, int &skipped, std::string &err);

    // 换模型以后重新算特征：列出所有照片，算好以后 update
    bool faces(std::vector<StoredFace> &out, std::string &err);
    bool update(std::int64_t face_id, const std::vector<float> &embedding, const std::string &model, std::string &err);

    const std::string &path() const { return path_; }

    struct Impl;

  private:
    FaceDb();
    std::unique_ptr<Impl> impl_;
    std::string path_;
};

}  // namespace rksec::face
