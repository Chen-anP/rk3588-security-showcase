#pragma once

#include <cstdint>

// 实时流的关键帧门（第 8c 步）：新的观看者（或者推流出了问题以后）要从关键帧开始。纯逻辑，CI 上测。
//
// 从中间的 P 帧开始解码，播放器开头是花的（8a 拖进度条时看到过）。所以每次 reset() 以后，
// 先扔掉包，等到下一个关键帧才放行，之后全部放行。代价：新客户端最多等一个 GOP（约 2 秒）才出画面。
namespace rksec::stream {

class KeyframeGate {
  public:
    // 这个包能不能推出去
    bool pass(bool keyframe)
    {
        if (!open_ && !keyframe) {
            ++skipped_;
            return false;
        }
        open_ = true;
        return true;
    }
    // 下一个关键帧之前都不放行（新的推流开始、积压太多丢过包）
    void reset() { open_ = false; }
    bool open() const { return open_; }
    std::uint64_t skipped() const { return skipped_; }

  private:
    bool open_ = false;
    std::uint64_t skipped_ = 0;
};

}  // namespace rksec::stream
