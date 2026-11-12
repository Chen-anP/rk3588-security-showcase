#!/usr/bin/env python3
"""输出 GB2312 一级汉字（3755 个常用字，按拼音排序），一行。

人名用字表（第 7d 步）：登记的名字是运行时才知道的，界面字体事先放进这些常用字，
名字里的字都在这张表里才允许登记。字表由编码规则直接算出来，不用下载：
GB2312 第 16～55 区（首字节 0xB0～0xD7、次字节 0xA1～0xFE）是一级汉字，第 55 区只到 0xF9。
"""
import sys


def main():
    chars = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            if hi == 0xD7 and lo > 0xF9:
                break
            chars.append(bytes([hi, lo]).decode("gb2312"))
    assert len(chars) == 3755, len(chars)
    print("".join(chars))
    return 0


if __name__ == "__main__":
    sys.exit(main())
