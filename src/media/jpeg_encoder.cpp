#include "media/jpeg_encoder.hpp"

#include <turbojpeg.h>

namespace rksec::media {

bool encode_rgb_jpeg(const std::uint8_t *rgb, int width, int height, int quality, std::vector<std::uint8_t> &out,
                     std::string &err)
{
    tjhandle h = tjInitCompress();
    if (h == nullptr) {
        err = "tjInitCompress 失败";
        return false;
    }
    unsigned char *buf = nullptr;
    unsigned long size = 0;
    const int rc = tjCompress2(h, rgb, width, 0, height, TJPF_RGB, &buf, &size, TJSAMP_444, quality, 0);
    if (rc != 0) {
        err = std::string{"JPEG 编码失败："} + tjGetErrorStr2(h);
    } else {
        out.assign(buf, buf + size);
    }
    tjFree(buf);
    tjDestroy(h);
    return rc == 0;
}

}  // namespace rksec::media
