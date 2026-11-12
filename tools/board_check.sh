#!/usr/bin/env bash
# board_check.sh —— 板级自检：逐项打印 PASS / FAIL，全部通过时退出码为 0
# 用法：sudo ./board_check.sh            （第 2 步放进仓库 tools/，以后发展成产测程序）
#
# 两个踩过的坑（调试记录素材）：
#  1) 管道会吞掉前一个命令的失败  -> 每项都用 bash -o pipefail
#  2) pipefail 下，右边是 head -1 / grep -m1 这种“读到就退出”的命令时，
#     左边会被 SIGPIPE 杀掉（退出码 141），明明找到了也判 FAIL
#     -> 不在管道里提前退出：先完整读完存进变量，再判断
# 每一项检查都是把单引号里的命令原样交给 bash -c 执行，变量要在那里才展开，
# 所以 shellcheck 的 SC2016（单引号里的变量不展开）在这个文件里是故意的。
# shellcheck disable=SC2016
set -u
FAIL=0
check() {                     # check "名称" 命令 参数...
  local name="$1"; shift
  local out
  if out=$("$@" 2>&1 | tr '\n' ' '; exit "${PIPESTATUS[0]}"); then printf 'PASS  %-16s %s\n' "$name" "$out"
  else printf 'FAIL  %-16s %s\n' "$name" "$out"; FAIL=$((FAIL + 1)); fi
}
[ "$(id -u)" -eq 0 ] || { echo "请用 sudo 运行（NPU 信息在 debugfs 里，需要 root）"; exit 2; }

RKNNRT_EXPECT="2.3.2"         # 本项目锁定的运行时版本

check "内核 6.1"      bash -o pipefail -c 'uname -r | grep -E "^6\.1\."'
check "NPU 驱动"      bash -o pipefail -c 'cat /sys/kernel/debug/rknpu/version'
check "RGA 设备"      bash -o pipefail -c 'ls /dev/rga'
check "MPP 设备"      bash -o pipefail -c 'ls /dev/mpp_service'
check "显示设备"      bash -o pipefail -c 'ls /dev/dri/card*'
check "HDMI 已接屏"   bash -o pipefail -c 's=$(cat /sys/class/drm/card*-HDMI-A-1/status 2>/dev/null); echo "${s:-没有 HDMI-A-1}"; [ "$s" = connected ]'
check "屏幕时序参数"  bash -o pipefail -c 'v=$(tr " " "\n" </proc/cmdline | grep "^video=HDMI-A-1:"); echo "${v:-启动参数里没有 video=HDMI-A-1}"; [[ "$v" == *MR@* ]]'
check "触摸屏"        bash -o pipefail -c 'ls /dev/input/by-id/*CTP*-event-if00'
check "USB 摄像头"    bash -o pipefail -c 'ls /dev/v4l/by-id/*-video-index0'
check "librknnrt $RKNNRT_EXPECT" bash -o pipefail -c '
                        f=/usr/lib/librknnrt.so; [ -f "$f" ] || { echo "没有 $f"; exit 1; }
                        v=$(strings "$f" | grep -i "librknnrt version"); echo "$v"
                        case "$v" in *"version: '"$RKNNRT_EXPECT"'"*) ;; *) exit 1 ;; esac'
check "根分区剩余>2G" bash -o pipefail -c 'a=$(df --output=avail -BG / | tail -1 | tr -dc 0-9); echo "${a}G"; [ "$a" -gt 2 ]'
check "温度<70℃"      bash -o pipefail -c 't=$(cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | sort -n | tail -1); [ -n "$t" ] || { echo "找不到温度传感器"; exit 1; }; echo "$((t / 1000))℃"; [ "$t" -lt 70000 ]'

echo "----"
if [ "$FAIL" -eq 0 ]; then echo "ALL PASS"; else echo "$FAIL 项失败"; fi
exit "$FAIL"
