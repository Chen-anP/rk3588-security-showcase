#include "core/instance_lock.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>

using rksec::InstanceLock;

namespace {

std::string temp_path()
{
    return ::testing::TempDir() + "rksec_lock_" + std::to_string(::getpid()) + "_" +
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
}

}  // namespace

TEST(InstanceLock, SecondInstanceIsRejectedWithPid)
{
    const std::string path = temp_path();
    std::string err;
    InstanceLock first;
    ASSERT_TRUE(first.acquire(path, err)) << err;

    // flock 按“打开的文件”算，同一个进程里再打开一次也会冲突，正好模拟第二个实例
    InstanceLock second;
    EXPECT_FALSE(second.acquire(path, err));
    EXPECT_NE(err.find("已经有一个实例在运行"), std::string::npos) << err;
    EXPECT_NE(err.find(std::to_string(::getpid())), std::string::npos) << "报错里要有占着锁的 pid：" << err;
    std::remove(path.c_str());
}

TEST(InstanceLock, ReleasedWhenOwnerGoesAway)
{
    const std::string path = temp_path();
    std::string err;
    {
        InstanceLock first;
        ASSERT_TRUE(first.acquire(path, err)) << err;
    }  // 析构 = 进程退出（崩溃时内核同样会关掉文件、释放锁）
    InstanceLock again;
    EXPECT_TRUE(again.acquire(path, err)) << "上一个实例退出后要能再启动：" << err;
    std::remove(path.c_str());
}

TEST(InstanceLock, StaleFileFromCrashDoesNotBlock)
{
    // 崩溃留下的锁文件（里面有旧 pid）不能挡住新实例：锁靠 flock，不靠文件存在与否
    const std::string path = temp_path();
    std::ofstream(path) << "99999\n";
    InstanceLock lock;
    std::string err;
    EXPECT_TRUE(lock.acquire(path, err)) << err;
    std::remove(path.c_str());
}

TEST(InstanceLock, UnwritableDirectoryIsAClearError)
{
    InstanceLock lock;
    std::string err;
    EXPECT_FALSE(lock.acquire("/nonexistent-dir/x.lock", err));
    EXPECT_NE(err.find("打不开锁文件"), std::string::npos) << err;
}
