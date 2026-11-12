#pragma once

#include <string>

// 单实例锁：同一时间只允许一个本程序在用摄像头和屏幕。
//
// 为什么需要（D18）：第 4b 步上板时开了第二个 --ui，它没有被拒绝——
// 第二个实例也设置显示成功，把第一个实例的翻页挤失败（EBUSY）；摄像头倒是被独占，第二个一直在重试。
// 不能指望设备自己互斥，所以程序启动时先拿锁。
//
// 做法：flock() 锁一个文件（默认 /run/lock/rk3588-security.lock，Ubuntu 上所有用户可写）。
// - 进程退出（包括崩溃、kill -9）时内核自动释放锁，不会留下“锁死”的文件，下次照常启动
// - 文件里写拿锁进程的 pid，第二个实例报错时告诉用户是谁占着
namespace rksec {

class InstanceLock {
  public:
    InstanceLock() = default;
    ~InstanceLock();
    InstanceLock(const InstanceLock &) = delete;
    InstanceLock &operator=(const InstanceLock &) = delete;

    // 拿到锁返回 true。已有实例在运行、或者锁文件打不开，返回 false，err 是给人看的原因
    bool acquire(const std::string &path, std::string &err);

  private:
    int fd_ = -1;
};

}  // namespace rksec
