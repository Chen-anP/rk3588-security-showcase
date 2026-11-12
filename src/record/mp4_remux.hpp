#pragma once

#include <cstdint>
#include <string>

// 把录像（分段 MP4）转成普通 MP4（第 8d 步，导出用）。用 libavformat 原样搬运压缩数据，不重新编码，
// 画质不变，几十 MB 的文件不到一秒。CI 上测。
//
// 为什么要转：录的时候用分段 MP4（断电不坏，见 mp4_writer.hpp），索引分散在每一段里。
// VLC 能播，但有的播放器（比如 Windows 自带的）拖进度条不好用。普通 MP4 只有一个完整索引（moov），
// 再用 faststart 把索引挪到文件开头：任何播放器都能拖，网页、聊天软件里也能边下边播。
//
// 写法：先写 out + ".part"，写完 fsync，再改名成 out、fsync 目录。中途失败删掉 .part，不留半个文件。
// out 已经存在时不覆盖，返回失败。输出文件权限 0600（录像里有人）。
//
// “不完整”的录像（程序被杀时正在录的）也能转：读到坏掉的尾巴就停，前面完整的部分照样导出，truncated = true。
namespace rksec::record {

struct RemuxInfo {
    std::int64_t packets = 0;
    std::int64_t keyframes = 0;
    std::int64_t duration_us = 0;  // 最后一帧的时间 + 一帧的时长
    int width = 0;
    int height = 0;
    bool truncated = false;  // 输入在结尾处读出错（不完整的录像），只导出了前面能读的部分
};

bool remux_to_faststart(const std::string &in, const std::string &out, RemuxInfo &info, std::string &err);

}  // namespace rksec::record
