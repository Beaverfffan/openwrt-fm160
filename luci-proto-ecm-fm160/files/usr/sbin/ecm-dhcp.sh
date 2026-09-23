#!/bin/sh
# ECM IPv4 地址获取：模组内置 DHCP server (udhcpc)。
# v6 走内核 SLAAC（模组 RA + accept_ra=2，见 proto ecm_fm160；RA 放行
# 依赖 fm160 接口挂在 fw4 zone 内，由 dial-ctl netif_sync 保证）。
# 由 proto ecm_fm160 通过 proto_run_command 托管，TERM 时杀掉 udhcpc。
# 用法: ecm-dhcp.sh <iface> <v4: 0|1> <v6: 0|1>   （v6 参数仅为接口一致保留）

IFACE=$1
V4=${2:-1}

[ "$V4" = 0 ] && exit 0

udhcpc -p /var/run/udhcpc-$IFACE.pid \
	-s /lib/netifd/dhcp.script \
	-f -t 0 -i "$IFACE" \
	-x "hostname:$(cat /proc/sys/kernel/hostname 2>/dev/null)"
