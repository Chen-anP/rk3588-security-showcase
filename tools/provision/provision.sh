#!/usr/bin/env bash
# provision.sh —— 装机：把一块刷好官方 Ubuntu 22.04 镜像的 Orange Pi 5 Ultra 变成本项目的标准环境。
# 第 1 步手工做过的事都在这里，一条命令做完，结束时跑 board_check.sh。
#
# 用法（板子上，在仓库根目录）：
#   sudo tools/provision/provision.sh --dry-run     # 只看会改什么，不改
#   sudo tools/provision/provision.sh               # 真正执行
# 选项：
#   --user 用户名          装给谁（默认是 sudo 之前的用户）
#   --rknnrt-dir 目录      librknnrt 来源，含 aarch64/librknnrt.so 和 include/
#                          （默认 ~用户/rksdk/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api）
#   --rknnrt-sha256 文件   librknnrt.so 的锁定校验值（默认本目录的 rknnrt.sha256）
#   --skip-ssh             不动 SSH 配置
#   --video 参数           屏幕时序启动参数（默认 video=HDMI-A-1:1024x600MR@60e，见 D14）；none 表示不管
#
# 原则：
# - 可以重复运行：每一项先检查，已经是目标状态就跳过；第二次运行应该显示“修改 0 项”
# - 先检查后修改，改坏能退回：SSH 配置先 sshd -t 校验，不通过就恢复原文件，不会把自己锁在外面
# - 每次运行的完整输出存到 ~用户/provision-logs/
# 退出码：0 全部成功；1 有步骤失败；2 用法错误或不是目标平台
# 注意：change 调用的函数在 “|| fail” 里执行，bash 的 set -e 在那里不起作用，所以函数里每条命令都要自己 || return 1
#
# 测试：tests/provision/test_provision.sh 用 RKSEC_ROOT（所有系统路径的前缀）和假命令在普通电脑上跑。
#
# install_pkgs、install_rt 这些函数是通过 change 间接调用的，shellcheck 0.9 以上会误报“执行不到”（SC2317）
# shellcheck disable=SC2317
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
ROOT=${RKSEC_ROOT:-} # 测试用；真机上为空

DRY=0
SKIP_SSH=0
TARGET_USER=${SUDO_USER:-}
RKNNRT_DIR=""
RKNNRT_SHA_FILE="$HERE/rknnrt.sha256"

# 1024x600 HDMI 小屏读不到 EDID，要在启动参数里指定时序。
# M = 按 CVT 标准计算，R = 减少消隐：像素时钟 43.833 MHz，HDMI PLL 能精确给出，不闪（D14）。
# 注意 M、R 要写在 @刷新率 前面，写成 @60Re 时 R 会被忽略
VIDEO_ARG="video=HDMI-A-1:1024x600MR@60e"

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY=1 ;;
        --skip-ssh) SKIP_SSH=1 ;;
        --user) TARGET_USER=${2:?--user 后面要跟用户名}; shift ;;
        --rknnrt-dir) RKNNRT_DIR=${2:?--rknnrt-dir 后面要跟目录}; shift ;;
        --rknnrt-sha256) RKNNRT_SHA_FILE=${2:?--rknnrt-sha256 后面要跟文件}; shift ;;
        --video) VIDEO_ARG=${2:?--video 后面要跟参数，比如 video=HDMI-A-1:1024x600MR@60e，或 none}; shift ;;
        -h | --help) usage; exit 0 ;;
        *) echo "未知选项: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [ "$VIDEO_ARG" != none ] && ! [[ "$VIDEO_ARG" =~ ^video=[A-Za-z0-9-]+:[^[:space:]]+$ ]]; then
    echo "--video 格式不对：$VIDEO_ARG（应为 video=输出口:模式，比如 video=HDMI-A-1:1024x600MR@60e）" >&2
    exit 2
fi
if [ -z "$ROOT" ] && [ "$(id -u)" -ne 0 ]; then
    echo "请用 sudo 运行" >&2
    exit 2
fi
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = root ]; then
    echo "不知道装给哪个用户：用 sudo 从普通用户运行，或加 --user 用户名" >&2
    exit 2
fi
HOME_DIR="$ROOT/home/$TARGET_USER"
[ -n "$RKNNRT_DIR" ] || RKNNRT_DIR="$HOME_DIR/rksdk/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api"

# ---------- 输出与计数 ----------
CHANGED=0
FAILED=0
step() { printf '\n== %s\n' "$1"; }
ok() { printf '  已是目标状态：%s\n' "$1"; }
fail() {
    printf '  失败：%s\n' "$1"
    FAILED=$((FAILED + 1))
}
# change "说明" 命令...：真正执行时运行命令；--dry-run 时只打印
change() {
    local what="$1"
    shift
    CHANGED=$((CHANGED + 1))
    if [ "$DRY" -eq 1 ]; then
        printf '  将修改：%s\n' "$what"
        return 0
    fi
    printf '  修改：%s\n' "$what"
    "$@"
}

run_all() {
echo "provision.sh  $(date '+%F %T')  用户=$TARGET_USER  $([ "$DRY" -eq 1 ] && echo '[dry-run 只检查不修改]')"
echo "仓库版本：$(git -C "$REPO" describe --tags --dirty --always 2>/dev/null || echo 未知)"

# ---------- 1. 平台 ----------
step "1. 平台：Ubuntu 22.04 + RK3588"
# shellcheck source=/dev/null
os_id=$(. "$ROOT/etc/os-release" && echo "${ID:-}-${VERSION_ID:-}")
compat=$(tr '\0' ' ' <"$ROOT/proc/device-tree/compatible" 2>/dev/null || true)
if [ "$os_id" != "ubuntu-22.04" ] || [[ "$compat" != *rk3588* ]]; then
    echo "  不是目标平台：系统 $os_id，设备树 ${compat:-读不到}（需要 ubuntu-22.04 + rk3588）"
    exit 2
fi
ok "$os_id，$compat"

# ---------- 2. 软件包 ----------
step "2. 软件包（$(basename "$HERE")/packages.txt）"
mapfile -t PKGS < <(sed 's/#.*//; s/[[:space:]]//g; /^$/d' "$HERE/packages.txt")
missing=()
for p in "${PKGS[@]}"; do
    # 状态是“期望 标志 状态”三段。只看最后一段是不是 installed：
    # Orange Pi 镜像把 Rockchip 定制的包（v4l-utils、mpp、rga、gstreamer……）设成了 hold，状态是 "hold ok installed"（D12）
    if ! dpkg-query -W -f='${Status}\n' "$p" 2>/dev/null | grep -q ' installed$'; then
        missing+=("$p")
    fi
done
if [ "${#missing[@]}" -eq 0 ]; then
    ok "${#PKGS[@]} 个包都已安装"
else
    install_pkgs() {
        # 某个第三方源坏了（比如缺签名公钥），update 会返回错误，但其他源的列表已经更新了。
        # 不因此放弃安装：给出警告，安装成不成功由 apt-get install 自己的结果决定（D12）
        if ! apt-get update; then
            echo "  警告：apt-get update 有错误（见上面的 E: 行），用现有的软件包列表继续安装"
        fi
        DEBIAN_FRONTEND=noninteractive apt-get install -y "${missing[@]}"
    }
    change "安装 ${missing[*]}" install_pkgs || fail "apt 安装失败"
fi
held=$(apt-mark showhold | wc -l)
echo "  镜像锁定（hold）的包 $held 个：Rockchip 定制版本，不要 unhold 或升级（D12）"
echo "  已装版本："
for p in "${PKGS[@]}"; do
    printf '    %-20s %s\n' "$p" "$(dpkg-query -W -f='${Version}' "$p" 2>/dev/null || echo 未安装)"
done

# ---------- 3. NPU 运行时 librknnrt ----------
step "3. NPU 运行时 librknnrt（ADR-03）"
want=$(sed 's/#.*//' "$RKNNRT_SHA_FILE" | awk 'NF {print $1; exit}')
LIB="$ROOT/usr/lib/librknnrt.so"
SRC="$RKNNRT_DIR/aarch64/librknnrt.so"
if ! [[ "$want" =~ ^[0-9a-f]{64}$ ]]; then
    fail "$RKNNRT_SHA_FILE 里还没有填锁定的 sha256（见文件里的说明）"
else
    have=$(sha256sum "$LIB" 2>/dev/null | awk '{print $1}' || true)
    if [ "$have" = "$want" ]; then
        ok "librknnrt.so sha256 = ${want:0:12}…"
    elif [ ! -f "$SRC" ]; then
        fail "需要替换，但找不到来源 $SRC（用 --rknnrt-dir 指定）"
    elif [ "$(sha256sum "$SRC" | awk '{print $1}')" != "$want" ]; then
        fail "来源 $SRC 的 sha256 和锁定值不一致，拒绝安装"
    else
        install_rt() {
            if [ -f "$LIB" ] && [ ! -e "$LIB.bak-${have:0:12}" ]; then
                cp -p "$LIB" "$LIB.bak-${have:0:12}" || return 1 # 旧版本留底，按 sha 命名，不会被覆盖
            fi
            install -m 644 "$SRC" "$LIB" || return 1
            ldconfig
        }
        change "librknnrt.so ${have:0:12}… → ${want:0:12}…（旧文件备份为 .bak-${have:0:12}）" install_rt ||
            fail "安装 librknnrt 失败"
    fi
    # 头文件：和库同一个来源
    if [ -d "$RKNNRT_DIR/include" ]; then
        for h in "$RKNNRT_DIR"/include/*.h; do
            dst="$ROOT/usr/include/$(basename "$h")"
            if ! cmp -s "$h" "$dst"; then
                change "头文件 $(basename "$h")" install -D -m 644 "$h" "$dst" || fail "安装 $(basename "$h") 失败"
            fi
        done
    fi
fi

# ---------- 4. 用户组 ----------
step "4. 用户组：$TARGET_USER 能访问摄像头、RGA、DRM、NPU、触摸屏"
for g in video render input; do
    if ! getent group "$g" >/dev/null; then
        echo "  系统里没有 $g 组，跳过"
    elif id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx "$g"; then
        ok "在 $g 组"
    else
        change "把 $TARGET_USER 加入 $g 组（重新登录后生效）" usermod -aG "$g" "$TARGET_USER" || fail "usermod 失败"
    fi
done

# ---------- 5. 启动方式 ----------
step "5. 开机进文本模式（ADR-01）"
if [ "$(systemctl get-default)" = multi-user.target ]; then
    ok "multi-user.target"
else
    change "systemctl set-default multi-user.target（下次开机生效）" systemctl set-default multi-user.target ||
        fail "设置启动目标失败"
fi

# ---------- 6. SSH 只允许密钥登录 ----------
step "6. SSH：只允许密钥登录，禁止 root 远程登录（ADR-06）"
CONF="$ROOT/etc/ssh/sshd_config.d/10-hardening.conf"
if [ "$SKIP_SSH" -eq 1 ]; then
    echo "  --skip-ssh，跳过"
elif cmp -s "$HERE/10-hardening.conf" "$CONF"; then
    ok "$CONF 与仓库一致"
elif ! grep -qE '(^|[[:space:]])(ssh-|ecdsa-|sk-)' "$HOME_DIR/.ssh/authorized_keys" 2>/dev/null; then
    # 没有任何公钥就关掉密码登录 = 把自己锁在门外
    fail "$TARGET_USER 还没有登录公钥（~/.ssh/authorized_keys），先配好密钥再跑，否则关掉密码后就进不来了"
else
    apply_ssh() {
        local backup=""
        if [ -f "$CONF" ]; then
            backup=$(mktemp) && cp -p "$CONF" "$backup" || return 1
        fi
        install -D -m 644 "$HERE/10-hardening.conf" "$CONF" || return 1
        if ! sshd -t; then
            echo "  sshd -t 校验不通过，恢复原配置"
            if [ -n "$backup" ]; then cp -p "$backup" "$CONF"; else rm -f "$CONF"; fi
            [ -z "$backup" ] || rm -f "$backup"
            return 1
        fi
        [ -z "$backup" ] || rm -f "$backup"
        systemctl reload ssh # reload 不断开已有连接
    }
    change "安装 $CONF 并重新加载 sshd" apply_ssh || fail "SSH 配置没有生效（原配置已恢复）"
fi

# ---------- 7. 命令历史带时间 ----------
step "7. 命令历史带时间"
HIST="$ROOT/etc/profile.d/rksec-history.sh"
if cmp -s "$HERE/history.sh" "$HIST"; then
    ok "$HIST"
else
    change "安装 $HIST（重新登录后生效）" install -D -m 644 "$HERE/history.sh" "$HIST" || fail "安装失败"
fi

# ---------- 8. 模型目录 ----------
step "8. 模型目录"
if [ -d "$HOME_DIR/models" ]; then
    ok "$HOME_DIR/models"
else
    make_models_dir() {
        mkdir -p "$HOME_DIR/models" || return 1
        if [ -z "$ROOT" ]; then chown "$TARGET_USER:" "$HOME_DIR/models"; fi
    }
    change "创建 $HOME_DIR/models（模型用 tools/model-convert/deploy_model.sh 从电脑拷过来）" make_models_dir ||
        fail "创建失败"
fi

# ---------- 9. 屏幕时序 ----------
step "9. 屏幕时序：启动参数 video=（D14，重启后生效）"
ENV_TXT="$ROOT/boot/orangepiEnv.txt"
if [ "$VIDEO_ARG" = none ]; then
    echo "  --video none，跳过"
elif [ ! -f "$ENV_TXT" ]; then
    fail "找不到 $ENV_TXT（不是 Orange Pi 官方镜像？）"
elif [ "$(grep -c '^extraargs=' "$ENV_TXT" || true)" -gt 1 ]; then
    fail "$ENV_TXT 里有多行 extraargs=，不知道改哪一行，请手工合并成一行"
else
    conn=${VIDEO_ARG#video=}
    conn=${conn%%:*}
    cur=$(sed -n 's/^extraargs=//p' "$ENV_TXT")
    # 去掉同一个输出口原来的 video= 参数（比如写错的 @60Re），其他参数（cma=128M 等）原样保留
    read -r -a old_args <<<"$cur"
    new_args=()
    for a in "${old_args[@]}"; do
        [[ "$a" == "video=$conn:"* ]] || new_args+=("$a")
    done
    new_args+=("$VIDEO_ARG")
    new="${new_args[*]}"
    if [ "$cur" = "$new" ]; then
        ok "extraargs=$cur"
    else
        set_video() {
            local backup tmp
            backup="$ENV_TXT.bak-$(date +%Y%m%d-%H%M%S)"
            cp -p "$ENV_TXT" "$backup" || return 1
            tmp="$ENV_TXT.tmp"
            if grep -q '^extraargs=' "$ENV_TXT"; then
                awk -v line="extraargs=$new" '/^extraargs=/ {print line; next} {print}' "$ENV_TXT" >"$tmp" || return 1
            else
                { cat "$ENV_TXT"; echo "extraargs=$new"; } >"$tmp" || return 1
            fi
            # 写坏启动配置可能开不了机：先在临时文件里核对，再整体替换（mv 是原子的，不会出现写了一半的文件）
            if ! grep -qxF "extraargs=$new" "$tmp" || [ "$(grep -c '' "$tmp")" -lt "$(grep -c '' "$ENV_TXT")" ]; then
                rm -f "$tmp"
                echo "  生成的新文件核对不通过，没有修改"
                return 1
            fi
            mv "$tmp" "$ENV_TXT" || return 1
            sync
            echo "  原文件备份为 ${backup#"$ROOT"}；改坏了用它恢复：sudo cp ${backup#"$ROOT"} /boot/orangepiEnv.txt"
        }
        change "extraargs：'${cur}' → '${new}'（重启后生效）" set_video || fail "修改 $ENV_TXT 失败"
    fi
fi

# ---------- 10. 自检 ----------
step "10. 板级自检 board_check.sh"
if [ -n "$ROOT" ] || [ "$DRY" -eq 1 ]; then
    echo "  dry-run / 测试模式，跳过"
elif ! bash "$REPO/tools/board_check.sh"; then
    fail "board_check.sh 有失败项"
fi

# ---------- 汇总 ----------
echo
if [ "$DRY" -eq 1 ]; then
    echo "汇总（dry-run）：将修改 $CHANGED 项，失败 $FAILED 项"
else
    echo "汇总：修改 $CHANGED 项，失败 $FAILED 项"
fi
[ "$FAILED" -eq 0 ]
}

# 日志：屏幕上照常显示，同时存一份（dry-run 也存，方便对比）
if [ ! -d "$HOME_DIR" ]; then
    echo "找不到 $HOME_DIR" >&2
    exit 2
fi
LOG_DIR="$HOME_DIR/provision-logs"
LOG="$LOG_DIR/provision-$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$LOG_DIR"
set +e
run_all 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
set -e
if [ -z "$ROOT" ]; then
    chown -R "$TARGET_USER:" "$LOG_DIR"
fi
echo "日志：${LOG#"$ROOT"}"
exit "$rc"
