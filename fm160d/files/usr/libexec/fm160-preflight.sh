#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# fm160-preflight - make sure nothing else is fighting fm160d for the module.
#
# fm160d is designed around one rule: it is the SINGLE owner of the FM160's AT
# port.  Two things on this image break that rule, and both were measured on
# the device rather than guessed:
#
#   1. adb-enablemodem.  Its S99 init script ends in `adb wait-for-device`,
#      which never returns when no Android device is attached, so the boot
#      stalls on it and the AT port is left half-configured.
#
#   2. A leftover iStoreOS interface on wwan0 (`network.2_1`, proto dhcp,
#      defaultroute 1, metric 11).  netifd then owns the same netdev fm160d is
#      trying to bring up, and the two race: whoever loses, the link never
#      settles.
#
# WHAT THIS SCRIPT MAY DO
#
#   - disable an init script through rc.common's own `disable`;
#   - set `disabled '1'` on the conflicting network section.
#
# WHAT IT MUST NEVER DO
#
#   - delete a network section, or a config file;
#   - change the modem's USB profile, or send it any AT command at all;
#   - touch /etc/config/qmodem, which is reported and then left alone.
#
# It is idempotent and safe to run at any time: `disable` on an already
# disabled service and `uci set disabled=1` on an already disabled section are
# both no-ops, and nothing is reported twice as though it were new.
#
# Called from uci-defaults/99-fm160 at install time.  Run it by hand after
# re-enabling any of these to see the conflict again.

WWAN_IF="${FM160_WWAN_IF:-wwan0}"
CHANGED=0

log() {
	logger -t fm160-preflight "$*"
	echo "$*"
}

# --- 1. adb-enablemodem -------------------------------------------------
#
# Look for it by what it does, not only by its name: any enabled service whose
# script waits for an adb device is the same bug with a different filename.
for f in /etc/rc.d/S*adb* /etc/rc.d/*adb*; do
	[ -e "$f" ] || continue
	name=${f##*/}
	name=${name#S[0-9][0-9]}
	name=${name#K[0-9][0-9]}

	if [ -x "/etc/init.d/$name" ]; then
		log "disabling /etc/init.d/$name: it starts at boot and can block on 'adb wait-for-device', which never returns without an Android device"
		/etc/init.d/"$name" disable
		CHANGED=1
	else
		log "WARNING: $f is enabled but there is no /etc/init.d/$name to disable. Disable it by hand; fm160d needs the AT port to itself."
	fi
done

# --- 2. another interface on the same netdev ----------------------------
#
# Deliberately conservative: the section is DISABLED, never removed.  A
# section deleted here is a setting the user cannot get back, while a disabled
# one shows up in the UI as disabled and can be re-enabled in one click.
for sec in $(uci show network 2>/dev/null |
	     sed -n 's/^network\.\([^=]*\)=interface$/\1/p'); do
	dev=$(uci -q get "network.$sec.device")
	[ -n "$dev" ] || dev=$(uci -q get "network.$sec.ifname")
	[ "$dev" = "$WWAN_IF" ] || continue

	if [ "$(uci -q get "network.$sec.disabled")" = "1" ]; then
		continue
	fi

	proto=$(uci -q get "network.$sec.proto")
	route=$(uci -q get "network.$sec.defaultroute")
	log "disabling network.$sec (proto ${proto:-?} on $WWAN_IF, defaultroute ${route:-0}): netifd and fm160d cannot both own $WWAN_IF. The section is kept, only disabled - re-enable it if you really want netifd to have the interface."
	uci -q set "network.$sec.disabled=1"
	CHANGED=1
done

# --- 3. QModem residue, report only -------------------------------------
#
# Read-only on purpose.  The package is gone from this image (its four rc.d
# symlinks are dangling), but /etc/config/qmodem still says enable_dial 1 on
# /dev/ttyUSB2 - and QModem's dial script would open the same port fm160d uses.
# Whether that file is stale text or an active hazard depends on whether
# anything still reads it, which is a judgement for the person in front of the
# device, not for an install script that runs once and never comes back.
if [ -f /etc/config/qmodem ]; then
	log "note: /etc/config/qmodem is present (left by QModem). Its init script is gone, so nothing should read it, but if the AT port is ever busy for no visible reason, that file names a competing dialler. Not modified."
elif command -v opkg >/dev/null 2>&1 && opkg list-installed 2>/dev/null | grep -q '^qmodem'; then
	log "WARNING: a qmodem package is still installed although /etc/config/qmodem is gone. It may compete for the AT port; fm160d wants to be the only thing opening /dev/ttyUSB*."
fi

if [ "$CHANGED" = "1" ]; then
	uci -q commit network 2>/dev/null
	log "configuration changed; a reboot (or /etc/init.d/network reload) is needed for the network change to take effect"
fi

exit 0
