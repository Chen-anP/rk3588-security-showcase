#include "media/h264_stream.hpp"

namespace rksec::media {

namespace {
constexpr std::uint8_t kNalTypeMask = 0x1F;
}

std::vector<NalUnit> split_annexb(const std::uint8_t *data, std::size_t size)
{
    std::vector<NalUnit> out;
    if (data == nullptr) {
        return out;
    }
    // 先找出所有起始码 00 00 01 的位置（00 00 00 01 的前一个 00 算进上一个 NAL 的尾部，下面去掉）
    std::vector<std::size_t> starts;  // 起始码后面第一个字节的位置
    for (std::size_t i = 0; i + 3 <= size;) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            starts.push_back(i + 3);
            i += 3;
        } else {
            ++i;
        }
    }
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::size_t begin = starts[k];
        // 这个 NAL 的结尾：下一个起始码的开头（begin 的下一个起始码位置 - 3）；最后一个到数据结尾
        std::size_t end = k + 1 < starts.size() ? starts[k + 1] - 3 : size;
        while (end > begin && data[end - 1] == 0) {
            --end;  // 去掉 4 字节起始码多出来的 00 和尾部填充
        }
        if (end <= begin) {
            continue;  // 空 NAL
        }
        out.push_back({begin, end - begin, data[begin] & kNalTypeMask});
    }
    return out;
}

bool has_idr(const std::uint8_t *data, std::size_t size)
{
    for (const auto &n : split_annexb(data, size)) {
        if (n.type == kNalIdr) {
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> extract_parameter_sets(const std::uint8_t *data, std::size_t size)
{
    std::vector<std::uint8_t> out;
    for (const auto &n : split_annexb(data, size)) {
        if (n.type == kNalSps || n.type == kNalPps) {
            out.insert(out.end(), {0, 0, 0, 1});
            out.insert(out.end(), data + n.start, data + n.start + n.size);
        }
    }
    return out;
}

}  // namespace rksec::media
