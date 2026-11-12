# shellcheck shell=sh
# 由 tools/provision/provision.sh 安装到 /etc/profile.d/
# 命令历史带日期时间：出了问题能对上“几点做了什么”
export HISTTIMEFORMAT='%F %T '
