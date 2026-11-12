#!/usr/bin/env bash
# test_provision.sh —— 在普通电脑上测试装机脚本，不需要板子、不需要 root。
#
# 做法：RKSEC_ROOT 指向一个临时目录，装机脚本读写的系统文件（/etc、/usr/lib、/home……）都在它下面；
# apt-get、systemctl、sshd 这些会改系统的命令换成假命令（放在 PATH 最前面），只把“被调用了”记到状态文件里。
# 这样可以验证：第一次会改哪些、第二次是不是“修改 0 项”、dry-run 是不是一点不改、出错时有没有退回。
#
# 用法：tests/provision/test_provision.sh      全部通过退出码 0
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
SCRIPT="$REPO/tools/provision/provision.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAILS=0
pass() {
    printf 'PASS  %s\n' "$1"
    PASS=$((PASS + 1))
}
bad() {
    printf 'FAIL  %s\n' "$1"
    FAILS=$((FAILS + 1))
}
expect() { # expect "说明" 条件命令...
    local what="$1"
    shift
    if "$@"; then pass "$what"; else bad "$what"; fi
}

# ---------- 假命令 ----------
FAKE="$WORK/fakebin"
mkdir -p "$FAKE"
fake() { # fake 名字 <<'脚本'
    {
        echo '#!/usr/bin/env bash'
        # 这一行是写进假命令的脚本内容，变量要在假命令运行时才展开，所以故意用单引号
        # shellcheck disable=SC2016
        echo 'S="$FAKE_STATE"'
        cat
    } >"$FAKE/$1"
    chmod 755 "$FAKE/$1"
}
fake dpkg-query <<'X'
pkg="${*: -1}"
grep -qx "$pkg" "$S/installed" || { echo "dpkg-query: no packages found matching $pkg" >&2; exit 1; }
# 在 held 文件里的包模拟镜像锁定（hold）的状态
if grep -qx "$pkg" "$S/held" 2>/dev/null; then st="hold ok installed"; else st="install ok installed"; fi
case "$*" in *Status*) echo "$st" ;; *) echo "1.0-fake" ;; esac
X
fake apt-get <<'X'
echo "apt-get $*" >>"$S/calls"
[ "$1" != update ] || [ "${FAKE_APT_UPDATE_RC:-0}" -eq 0 ] || { echo "E: The repository 'docker' is not signed."; exit 100; }
[ "${FAKE_APT_RC:-0}" -eq 0 ] || exit "$FAKE_APT_RC"
if [ "$1" = install ]; then shift; for p in "$@"; do [ "$p" = -y ] || echo "$p" >>"$S/installed"; done; fi
X
fake systemctl <<'X'
echo "systemctl $*" >>"$S/calls"
case "$1" in
    get-default) cat "$S/default" ;;
    set-default) echo "$2" >"$S/default" ;;
esac
X
fake sshd <<'X'
echo "sshd $*" >>"$S/calls"
exit "${FAKE_SSHD_RC:-0}"
X
fake getent <<'X'
grep -qx "$2" "$S/groups_exist" && echo "$2:x:44:"
X
fake id <<'X'
cat "$S/user_groups"
X
fake usermod <<'X'
echo "usermod $*" >>"$S/calls"
printf ' %s' "$2" >>"$S/user_groups"
X
fake apt-mark <<'X'
cat "$S/held" 2>/dev/null
X
fake ldconfig <<'X'
echo "ldconfig" >>"$S/calls"
X

# ---------- 一块“刚刷好系统”的假板子 ----------
new_board() { # new_board 目录
    local r="$1"
    mkdir -p "$r/etc/ssh/sshd_config.d" "$r/etc/profile.d" "$r/usr/lib" "$r/usr/include" "$r/proc/device-tree" \
        "$r/home/orangepi/.ssh" "$r/state" "$r/boot"
    printf 'ID=ubuntu\nVERSION_ID="22.04"\n' >"$r/etc/os-release"
    printf 'rockchip,rk3588-orangepi-5-ultra\0rockchip,rk3588\0' >"$r/proc/device-tree/compatible"
    echo "librknnrt version: 2.3.0" >"$r/usr/lib/librknnrt.so"
    echo "ssh-ed25519 AAAAC3Nza test@pc" >"$r/home/orangepi/.ssh/authorized_keys"
    printf 'git\ncmake\nv4l-utils\n' >"$r/state/installed"
    echo v4l-utils >"$r/state/held" # 镜像把 v4l-utils 锁定成了 hold（D12）
    echo graphical.target >"$r/state/default"
    printf 'video\nrender\ninput\n' >"$r/state/groups_exist"
    # Orange Pi 镜像的启动配置；extraargs 是第 4a 步手工改过、写错了的样子（R 在 @ 后面，被内核忽略）
    cat >"$r/boot/orangepiEnv.txt" <<'ENV'
verbosity=1
bootlogo=false
console=both
overlay_prefix=rk3588
fdtfile=rockchip/rk3588-orangepi-5-ultra.dtb
rootdev=UUID=0b1c2d3e-0000-4000-8000-000000000000
rootfstype=ext4
extraargs=cma=128M video=HDMI-A-1:1024x600@60Re
ENV
    echo "orangepi adm sudo" >"$r/state/user_groups"
    : >"$r/state/calls"
}

# 锁定版本的 librknnrt 来源
SRC="$WORK/rknnrt"
mkdir -p "$SRC/aarch64" "$SRC/include"
echo "librknnrt version: 2.3.2" >"$SRC/aarch64/librknnrt.so"
echo "/* rknn_api.h */" >"$SRC/include/rknn_api.h"
SHA="$WORK/rknnrt.sha256"
sha256sum "$SRC/aarch64/librknnrt.so" | awk '{print $1 "  librknnrt.so"}' >"$SHA"

OUT="$WORK/out.txt"
provision() { # provision 板子目录 [额外参数...]，退出码存在 RC，输出存在 OUT
    local r="$1"
    shift
    set +e
    RKSEC_ROOT="$r" FAKE_STATE="$r/state" PATH="$FAKE:$PATH" SUDO_USER=orangepi \
        bash "$SCRIPT" --rknnrt-dir "$SRC" --rknnrt-sha256 "$SHA" "$@" >"$OUT" 2>&1
    RC=$?
    set -e
}
snapshot() { # 板子目录下所有文件的内容摘要（不含日志目录和假命令的调用记录）
    (cd "$1" && find . -type f ! -path './home/orangepi/provision-logs/*' ! -path ./state/calls -print0 |
        sort -z | xargs -0 sha256sum)
}
CASE_FAILS=0
case_begin() {
    printf '\n== %s\n' "$1"
    CASE_FAILS=$FAILS
}
show_output_on_failure() { # 这个用例有失败时，把装机脚本的输出打出来，方便查
    if [ "$FAILS" -gt "$CASE_FAILS" ]; then
        echo "---- 装机脚本输出 ----"
        cat "$OUT"
    fi
}

# ---------- 用例 ----------
case_begin "第一次运行：把新板子装成标准环境"
B="$WORK/b1"
new_board "$B"
provision "$B"
expect "退出码 0" [ "$RC" -eq 0 ]
expect "装了缺的包（libstdc++-12-dev）" grep -qx libstdc++-12-dev "$B/state/installed"
expect "librknnrt 换成锁定版本" cmp -s "$SRC/aarch64/librknnrt.so" "$B/usr/lib/librknnrt.so"
expect "旧 librknnrt 有备份" grep -q "2.3.0" "$B"/usr/lib/librknnrt.so.bak-*
expect "头文件装好" cmp -s "$SRC/include/rknn_api.h" "$B/usr/include/rknn_api.h"
expect "加入 video、render、input 组" bash -c "grep -q ' video' '$B/state/user_groups' && grep -q ' render' '$B/state/user_groups' && grep -q ' input' '$B/state/user_groups'"
expect "屏幕时序参数改对了，cma 保留" grep -qx "extraargs=cma=128M video=HDMI-A-1:1024x600MR@60e" "$B/boot/orangepiEnv.txt"
expect "写错的 @60Re 去掉了" bash -c "! grep -q '@60Re' '$B/boot/orangepiEnv.txt'"
expect "启动配置的其他行不变" grep -qx "rootdev=UUID=0b1c2d3e-0000-4000-8000-000000000000" "$B/boot/orangepiEnv.txt"
expect "启动配置有备份（内容是原来的）" bash -c "grep -q '@60Re' '$B'/boot/orangepiEnv.txt.bak-*"
expect "没有留下临时文件" [ ! -e "$B/boot/orangepiEnv.txt.tmp" ]
expect "默认启动 multi-user" grep -qx multi-user.target "$B/state/default"
expect "SSH 配置装好" cmp -s "$REPO/tools/provision/10-hardening.conf" "$B/etc/ssh/sshd_config.d/10-hardening.conf"
expect "先 sshd -t 再 reload" bash -c "grep -n 'sshd -t\|reload ssh' '$B/state/calls' | head -1 | grep -q 'sshd -t'"
expect "命令历史带时间" [ -f "$B/etc/profile.d/rksec-history.sh" ]
expect "模型目录" [ -d "$B/home/orangepi/models" ]
expect "写了日志" bash -c "ls '$B'/home/orangepi/provision-logs/provision-*.log >/dev/null"
expect "hold 的包算已安装，不会去重装" bash -c "! grep -q 'install.* v4l-utils' '$B/state/calls'"
show_output_on_failure

case_begin "第二次运行：什么都不该改"
before=$(snapshot "$B")
provision "$B"
expect "退出码 0" [ "$RC" -eq 0 ]
expect "输出 修改 0 项" grep -q "汇总：修改 0 项，失败 0 项" "$OUT"
expect "文件一个都没变" [ "$before" = "$(snapshot "$B")" ]
show_output_on_failure

case_begin "apt-get update 报错（某个第三方源坏了）：照样安装"
B="$WORK/b9"
new_board "$B"
FAKE_APT_UPDATE_RC=100 provision "$B"
expect "退出码 0" [ "$RC" -eq 0 ]
expect "给出警告" grep -q "apt-get update 有错误" "$OUT"
expect "缺的包装上了" grep -qx libstdc++-12-dev "$B/state/installed"
show_output_on_failure

case_begin "apt-get install 失败：算失败"
B="$WORK/b10"
new_board "$B"
FAKE_APT_RC=100 provision "$B"
expect "退出码 1" [ "$RC" -eq 1 ]
expect "提示 apt 安装失败" grep -q "失败：apt 安装失败" "$OUT"
show_output_on_failure

case_begin "--dry-run：只报告，不修改"
B="$WORK/b2"
new_board "$B"
before=$(snapshot "$B")
provision "$B" --dry-run
expect "退出码 0" [ "$RC" -eq 0 ]
expect "报告了将修改的项" grep -q "将修改：" "$OUT"
expect "文件一个都没变" [ "$before" = "$(snapshot "$B")" ]
expect "没有调用 apt-get / systemctl set-default" bash -c "! grep -q 'apt-get\|set-default' '$B/state/calls'"
show_output_on_failure

case_begin "没有登录公钥：不能关密码登录"
B="$WORK/b3"
new_board "$B"
rm "$B/home/orangepi/.ssh/authorized_keys"
provision "$B"
expect "退出码 1" [ "$RC" -eq 1 ]
expect "SSH 配置没有装" [ ! -e "$B/etc/ssh/sshd_config.d/10-hardening.conf" ]
expect "提示原因" grep -q "还没有登录公钥" "$OUT"
expect "其余步骤照常完成" grep -qx multi-user.target "$B/state/default"
show_output_on_failure

case_begin "sshd -t 校验失败：恢复原配置"
B="$WORK/b4"
new_board "$B"
echo "PasswordAuthentication no  # 旧文件" >"$B/etc/ssh/sshd_config.d/10-hardening.conf"
old=$(sha256sum <"$B/etc/ssh/sshd_config.d/10-hardening.conf")
FAKE_SSHD_RC=255 provision "$B"
expect "退出码 1" [ "$RC" -eq 1 ]
expect "原配置原样恢复" [ "$old" = "$(sha256sum <"$B/etc/ssh/sshd_config.d/10-hardening.conf")" ]
expect "没有 reload sshd" bash -c "! grep -q 'reload ssh' '$B/state/calls'"
show_output_on_failure

case_begin "librknnrt 来源被换过：拒绝安装"
B="$WORK/b5"
new_board "$B"
cp -r "$SRC" "$WORK/rknnrt-bad"
echo "tampered" >>"$WORK/rknnrt-bad/aarch64/librknnrt.so"
provision "$B" --rknnrt-dir "$WORK/rknnrt-bad"
expect "退出码 1" [ "$RC" -eq 1 ]
expect "原库没被替换" grep -q "2.3.0" "$B/usr/lib/librknnrt.so"
expect "提示 sha256 不一致" grep -q "sha256 和锁定值不一致" "$OUT"
show_output_on_failure

case_begin "锁定值还没填"
B="$WORK/b6"
new_board "$B"
printf '待填  librknnrt.so\n' >"$WORK/unset.sha256"
provision "$B" --rknnrt-sha256 "$WORK/unset.sha256"
expect "退出码 1" [ "$RC" -eq 1 ]
expect "提示要先填" grep -q "还没有填锁定的 sha256" "$OUT"
show_output_on_failure

case_begin "不是目标平台：什么都不做"
B="$WORK/b7"
new_board "$B"
printf 'ID=ubuntu\nVERSION_ID="24.04"\n' >"$B/etc/os-release"
before=$(snapshot "$B")
provision "$B"
expect "退出码 2" [ "$RC" -eq 2 ]
expect "文件一个都没变" [ "$before" = "$(snapshot "$B")" ]
show_output_on_failure

case_begin "启动配置里没有 extraargs 行：加一行"
B="$WORK/b11"
new_board "$B"
sed -i '/^extraargs=/d' "$B/boot/orangepiEnv.txt"
provision "$B"
expect "退出码 0" [ "$RC" -eq 0 ]
expect "加了 extraargs" grep -qx "extraargs=video=HDMI-A-1:1024x600MR@60e" "$B/boot/orangepiEnv.txt"
expect "原有的行还在" grep -qx "rootfstype=ext4" "$B/boot/orangepiEnv.txt"
show_output_on_failure

case_begin "启动配置里有两行 extraargs：不猜，报错，不改"
B="$WORK/b12"
new_board "$B"
echo "extraargs=quiet" >>"$B/boot/orangepiEnv.txt"
old=$(sha256sum <"$B/boot/orangepiEnv.txt")
provision "$B"
expect "退出码 1" [ "$RC" -eq 1 ]
expect "提示手工合并" grep -q "多行 extraargs" "$OUT"
expect "文件没动" [ "$old" = "$(sha256sum <"$B/boot/orangepiEnv.txt")" ]
show_output_on_failure

case_begin "--video none：不动启动配置"
B="$WORK/b13"
new_board "$B"
old=$(sha256sum <"$B/boot/orangepiEnv.txt")
provision "$B" --video none
expect "退出码 0" [ "$RC" -eq 0 ]
expect "文件没动" [ "$old" = "$(sha256sum <"$B/boot/orangepiEnv.txt")" ]
show_output_on_failure

case_begin "换一块屏：--video 指定别的时序，只替换同一个输出口的参数"
B="$WORK/b14"
new_board "$B"
sed -i 's/^extraargs=.*/extraargs=cma=128M video=DSI-1:800x1280@60 video=HDMI-A-1:1024x600@60e/' "$B/boot/orangepiEnv.txt"
provision "$B" --video "video=HDMI-A-1:1280x800MR@60e"
expect "退出码 0" [ "$RC" -eq 0 ]
expect "HDMI 的换了，DSI 的保留" grep -qx "extraargs=cma=128M video=DSI-1:800x1280@60 video=HDMI-A-1:1280x800MR@60e" "$B/boot/orangepiEnv.txt"
show_output_on_failure

case_begin "没有 /boot/orangepiEnv.txt：这一项失败，其余照做"
B="$WORK/b15"
new_board "$B"
rm "$B/boot/orangepiEnv.txt"
provision "$B"
expect "退出码 1" [ "$RC" -eq 1 ]
expect "提示找不到" grep -q "找不到 .*orangepiEnv.txt" "$OUT"
expect "其余步骤照常完成" grep -qx multi-user.target "$B/state/default"
show_output_on_failure

case_begin "用法错误"
B="$WORK/b8"
new_board "$B"
provision "$B" --bogus
expect "未知选项退出码 2" [ "$RC" -eq 2 ]
provision "$B" --video "1024x600@60"
expect "--video 格式不对退出码 2" [ "$RC" -eq 2 ]

echo "----"
echo "通过 $PASS 项，失败 $FAILS 项"
[ "$FAILS" -eq 0 ]
