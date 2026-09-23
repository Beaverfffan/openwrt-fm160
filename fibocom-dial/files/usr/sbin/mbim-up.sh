#!/bin/sh
# FM160 原厂 MBIM 拨号（复刻 Fibocom ECM&NCM&RNDIS&MBIM 拨号集成指导 V1.8 §5.5）
# 前置：模块已切 MBIM 模式（GTUSBMODE 30），cdc_mbim 驱动已枚举 wwan0 + /dev/cdc-wdm0
# 用法：mbim-up.sh [APN] [ip-type: ipv4v6|ipv4|ipv6] [configure|report]   默认 ctnet、ipv4v6、configure
set -e
APN=${1:-ctnet}
IPTYPE=${2:-ipv4v6}
DEV=/dev/cdc-wdm0
IF=wwan0
LOG=/var/log/vendor-mbim.log
log() { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }

# 0. 清场：QMAP 残留的 fibo_qmimsg_server 独占 cdc-wdm0 并向 MBIM 通道灌 QMI 报文，
#    会把模组侧 MBIM 端点打挂（2026-09-23 实测，需 USB 模式弹跳才能恢复），必须先杀干净
killall fibo_qmimsg_server 2>/dev/null && log "杀掉残留的 fibo_qmimsg_server"
pkill -f fibocom-dial 2>/dev/null || true
killall mbim-proxy 2>/dev/null || true
# 上次 setup 被杀可能留下挂死的 mbimcli（占着 wdm 端口让新连接永久等待）
killall mbimcli 2>/dev/null && log "杀掉挂死的 mbimcli"
sleep 2

# busybox 没有 timeout：mbimcli 对挂死端点会无限等，用后台+看门狗代替（25s 超时）
mbim() {
	"$@" &
	local pid=$! i=0
	while kill -0 $pid 2>/dev/null && [ $i -lt 25 ]; do sleep 1; i=$((i+1)); done
	if kill -0 $pid 2>/dev/null; then
		kill -9 $pid 2>/dev/null
		log "mbimcli 看门狗超时（25s）: $*"
		return 124
	fi
	wait $pid
	return $?
}

[ -c "$DEV" ] || { log "$DEV 不存在"; exit 1; }
ip link set "$IF" up 2>/dev/null || true

# 1. 初始化查询（等同文档步骤 1）
log "查询订阅者就绪状态"
mbim mbimcli -p -d "$DEV" --query-subscriber-ready-status >>"$LOG" 2>&1 || {
	log "subscriber-ready 失败，模块未就绪"; exit 1; }

# 2. 拨号（等同文档步骤 2，ip-type 决定 v4/v6/双栈）
log "发起 MBIM 连接 apn=$APN ip-type=$IPTYPE"
mbim mbimcli -p -d "$DEV" --connect="session-id=0,apn=$APN,ip-type=$IPTYPE" >>"$LOG" 2>&1 || {
	log "connect 失败"; exit 1; }

# 3. 把拿到的 IP 配到网卡（复刻 mbim-set-ip；第 3 参 report 时只写 env 不碰网卡，供 netifd proto fm160 用）
/usr/sbin/mbim-set-ip.sh "$DEV" "$IF" session-id=0 "${3:-configure}" >>"$LOG" 2>&1 || exit 1

log "MBIM 拨号完成：ifconfig $IF 应已拿到地址"
