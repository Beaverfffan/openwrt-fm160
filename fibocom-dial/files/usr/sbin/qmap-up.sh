#!/bin/sh
# FM160 原厂 QMAP 拨号（复刻 Fibocom QMI&Gobinet 拨号指南 V1.7 §6.5.2）
# 前置：模块已在 QMI 模式（GTUSBMODE 32），qmi_wwan_f 驱动已枚举 wwan0 + /dev/cdc-wdm0
# 流程：设 qmap_mode → 起 fibo_qmimsg_server → fibocom-dial 逐路拨号（QMI 直配 IP，无 DHCP）
# 用法：qmap-up.sh [通道数] [APN] [栈: 46|4|6]     默认 2 路、APN ctnet、双栈
set -e

QMAP_N=${1:-2}
APN=${2:-ctnet}
STACK=${3:-46}
FLAGS=""
case "$STACK" in
	*4*) FLAGS="$FLAGS -4" ;;
esac
case "$STACK" in
	*6*) FLAGS="$FLAGS -6" ;;
esac
[ -n "$FLAGS" ] || FLAGS=" -4"
LOG=/var/log/vendor-qmap.log
log() { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }

# 0. 清场：MBIM 残留的 mbim-proxy 独占 cdc-wdm0，QMAP 的 QMI 代理起不来
killall mbim-proxy 2>/dev/null && log "杀掉残留的 mbim-proxy"
sleep 1

[ -d /sys/class/net/wwan0 ] || { log "wwan0 不存在，模块未在 QMI 模式"; exit 1; }
[ -c /dev/cdc-wdm0 ] || { log "/dev/cdc-wdm0 不存在"; exit 1; }

# 1. 设置 qmap_mode（只设一次，重设会重建所有虚拟网卡）
cur=$(cat /sys/class/net/wwan0/qmap_mode 2>/dev/null || echo 0)
if [ "$cur" != "$QMAP_N" ]; then
	log "设置 qmap_mode=$QMAP_N（当前 $cur）"
	echo "$QMAP_N" > /sys/class/net/wwan0/qmap_mode || { log "qmap_mode 设置失败"; exit 1; }
	sleep 3
fi

# 2. 启动 QMI 消息代理（多 PDN 必配，单路也统一走它）
if ! pidof fibo_qmimsg_server >/dev/null 2>&1; then
	log "启动 fibo_qmimsg_server"
	/usr/bin/fibo_qmimsg_server >>"$LOG" 2>&1 &
	sleep 2
fi

# 3. 关反向路由（原厂 FAQ 5.7.1 / 7.x：多路网卡 ping 不通的解法）
echo 0 > /proc/sys/net/ipv4/conf/all/rp_filter 2>/dev/null || true
i=1
while [ "$i" -le "$QMAP_N" ]; do
	f="/proc/sys/net/ipv4/conf/wwan0.$i/rp_filter"
	[ -e "$f" ] && echo 0 > "$f" 2>/dev/null
	i=$((i + 1))
done

# 4. 逐路拨号：第 1 路设置 APN，其余路用模块预置 PDP
i=1
if ! pidof fibocom-dial >/dev/null 2>&1; then
	log "第 1 路拨号：fibocom-dial -n 1 -m 1$FLAGS -s $APN"
	/usr/bin/fibocom-dial -n 1 -m 1 $FLAGS -s "$APN" >>"$LOG" 2>&1 &
	sleep 8
fi
i=2
while [ "$i" -le "$QMAP_N" ]; do
	if ! pgrep -f "fibocom-dial -n $i " >/dev/null 2>&1; then
		log "第 $i 路拨号：fibocom-dial -n $i -m $i -4"
		/usr/bin/fibocom-dial -n "$i" -m "$i" -4 >>"$LOG" 2>&1 &
		sleep 5
	fi
	i=$((i + 1))
done

log "拨号指令已全部下发，观察 /var/log/vendor-qmap.log 与 ifconfig wwan0.x"
