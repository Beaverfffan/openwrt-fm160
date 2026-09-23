#!/bin/sh
# netifd proto handler: FM160 ECM 数据链路（vendor 自拨号 + 模组侧 DHCP/RA）
#
# ECM 模式下模组自身完成 PDP 激活，路由器侧只看到 usb0：模组内置 DHCP
# server 分配 v4，v6 靠模组发 RA（O-flag，stateless）。内核 SLAAC 在本机型
# （forwarding=1 + cdc_ether）上收到 RA 不落地址，所以 v6 交给 odhcp6c 收
# RA 维护（ecm-dhcp.sh 统一托管 udhcpc + odhcp6c）。
# 本 handler 的价值：让 ECM 链路在 LuCI 显示专用协议芯片、可挂防火墙区域、
# mwan3 可跟踪。USB profile 33 的切换由 dial-ctl 在 ifup 前完成。

. /lib/functions.sh
. /lib/netifd/netifd-proto.sh
init_proto "$@"

fm160_log() { logger -t "proto-ecm-fm160" "$*"; }

fm160_v4() { uci -q get fm160.main.dial_v4; }
fm160_v6() { uci -q get fm160.main.dial_v6; }

proto_ecm_fm160_init_config() {
	# 不用 no_device：usb0 有真实载波（PDP 激活后 LOWER_UP），交给 netifd
	# 认领管理；wwan0 raw-ip 无载波才需要 no_device（见 qmi/mbim handler）
	available=1
	proto_config_add_string "device:device"
}

proto_ecm_fm160_setup() {
	local config="$1" iface="$2"
	local i device

	json_get_vars device
	iface=${device:-${iface:-usb0}}

	# 等 usb0 出现（profile 33 下模组枚举后即存在，最长 60s）
	i=0
	while [ ! -d "/sys/class/net/$iface" ] && [ $i -lt 30 ]; do
		sleep 2; i=$((i + 1))
	done
	[ -d "/sys/class/net/$iface" ] || {
		proto_notify_error "$config" "NO_DEV"
		proto_block_restart "$config"
		return
	}
	ip link set dev "$iface" up

	if [ "$(fm160_v6)" != 0 ]; then
		# v6 走内核 SLAAC（模组 RA）。forwarding=1 时必须 accept_ra=2
		# 内核才处理 RA；RA 能否到达内核取决于 fw4 zone（netif_sync 保证）
		sysctl -qw "net.ipv6.conf.$iface.accept_ra=2" 2>/dev/null
		sysctl -qw "net.ipv6.conf.$iface.autoconf=1" 2>/dev/null
	fi

	proto_init_update "$iface" 1
	proto_set_keep 1
	proto_send_update "$config"

	if [ "$(fm160_v4)" != 0 ] || [ "$(fm160_v6)" != 0 ]; then
		proto_export "INTERFACE=$config"
		proto_run_command "$config" /usr/sbin/ecm-dhcp.sh \
			"$iface" "$(fm160_v4)" "$(fm160_v6)"
	fi
	fm160_log "$config: ecm link setup on $iface (v4=$(fm160_v4) v6=$(fm160_v6))"
	/usr/sbin/fm160-oplog add netifd 链路建立 "ECM 拨号已发起 v4=$(fm160_v4) v6=$(fm160_v6)，地址由模组 DHCP/RA 下发" 进行中
}

proto_ecm_fm160_teardown() {
	local config="$1" iface="$2"
	proto_kill_command "$config"
	/etc/init.d/fm160-prefix withdraw >/dev/null 2>&1
	fm160_log "$config: teardown done"
	/usr/sbin/fm160-oplog add netifd 链路拆除 "ECM 连接已断开" 已断开
}

add_protocol ecm_fm160
