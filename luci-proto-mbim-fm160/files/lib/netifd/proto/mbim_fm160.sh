#!/bin/sh
# netifd proto handler: FM160 MBIM 拨号（广和通 vendor CLI 实现，非 umbim）
#
# 拨号动作全部走广和通原厂 CLI（/usr/sbin/mbim-up.sh，内部 GTI 命令），
# 不使用 OpenWrt 原生 umbim/libmbim 栈 —— 广和通 MBIM 实现与原生差异大。
# 拨号参数（APN/协议栈）读 /etc/config/fm160，由 FM160 拨号页管理。
#
# setup:   mbim-up.sh report（只拨号+写 env，不碰网卡）→ 上报 netifd
# teardown: mbim-down.sh 断开 + 收回 LAN 侧运营商前缀

. /lib/functions.sh
. /lib/netifd/netifd-proto.sh
init_proto "$@"

fm160_log() { logger -t "proto-mbim-fm160" "$*"; }

fm160_apn() { uci -q get fm160.main.dial_apn || echo ctnet; }
fm160_v4()  { uci -q get fm160.main.dial_v4; }
fm160_v6()  { uci -q get fm160.main.dial_v6; }

proto_mbim_fm160_init_config() {
	no_device=1
	available=1
	proto_config_add_string "device:device"
}

proto_mbim_fm160_setup() {
	local config="$1" iface="$2"
	local apn iptype i device

	json_get_vars device
	iface=${device:-${iface:-wwan0}}

	apn=$(fm160_apn)
	iptype=ipv4v6
	[ "$(fm160_v4)" = 0 ] && iptype=ipv6
	[ "$(fm160_v6)" = 0 ] && iptype=ipv4

	# 防御性等待控制通道（USB profile 切换由 dial-ctl 事先完成）
	i=0
	while [ ! -c /dev/cdc-wdm0 ] && [ $i -lt 30 ]; do
		sleep 2; i=$((i + 1))
	done
	[ -c /dev/cdc-wdm0 ] || {
		proto_notify_error "$config" "NO_CDC_WDM"
		proto_block_restart "$config"
		return
	}

	rm -f /var/run/fm160-mbim.env
	if ! mbim-up.sh "$apn" "$iptype" report; then
		proto_notify_error "$config" "DIAL_FAILED"
		proto_block_restart "$config"
		return
	fi
	[ -f /var/run/fm160-mbim.env ] || {
		proto_notify_error "$config" "NO_IP_CONFIG"
		proto_block_restart "$config"
		return
	}
	. /var/run/fm160-mbim.env

	local dev="$iface"
	[ -d "/sys/class/net/$dev" ] || {
		proto_notify_error "$config" "NO_DEV"
		proto_block_restart "$config"
		return
	}
	ip link set dev "$dev" up
	[ -n "$IP4MTU" ] && ip link set dev "$dev" mtu "$IP4MTU" 2>/dev/null

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
		proto_add_ipv6_address "$IP6ADDR" "$IP6MASK"
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
	fm160_log "$config: mbim link up on $dev v4=${IP4ADDR:-none} v6=${IP6ADDR:-none}"
	/usr/sbin/fm160-oplog add netifd 链路建立 "MBIM 拨号成功 v4=${IP4ADDR:-无} v6=${IP6ADDR:-无}" 已连接
}

proto_mbim_fm160_teardown() {
	local config="$1" iface="$2"
	mbim-down.sh >/dev/null 2>&1
	# 链路拆除时立刻收回 LAN 侧运营商前缀
	/etc/init.d/fm160-prefix withdraw >/dev/null 2>&1
	fm160_log "$config: teardown done"
	/usr/sbin/fm160-oplog add netifd 链路拆除 "MBIM 连接已断开" 已断开
}

add_protocol mbim_fm160
