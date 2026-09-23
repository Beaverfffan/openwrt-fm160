#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# fm160 - a netifd protocol handler for the FM160's ECM interface.
#
# WHY THIS EXISTS
#
# ECM is not "the netdev comes up and DHCP does the rest".  The module keeps the
# PDP context closed until something activates it from INSIDE with an AT
# command, and the vendor is explicit that the interface can look perfectly up
# while carrying nothing.  netifd has no way to send AT, so on QMI and MBIM the
# kernel stack does that job (uqmi/umbim) and here it does not exist at all.
#
# WHAT THIS SCRIPT DOES *NOT* DO
#
# It never opens a serial port and never sends an AT command.  fm160d is the
# single owner of the AT port by design (PLAN D3), so this handler asks it over
# ubus instead: "make the context active", "is it active yet", "take it down".
# A second writer on that port is exactly the failure this project exists to
# avoid - two processes interleaving on a modem whose AT parser is easily
# confused.
#
# THE SEQUENCE
#
#   1. find the ECM netdev (usb0 by default).  It may not exist yet: after a USB
#      profile switch the module re-enumerates, and netifd can call setup()
#      before the kernel has finished creating the interface.
#   2. hand the APN/context settings to fm160d and ask it to dial.
#   3. wait, bounded, for the daemon to report the context active.
#   4. hand the interface to udhcpc.  The module runs a DHCP server on the ECM
#      interface, so the address comes from the module - DESIGN 4.2 step 7.
#
# The wait in step 3 is bounded on purpose.  A proto handler that waits forever
# is an interface that never reports a failure: netifd would leave it "pending"
# and the user would see nothing at all.

[ -n "$INCLUDE_ONLY" ] || {
	. /lib/functions.sh
	. ../netifd-proto.sh
	init_proto "$@"
}

# Default ECM netdev for this module: cdc_ether names it usb0, and the FM160
# declares one ECM interface in profiles 33/18/23/19.  Overridable, because the
# kernel's naming is not a promise.
FM160_ECM_DEV="usb0"
# How long to wait for the netdev, and then for the context.  The second number
# is generous on purpose: registration plus activation is documented as taking
# tens of seconds, and a setup that gives up early produces a retry storm
# against the one resource that must not be hammered.
FM160_DEV_WAIT=20
FM160_DIAL_WAIT=120

fm160_ubus() {
	ubus call fm160 "$@" 2>/dev/null
}

# One field out of the daemon's snapshot, or "" when it cannot be read.
# jsonfilter ships with OpenWrt; when it cannot be used we simply do not block,
# because a missing tool must not turn into an interface that never comes up.
fm160_field() {
	command -v jsonfilter >/dev/null 2>&1 || return 1
	fm160_ubus status | jsonfilter -e "$1" 2>/dev/null
}

proto_fm160_init_config() {
	no_device=1
	available=1

	proto_config_add_string "device:device"
	proto_config_add_string "apn"
	proto_config_add_string "pdp"
	proto_config_add_string "cid"
	proto_config_add_string "allow_reset"
	proto_config_add_string "dial_wait"
}

proto_fm160_setup() {
	local interface="$1"
	local device apn pdp cid allow_reset dial_wait
	local json_allow_reset=false
	local i=0 waited=0 res up step why

	json_get_vars device apn pdp cid allow_reset dial_wait

	device="${device:-$FM160_ECM_DEV}"
	[ -n "$dial_wait" ] || dial_wait="$FM160_DIAL_WAIT"
	[ "$allow_reset" = "1" ] && json_allow_reset=true

	# --- 1. the netdev ------------------------------------------------
	i=0
	while [ ! -e "/sys/class/net/$device" ]; do
		i=$((i + 1))
		if [ "$i" -gt "$FM160_DEV_WAIT" ]; then
			echo "fm160: $device did not appear within ${FM160_DEV_WAIT}s" >&2
			proto_notify_error "$interface" FM160_NO_DEVICE
			return 1
		fi
		sleep 1
	done

	# --- 2. hand the settings over, and dial ---------------------------
	#
	# dial_config writes the values to uci AND applies them to the running
	# daemon, so the file and the daemon cannot disagree.  Empty values are
	# not sent at all: the daemon already has whatever uci holds.
	fm160_ubus dial_config \
		"{\"apn\":\"${apn}\",\"pdp\":\"${pdp}\",\"cid\":${cid:-1},\"allow_reset\":${json_allow_reset}}" \
		>/dev/null

	res=$(fm160_ubus dial_start | jsonfilter -e '@.result' 2>/dev/null)
	case "$res" in
	started|"")
		;;
	bad-config)
		echo "fm160: fm160d refused to dial: no usable APN is configured" >&2
		proto_notify_error "$interface" FM160_NO_APN
		return 1
		;;
	wrong-profile)
		echo "fm160: the modem is not in an ECM USB profile; use proto qmi or proto mbim instead" >&2
		proto_notify_error "$interface" FM160_WRONG_PROFILE
		return 1
		;;
	not-ready)
		# Transient: no AT port yet, or the profile is still being read.
		# netifd may try again.
		echo "fm160: fm160d is not ready to dial yet" >&2
		proto_notify_error "$interface" FM160_NOT_READY
		return 1
		;;
	*)
		echo "fm160: dial_start answered '$res'" >&2
		proto_notify_error "$interface" FM160_DIAL_FAILED
		return 1
		;;
	esac

	# --- 3. wait for the context --------------------------------------
	#
	# dial.up is the daemon's own view, and it is the only one that can tell
	# "the interface is up" from "the PDP context is active".  The
	# distinction matters: an ECM interface with no context is up and
	# carries nothing, and reporting that as success is how a user ends up
	# debugging DNS instead of the modem.
	while :; do
		up=$(fm160_field '@.dial.up')
		[ "$up" = "true" ] && break

		waited=$((waited + 1))
		if [ "$waited" -gt "$dial_wait" ]; then
			step=$(fm160_field '@.dial.step')
			why=$(fm160_field '@.dial.last_error')
			echo "fm160: the data context did not come up within ${dial_wait}s (step: ${step:-unknown}${why:+, $why})" >&2
			proto_notify_error "$interface" FM160_TIMEOUT
			return 1
		fi
		sleep 1
	done

	# --- 4. the interface and its DHCP client --------------------------
	#
	# The module runs a DHCP server on the ECM interface, so the address
	# comes from the module and not from the AT read-back.  The AT address
	# is kept in the snapshot as evidence, never configured from here.
	proto_init_update "$device" 1
	proto_set_keep 1
	proto_send_update "$interface"

	proto_run_command "$interface" /usr/sbin/udhcpc \
		-p "/var/run/udhcpc-$interface.pid" \
		-s /usr/share/udhcpc/default.script \
		-t 15 -T 10 -A 3 -f -q \
		-i "$device"
}

proto_fm160_teardown() {
	local interface="$1"
	local device

	json_get_vars device
	device="${device:-$FM160_ECM_DEV}"

	# The vendor's red line, and the reason this is a call to the daemon
	# rather than an interface down: "disconnecting" on this module means
	# AT+GTWWAN=0,<cid> and nothing else.  Pulling the cable, restarting
	# netifd or powering the module down leaves the session active and
	# billable with nobody using it.
	fm160_ubus dial_stop >/dev/null

	if [ -e "/var/run/udhcpc-$interface.pid" ]; then
		kill "$(cat "/var/run/udhcpc-$interface.pid")" 2>/dev/null
		rm -f "/var/run/udhcpc-$interface.pid"
	fi

	proto_init_update "$device" 0
	proto_send_update "$interface"
}

add_proto fm160 fm160
