#!/usr/bin/env python3
"""收集界面要用到的非 ASCII 字符（汉字、中文标点），去重排序后输出一行。

    collect_chars.py src/ui/text.hpp tools/fonts/extra_chars.txt

- 源文件：取所有双引号字符串里的字符（注释里的字不算）
- 文本文件：# 开头的行是注释，其余行的每个字符都算
"""
import re
import sys


def main():
    chars = set()
    for path in sys.argv[1:]:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        if path.endswith((".hpp", ".cpp", ".h")):
            for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', text):
                chars.update(lit)
        else:
            for line in text.splitlines():
                if not line.startswith("#"):
                    chars.update(line.strip())
    print("".join(sorted(c for c in chars if ord(c) > 0x7E)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
