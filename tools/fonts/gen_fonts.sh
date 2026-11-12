#!/usr/bin/env bash
# gen_fonts.sh —— 生成界面用的中文字体（LVGL 位图字体，C 源文件，提交进仓库）
#
# 用法（在仓库根目录，电脑上或板子上都行）：
#   tools/fonts/gen_fonts.sh [源字体文件]
#   源字体默认 /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc（sudo apt install fonts-noto-cjk）
# 需要：python3-fonttools（sudo apt install python3-fonttools）、Node.js（npx 自动下载 lv_font_conv）
#   没有网络时，可以用环境变量 LV_FONT_CONV 指定本地的 lv_font_conv，比如 LV_FONT_CONV="node ~/lv_font_conv/lv_font_conv.js"
#
# 什么时候运行：界面加了新字（src/ui/text.hpp 或 extra_chars.txt），测试 UiText.EveryCharacterIsInTheFont 失败时。
# 生成三个文件：界面文字 20 号、28 号，人名用字 20 号（GB2312 一级汉字，第 7d 步）。
#
# 为什么不直接用 LVGL 自带的中文字体：它只有 16 号一种大小、1000 个字，“设置”“记录”“摄像头”这些都没有。
# 为什么不在板子上运行时渲染（FreeType）：多一个依赖、启动时要加载十几 MB 的字体文件；
# 界面上的字是固定的，事先把用到的字转成位图，只有几十 KB，显示效果也确定。
set -euo pipefail
cd "$(dirname "$0")/../.."

FONT=${1:-/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc}
FACE="Noto Sans CJK SC"
SIZES=(20 28)
OUT=src/ui/fonts
CONV=${LV_FONT_CONV:-npx --yes lv_font_conv@1.5.3}

[ -f "$FONT" ] || { echo "找不到源字体 $FONT（sudo apt install fonts-noto-cjk，或者在参数里指定）" >&2; exit 2; }
want=$(sed 's/#.*//' tools/fonts/fonts.sha256 | awk 'NF {print $1; exit}')
have=$(sha256sum "$FONT" | awk '{print $1}')
if [ "$have" != "$want" ]; then
    echo "警告：源字体和锁定的不一样（$have），生成的字形可能和原来略有差别，生成后看一眼界面截图"
fi

CHARS=$(python3 tools/fonts/collect_chars.py src/ui/text.hpp tools/fonts/extra_chars.txt)
echo "字符：$(printf '%s' "$CHARS" | python3 -c 'import sys; print(len(sys.stdin.read()))') 个非 ASCII 字符 + ASCII 0x20-0x7E"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
python3 tools/fonts/extract_face.py "$FONT" "$FACE" "$TMP/NotoSansCJKsc-Regular.otf"

mkdir -p "$OUT"
for size in "${SIZES[@]}"; do
    name="rksec_font_cn_$size"
    # 在临时目录里运行，生成文件开头记录的命令行里只有相对路径，每次生成结果一致。
    # --bpp 4：16 级灰度抗锯齿；--no-compress：不压缩，显示时不用解压（字少，文件大一点无所谓）
    (cd "$TMP" && $CONV --font NotoSansCJKsc-Regular.otf --size "$size" --bpp 4 --no-compress --format lvgl \
        -r 0x20-0x7E --symbols "$CHARS" --lv-font-name "$name" -o "$name.c")
    {
        echo "/* 由 tools/fonts/gen_fonts.sh 生成，不要手工修改。源字体 sha256 $have（$FACE） */"
        cat "$TMP/$name.c"
    } >"$OUT/$name.c"
    echo "生成 $OUT/$name.c（$(wc -c <"$OUT/$name.c") 字节）"
done

# 人名用字（第 7d 步）：GB2312 一级汉字 3755 个，只做 20 号（检测框的标签）。
# 界面字体找不到的字再到这里找（LVGL 的 fallback），登记时名字里的字必须都在这里或界面字体里。
# 单独一个文件：界面文字改了它不变，重新生成时不会产生几 MB 的差异
NAMES=$(python3 tools/fonts/gb2312_level1.py)
name="rksec_font_names_20"
(cd "$TMP" && $CONV --font NotoSansCJKsc-Regular.otf --size 20 --bpp 4 --no-compress --format lvgl \
    --symbols "$NAMES" --lv-font-name "$name" -o "$name.c")
{
    echo "/* 由 tools/fonts/gen_fonts.sh 生成，不要手工修改。GB2312 一级汉字 3755 个（人名用字）。源字体 sha256 $have（$FACE） */"
    cat "$TMP/$name.c"
} >"$OUT/$name.c"
echo "生成 $OUT/$name.c（$(wc -c <"$OUT/$name.c") 字节）"
