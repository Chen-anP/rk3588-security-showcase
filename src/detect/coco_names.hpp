#pragma once

#include <array>
#include <string_view>

// COCO 数据集的 80 个类别名（YOLO11 官方模型按这个顺序输出），第 5d 步。
// 只用到 person（0），其他类别留着：配置文件可以选，--detect-image 对照官方 demo 时全部显示。
namespace rksec::detect {

inline constexpr std::array<std::string_view, 80> kCocoNames = {
    "person",         "bicycle",    "car",           "motorcycle",    "airplane",     "bus",           "train",
    "truck",          "boat",       "traffic light", "fire hydrant",  "stop sign",    "parking meter", "bench",
    "bird",           "cat",        "dog",           "horse",         "sheep",        "cow",           "elephant",
    "bear",           "zebra",      "giraffe",       "backpack",      "umbrella",     "handbag",       "tie",
    "suitcase",       "frisbee",    "skis",          "snowboard",     "sports ball",  "kite",          "baseball bat",
    "baseball glove", "skateboard", "surfboard",     "tennis racket", "bottle",       "wine glass",    "cup",
    "fork",           "knife",      "spoon",         "bowl",          "banana",       "apple",         "sandwich",
    "orange",         "broccoli",   "carrot",        "hot dog",       "pizza",        "donut",         "cake",
    "chair",          "couch",      "potted plant",  "bed",           "dining table", "toilet",        "tv",
    "laptop",         "mouse",      "remote",        "keyboard",      "cell phone",   "microwave",     "oven",
    "toaster",        "sink",       "refrigerator",  "book",          "clock",        "vase",          "scissors",
    "teddy bear",     "hair drier", "toothbrush",
};

// 类别名 → 编号；没有返回 -1
inline int coco_id(std::string_view name)
{
    for (std::size_t i = 0; i < kCocoNames.size(); ++i) {
        if (kCocoNames[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 编号 → 类别名；越界返回 "?"
inline std::string_view coco_name(int id)
{
    return id >= 0 && id < static_cast<int>(kCocoNames.size()) ? kCocoNames[static_cast<std::size_t>(id)]
                                                               : std::string_view{"?"};
}

}  // namespace rksec::detect
