#!/usr/bin/env python3
"""从字体集合（.ttc）里取出一个字体存成单独的 .otf/.ttf，供 lv_font_conv 使用。

    extract_face.py 输入.ttc "Noto Sans CJK SC" 输出.otf

Noto Sans CJK 的 .ttc 里有日、韩、简中、繁中、港五个字体，同一个汉字字形不完全一样
（比如“关”“说”在日文字体里的写法），必须选简体中文（SC）那一个。
输入本身就是单个字体（.otf/.ttf）时直接复制。
"""
import shutil
import sys

from fontTools.ttLib import TTCollection, TTFont


def family(font):
    name = font["name"]
    return name.getDebugName(1) or ""


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    src, want, dst = sys.argv[1:]
    if not src.lower().endswith(".ttc"):
        shutil.copyfile(src, dst)
        return 0
    coll = TTCollection(src)
    seen = []
    for font in coll.fonts:
        fam = family(font)
        seen.append(fam)
        if fam == want:
            font.save(dst)
            return 0
    print(f"{src} 里没有字体 {want!r}，有：{seen}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
