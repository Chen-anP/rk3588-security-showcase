#include "media/jpeg_decoder.hpp"

#include <turbojpeg.h>

namespace rksec::media {

void scaled_size(int width, int height, int max_width, int &out_w, int &out_h)
{
    int n = 0;
    const tjscalingfactor *f = tjGetScalingFactors(&n);
    // 缩放档从大到小排列（2/1、15/8 … 1/8）；只考虑缩小或原尺寸
    bool found = false;
    int best_w = 0;
    int best_h = 0;
    int min_w = width;
    int min_h = height;
    for (int i = 0; i < n; ++i) {
        if (f[i].num > f[i].denom) {
            continue;
        }
        const int w = TJSCALED(width, f[i]);
        const int h = TJSCALED(height, f[i]);
        if (w < min_w) {
            min_w = w;
            min_h = h;
        }
        if (w <= max_width && w > best_w) {
            best_w = w;
            best_h = h;
            found = true;
        }
    }
    out_w = found ? best_w : min_w;
    out_h = found ? best_h : min_h;
}

JpegDecoder::JpegDecoder() : handle_(tjInitDecompress()) {}

JpegDecoder::~JpegDecoder()
{
    if (handle_ != nullptr) {
        tjDestroy(handle_);
    }
}

bool JpegDecoder::decode(const std::uint8_t *data, std::size_t size, int max_width, Image &out, std::string &err)
{
    if (handle_ == nullptr) {
        err = "libjpeg-turbo 初始化失败";
        ++stats_.failed;
        return false;
    }
    if (data == nullptr || size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        err = "不是 JPEG 数据（开头不是 FF D8）";
        ++stats_.failed;
        return false;
    }
    int w = 0;
    int h = 0;
    int subsamp = 0;
    int colorspace = 0;
    if (tjDecompressHeader3(handle_, data, static_cast<unsigned long>(size), &w, &h, &subsamp, &colorspace) != 0) {
        err = std::string{"JPEG 头解析失败："} + tjGetErrorStr2(handle_);
        ++stats_.failed;
        return false;
    }
    int ow = 0;
    int oh = 0;
    scaled_size(w, h, max_width, ow, oh);
    const std::size_t stride = static_cast<std::size_t>(ow) * 4;
    out.pixels.resize(stride * static_cast<std::size_t>(oh));  // 尺寸不变时不会重新分配
    out.width = ow;
    out.height = oh;
    out.stride = stride;

    // TJFLAG_FASTDCT：快速整数 DCT，精度损失肉眼看不出，预览够用
    if (tjDecompress2(handle_, data, static_cast<unsigned long>(size), out.pixels.data(), ow, static_cast<int>(stride),
                      oh, TJPF_BGRX, TJFLAG_FASTDCT) != 0) {
        if (tjGetErrorCode(handle_) == TJERR_WARNING) {
            ++stats_.warnings;
            ++stats_.ok;
            return true;
        }
        err = std::string{"JPEG 解码失败："} + tjGetErrorStr2(handle_);
        ++stats_.failed;
        return false;
    }
    ++stats_.ok;
    return true;
}

}  // namespace rksec::media
