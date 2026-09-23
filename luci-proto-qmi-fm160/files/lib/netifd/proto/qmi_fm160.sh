#!/bin/sh
# netifd proto handler: FM160 QMAP 拨号（广和通 vendor CLI 实现，非 uqmi）
#
# QMAP = QMI 控制通道 + wwan0.x 多路数据通道，拨号全部走广和通原厂
# CLI（/usr/sbin/qmap-up.sh，内部 QMICLIENT/QMAPFWD 命令），不使用 OpenWrt
# 原生 uqmi/libqmi 栈。拨号参数（APN/协议栈/通道数）读 /etc/config/fm160，
# 由 FM160 拨号页管理。
#
# setup:   qmap-up.sh 后台拨号 → 等数据设备出现、等厂商日志写出本拨号的
#          IP 配置（以日志为准：fibocom-dial 守护进程会反复增删网卡上的
#          地址，直接读网卡不可靠）→ 自己用日志值落地址 → 洗掉厂商残留
#          由 netifd 接管（避免 EEXIST）
# teardown: qmap-down.sh 拆通道 + 收回 LAN 侧运营商前缀

. /lib/functions.sh
. /lib/netifd/netifd-proto.sh
init_proto "$@"

fm160_log() { logger -t "proto-qmi-fm160" "$*"; }

fm160_apn()   { uci -q get fm160.main.dial_apn || echo ctnet; }
fm160_v4()    { uci -q get fm160.main.dial_v4; }
fm160_v6()    { uci -q get fm160.main.dial_v6; }
fm160_chans() { uci -q get fm160.main.qmap_channels || echo 1; }

# 取本拨号日志段里某字段的最后一个值（qmap-up 前须先设 LOGMARK）
QLOG=/var/log/vendor-qmap.log
qlog() { tail -n +$((LOGMARK + 1)) "$QLOG" 2>/dev/null | grep -m1 "$1" | awk '{print $NF}' | tr -d '\r'; }

# 等到 wwan0 / wwan0.x 上某个设备出现全局地址；stdout 打出设备名
fm160_wait_dev() {
	local want4 want6 i d
	[ "$(fm160_v4)" != 0 ] && want4=1
	[ "$(fm160_v6)" != 0 ] && want6=1
	i=0
	while [ $i -lt 45 ]; do
		for d in wwan0 wwan0.1 wwan0.2 wwan0.3 wwan0.4; do
			[ -d "/sys/class/net/$d" ] || continue
			if { [ -n "$want4" ] && ip -4 -br a show dev "$d" 2>/dev/null | grep -qE ' (10\.|100\.|172\.|192\.|[0-9])'; } || \
			   { [ -n "$want6" ] && ip -6 -br a show dev "$d" 2>/dev/null | grep -q ' 2[0-9a-f][0-9a-f]:'; }; then
				echo "$d"
				return 0
			fi
		done
		sleep 2; i=$((i + 1))
	done
	return 1
}

proto_qmi_fm160_init_config() {
	no_device=1
	available=1
	proto_config_add_string "device:device"
}

proto_qmi_fm160_setup() {
	local config="$1" iface="$2"
	local ST dev v4a i

	ST=46; [ "$(fm160_v4)" = 0 ] && ST=6; [ "$(fm160_v6)" = 0 ] && ST=4
	LOGMARK=$(wc -l < "$QLOG" 2>/dev/null || echo 0)

	qmap-up.sh "$(fm160_chans)" "$(fm160_apn)" "$ST" >/dev/null 2>&1 &
	dev=$(fm160_wait_dev) || {
		proto_notify_error "$config" "NO_DATA_DEV"
		proto_block_restart "$config"
		return
	}
	# 必须先拉起设备：fibocom-dial 守护进程只在接口 UP 后才往网卡落配置
	ip link set dev "$dev" up

	# 等厂商日志写出本拨号的 v6 配置（最长 45s；守护进程地址增删频繁，
	# 后续一律以日志值为准，不信网卡读取）
	if [ "$(fm160_v6)" != 0 ]; then
		i=0
		while [ $i -lt 22 ]; do
			[ -n "$(qlog 'IPv6 Address')" ] && break
			sleep 2; i=$((i + 1))
		done
	fi

	# 从本拨号日志段读配置（v4 地址以网卡为准——wait_dev 已保证其存在）
	local IP4ADDR="" IP4MASK="" IP4GW="" IP4DNS="" IP4MTU=""
	local IP6ADDR="" IP6MASK="" IP6GW="" IP6DNS=""
	v4a=$(ip -4 -br a show dev "$dev" 2>/dev/null | awk '{print $3}' | head -1)
	IP4ADDR=${v4a%/*}; IP4MASK=${v4a#*/}
	IP4GW=$(qlog 'IPv4 Gateway')
	IP4DNS=$(tail -n +$((LOGMARK + 1)) "$QLOG" 2>/dev/null | grep -E "IPv4 DNS[0-9]" | tail -2 | cut -d"'" -f2 | tr '\n' ' ')
	IP6ADDR=$(qlog 'IPv6 Address')
	IP6MASK=$(qlog 'IPv6 PrefixLengthIPAddr')
	IP6GW=$(qlog 'IPv6 Gateway')
	IP6DNS=$(tail -n +$((LOGMARK + 1)) "$QLOG" 2>/dev/null | grep -E "IPv6 DNS[0-9]" | tail -2 | cut -d"'" -f2 | tr '\n' ' ')

	# 用日志值把地址落回网卡（replace + nodad：幂等且跳过 DAD 窗口），
	# 让 netifd 随后的安装/读取都稳定
	[ -n "$IP6ADDR" ] && ip -6 addr replace "$IP6ADDR/${IP6MASK:-64}" dev "$dev" nodad 2>/dev/null

	ip link set dev "$dev" up

	# 洗掉厂商脚本直接配置的部分，由 netifd 接管（避免 EEXIST）
	ip route del default dev "$dev" 2>/dev/null
	ip -6 route del default dev "$dev" 2>/dev/null
	ip addr flush dev "$dev" label "$dev" 2>/dev/null || ip -4 addr flush dev "$dev"
	ip -6 addr flush dev "$dev" scope global 2>/dev/null

	proto_init_update "$dev" 1
	proto_set_keep 1

	if [ -n "$IP4ADDR" ] && [ "$IP4MASK" != "$IP4ADDR" ]; then
		proto_add_ipv4_address "$IP4ADDR" "$IP4MASK" "" "$IP4GW"
		if [ -n "$IP4GW" ]; then
			proto_add_ipv4_route "0.0.0.0" 0 "$IP4GW"
		else
			proto_add_ipv4_route "0.0.0.0" 0 ""
		fi
	fi
	for d in ${IP4DNS:-}; do
		case "$d" in *.*) proto_add_dns_server "$d" ;; esac
	done

	if [ -n "$IP6ADDR" ] && [ "$IP6MASK" != "$IP6ADDR" ]; then
		proto_add_ipv6_address "$IP6ADDR" "${IP6MASK:-64}"
		if [ -n "$IP6GW" ]; then
			proto_add_ipv6_route "::" 0 "$IP6GW"
		else
			proto_add_ipv6_route "::" 0 ""
		fi
	fi
	for d in ${IP6DNS:-}; do
		case "$d" in *:*) proto_add_dns_server "$d" ;; esac
	done

	proto_send_update "$config"
	fm160_log "$config: qmap link up on $dev v4=${IP4ADDR:-none} v6=${IP6ADDR:-none}"
	/usr/sbin/fm160-oplog add netifd 链路建立 "QMAP 拨号成功 v4=${IP4ADDR:-无} v6=${IP6ADDR:-无}" 已连接
}

proto_qmi_fm160_teardown() {
	local config="$1" iface="$2"
	qmap-down.sh >/dev/null 2>&1
	/etc/init.d/fm160-prefix withdraw >/dev/null 2>&1
	fm160_log "$config: teardown done"
	/usr/sbin/fm160-oplog add netifd 链路拆除 "QMAP 连接已断开" 已断开
}

add_protocol qmi_fm160
