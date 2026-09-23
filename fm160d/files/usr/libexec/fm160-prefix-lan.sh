#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# fm160-prefix-lan.sh -- give the LAN the operator's /64 for real, by putting it
# on br-lan and teaching the module how to find the hosts behind us.
#
#   fm160-prefix-lan.sh apply      (re)read the wan prefix and install it
#   fm160-prefix-lan.sh withdraw   take the prefix off the LAN, leave odhcpd be
#   fm160-prefix-lan.sh teardown   remove everything this script ever added
#   fm160-prefix-lan.sh status     one screen of what is currently in place
#
# ---------------------------------------------------------------------------
# WHY NOT NPTv6
# ---------------------------------------------------------------------------
#
# The original plan was ULA on the LAN plus NPTv6 (RFC 6296) at the router, so
# that LAN addressing stays stable while the operator prefix changes.  The
# mechanism is all present on this board -- ip6t_NPT is built in, ip6tables-mod-
# nat ships the userspace half, and the legacy backend runs it -- and it even
# rewrites correctly.  Measured, not assumed:
#
#   fd3f:2f90:ff24::dead:beef:1  leaves as  240e:479:1660:2000:cd0d:dead:beef:1
#
# and the cd0d is not a bug: after replacing the top 64 bits ip6t_NPT overwrites
# the first non-0xffff group of the IID with the checksum adjustment, exactly as
# RFC 6296 requires, and for a given prefix pair that value is a constant.
#
# What kills it is hook order, and it cannot be configured around:
#
#   priority -200  conntrack   IPv6 conntrack sees the reply FIRST
#   priority -150  mangle      ...and ip6t_NPT's table is hardcoded to mangle
#
# So the reply is classified NEW while its destination still reads
# 240e:...:cd0d:... -- a connection nobody has a socket for.  The host answers
# its own RST 28 ms later.  A capture says so:
#
#   SYN     -> 240e:978:1509:2:3::28.443
#   SYN-ACK <- 240e:978:1509:2:3::28.443 > 240e:479:1660:2000:cd0d:dead:beef:1
#   RST     -> (from the host, 28 ms later)
#
# with the DNPT counter at 23, so the rewrite happened and was still too late.
# The RFC's own answer is to make the translated traffic stateless
# (section 3.4), and the vehicle for that here would be raw/CT --notrack -- but
# this build's legacy ip6tables has no raw table at all:
#
#   ip6tables v1.8.10 (legacy): can't initialize table 'raw': Table does not
#   exist (do you need to insmod?)
#
# There is no configuration that moves ip6t_NPT ahead of conntrack, so nptv6
# stays out.
#
# ---------------------------------------------------------------------------
# WHAT THIS DOES INSTEAD
# ---------------------------------------------------------------------------
#
# Two measurements on the real link shaped this:
#
#   1. the whole /64 is ours.  An address added with nodad -- never announced,
#      never probed -- completed a TLS handshake to the internet.
#   2. odhcpd reads interface addresses straight off netlink (config.c:987), so
#      a prefix on br-lan is advertised with no help from netifd -- as an
#      on-link PIO, and with a NON-zero router lifetime, because it is a global
#      address and not a ULA.  LAN clients then get a real global address and a
#      default route by ordinary RA.
#
# The remaining half is answering neighbour discovery for those clients.  The
# module resolves the hosts behind us one address at a time:
#
#   ICMP6, neighbor solicitation,  who has 240e:...:1::2
#   ICMP6, neighbor advertisement, tgt is 240e:...:1::2      <- someone answers
#
# and only then does anything come back.  Who answers is a choice, and it was
# measured four ways on this board with a fresh client address each time so the
# module could not answer from cache (each stage also had a control where
# nothing was configured, and that control did fail):
#
#   ndppd, rule { auto }              no advertisement, ever
#   linux proxy_ndp + a host entry    works, but one entry per client
#   nftables + a mirror daemon        works, needs a daemon
#   odhcpd ndp relay                  works, and needs nothing else
#
# odhcpd wins on every axis.  It already tracks the LAN, it answers by itself
# with no kernel proxy entry and without net.proxy_ndp being set at all, and a
# relay covers addresses this router has never seen, so there is no list of
# clients to keep in step and no daemon to babysit.
#
# One trap, worth recording because it cost a full round of measurements:
# odhcpd's `interface` option names a NETWORK section, not a device.  Pointing
# it at network.wan looks right and does nothing, because on this board wan's
# device is eth0 while the radio link is ecm6 on usb0.  The section is chosen
# by looking up which network section owns the wan device.
#
# So, in full:
#
#   br-lan gets <prefix>:1::1/64, plus a route to the /64 at metric 100 so it
#   wins the lookup against the /64 the wan interface already has at 256 --
#   installed with noprefixroute so the kernel does not add a second copy.
#
#   odhcpd is given an upstream section on the wan network with ndp relay, and
#   the LAN section is set to relay too.  Both are needed: odhcpd only keeps a
#   relay on a master interface if some non-master also relays, so configuring
#   one alone silently degrades to disabled.
#
# netfilter needs no rule at all, because nothing is translated any more.
#
# ---------------------------------------------------------------------------
# THE ROUTER'S OWN ADDRESS NEEDS ONE PROXY ENTRY
# ---------------------------------------------------------------------------
#
# The address on br-lan (<prefix>:1::1) is the router's LAN-side address, and
# the kernel answers neighbour solicitation for it only on br-lan.  The module,
# however, resolves it over the mobile link - and when it does, the reply
# packets for anything the router itself sent (its source selection happily
# picks the br-lan address) die right there.  Measured 2026-09-22 on firmware
# .23 with tcpdump on the mobile link:
#
#   ICMP6, neighbor solicitation,  who has 240e:...:1::1   <- module, x5
#   (nothing ever answers)
#
# while LAN clients kept working, because odhcpd's ndp relay answers for them.
# So the router's own address gets exactly one proxy neighbour entry on the
# mobile device (plus the proxy_ndp sysctl that entry needs to be answered):
#
#   ip neigh add proxy <prefix>:1::1 dev usb0
#
# This is NOT the rejected per-client proxy scheme - it is one static entry
# for the one address this script itself put on the bridge, added and removed
# strictly in step with that address.
#
# ---------------------------------------------------------------------------
# WHAT IT DOES NOT TOUCH
# ---------------------------------------------------------------------------
#
# The wan interface keeps its address, its on-link /64 (metric 256), its
# source-qualified default route and its own connectivity: the LAN route wins on
# metric alone, and the router's own address stays reachable because a local
# /128 in the local table outranks any main-table prefix.  Teardown removes
# exactly what this script added and nothing else, including the LAN section's
# previous ndp setting if it had one.

set -e

CFG=fm160
STATE=/var/run/fm160-prefix-lan.state
ODHCPD_STATE=/var/run/fm160-prefix-lan.odhcpd
LEGACY_NDPPD_PID=/var/run/fm160-ndppd.pid
ODHCPD_SEG=fm160ndp
ODHCPD_INIT=/etc/init.d/odhcpd
ROUTE_METRIC=100

log() { logger -t fm160-prefix "$*"; }

# Hotplug events arrive in bursts, and two applies racing over the same address
# and route is a mess that reports itself as "could not add".  A directory is the
# lock; the pid inside it is what lets a run that was killed mid-flight be
# reclaimed instead of wedging every later run out of the prefix forever.
LOCK=/var/lock/fm160-prefix

lock() {
	local i=0
	mkdir -p /var/lock
	while ! mkdir "$LOCK" 2>/dev/null; do
		if [ -f "$LOCK/pid" ] && ! kill -0 "$(cat "$LOCK/pid" 2>/dev/null)" 2>/dev/null; then
			rm -rf "$LOCK"
			continue
		fi
		i=$((i + 1))
		if [ "$i" -gt 30 ]; then
			log "lock still held after 30s, going ahead anyway"
			break
		fi
		sleep 1
	done
	echo $$ > "$LOCK/pid"
}

unlock() { rm -rf "$LOCK" 2>/dev/null; return 0; }

# Everything leaves through here, and a non-zero status leaves a trace in the log.
#
# This is not decoration.  The first version of this script called a helper whose
# last command was `[ -f "$STATE" ] && cat "$STATE"`; on a machine with no state
# file that helper returns 1, `old="$(state_prefix)"` inherits the 1, and `set -e`
# ends the script right there - before the address, the route or the proxy.  All
# three entry points died at the same line, so the plugin installed nothing and
# said nothing, and the only symptom was a LAN without a prefix.  A silent exit
# is the one failure mode nobody investigates, so it is now logged, and every
# helper that is read through $( ) is written to be total.
finish() {
	local rc=$?
	trap - EXIT
	unlock
	[ "$rc" = 0 ] || log "exited with status $rc"
	exit "$rc"
}
trap finish EXIT

# ---------------------------------------------------------------- config ----

cfg_get() { uci -q get "$CFG.main.$1" 2>/dev/null || true; }

enabled() {
	[ "$(cfg_get lan_ipv6)" = "1" ]
	return $?
}

# Which netdev carries the mobile link.  Auto-detected rather than hardcoded:
# ECM is usb0 on this unit, but the name is a function of the USB profile and
# the module's descriptor, and hardcoding it is how a plugin works on exactly
# one firmware.
wan_dev() {
	local d
	d="$(cfg_get lan_ipv6_wan)"
	if [ -n "$d" ] && [ -d "/sys/class/net/$d" ]; then
		echo "$d"
		return 0
	fi
	for d in usb0 wwan0 eth1; do
		[ -d "/sys/class/net/$d" ] || continue
		# the mobile link is the one holding a global, non-ula address
		if ip -6 addr show dev "$d" scope global 2>/dev/null | grep -qi 'inet6'; then
			echo "$d"
			return 0
		fi
	done
	return 1
}

lan_dev() {
	local d
	d="$(cfg_get lan_ipv6_if)"
	[ -n "$d" ] || d=br-lan
	echo "$d"
	return 0
}

# The operator /64 currently on the wan device, as four groups, e.g.
# 240e:479:1660:2000.  Empty when the link is down or the prefix has not
# arrived.  Only /64 is accepted: the ECM link announces exactly one, and
# anything else here would be a prefix this script has never been tested with.
#
# ula (fc00::/7) and link-local are skipped on purpose -- the ULA is the router's
# own, and letting it through would put the ULA on br-lan twice.
wan_prefix() {
	local dev="$1" p
	p="$(ip -6 addr show dev "$dev" scope global 2>/dev/null | awk '
		/inet6/ {
			addr = $2
			split(addr, a, "/")
			if (a[2] != 64) next
			# fc00::/7 and fe80::/10 are not the operator prefix
			if (a[1] ~ /^f[cd]/ || a[1] ~ /^fe[89ab]/) next
			n = split(a[1], g, ":")
			if (n < 4) next
			print g[1]":"g[2]":"g[3]":"g[4]
			exit
		}')"
	[ -n "$p" ] || return 1
	echo "$p"
}

# The state file is the only record of what this script put in place, and it
# carries the device it was installed from as well as the prefix.  The device
# matters at withdraw time: by then the address may already be gone (so probing
# for it answers a different question than the one being asked), while the
# device recorded here is the one this prefix actually came from.
#
# The reader tolerates the older single-line format, because a device that
# upgrades the package mid-flight has one on disk and a parser that only knows
# the new shape would read it as "no prefix installed" and skip the withdraw.
state_prefix() {
	[ -f "$STATE" ] || return 0
	# new format is "prefix=<pfx>"; the older one is the bare prefix
	head -1 "$STATE" 2>/dev/null | sed 's/^prefix=//'
	return 0
}

state_wandev() {
	[ -f "$STATE" ] || return 0
	sed -n 's/^wandev=//p' "$STATE" 2>/dev/null
	return 0
}

state_set()      { printf 'prefix=%s\nwandev=%s\n' "$1" "$2" > "$STATE"; }
state_clear()    { rm -f "$STATE" 2>/dev/null; return 0; }

# --------------------------------------------------------------- odhcpd -----

# The network section that owns a device, preferring the dhcpv6 half when there
# is more than one (ecm and ecm6 share usb0).  This indirection exists because
# odhcpd's `interface` names a section, and a device name is silently accepted
# and does nothing.
net_seg_for() {
	local dev="$1" s
	[ -n "$dev" ] || return 1
	for s in $(uci -q show network 2>/dev/null | sed -n "s/^network\.\([^.=]*\)\.device='$dev'\$/\1/p"); do
		if [ "$(uci -q get "network.$s.proto" 2>/dev/null)" = "dhcpv6" ]; then
			echo "$s"
			return 0
		fi
	done
	for s in $(uci -q show network 2>/dev/null | sed -n "s/^network\.\([^.=]*\)\.device='$dev'\$/\1/p"); do
		echo "$s"
		return 0
	done
	return 1
}

# What the LAN section had before we touched it, so teardown can put it back.
# Written once, on the first apply, never overwritten: on a second apply the
# value being read would be our own.
odhcpd_state_save() {
	local lannet="$2" lanndp=""
	[ -n "$lannet" ] && lanndp="$(uci -q get "dhcp.$lannet.ndp" 2>/dev/null || true)"
	{
		printf 'wanseg=%s\n' "$1"
		printf 'lannet=%s\n' "$lannet"
		printf 'lanndp=%s\n' "$lanndp"
	} > "$ODHCPD_STATE"
	return 0
}

odhcpd_state_field() {
	[ -f "$ODHCPD_STATE" ] || return 0
	sed -n "s/^$1=//p" "$ODHCPD_STATE" 2>/dev/null | head -1
	return 0
}

odhcpd_state_clear() { rm -f "$ODHCPD_STATE" 2>/dev/null; return 0; }

# True when the uci database already says exactly what we would write.  Applying
# on every hotplug event must not restart odhcpd and blink the LAN for no
# reason, and this is the common case.
odhcpd_configured() {
	local wanseg="$1" lannet="$2"
	[ "$(uci -q get "dhcp.$ODHCPD_SEG.interface" 2>/dev/null)" = "$wanseg" ] || return 1
	[ "$(uci -q get "dhcp.$ODHCPD_SEG.master" 2>/dev/null)" = "1" ] || return 1
	[ "$(uci -q get "dhcp.$ODHCPD_SEG.ndp" 2>/dev/null)" = "relay" ] || return 1
	# Checked here and not only written below, because a device upgrading from
	# a version of this plugin that predates the option has every other field
	# of this section already correct: without this line the section compares
	# equal, odhcpd is left alone, and the /128 defect it is here to prevent
	# survives the upgrade.  An unset option reads empty and so is not "0".
	[ "$(uci -q get "dhcp.$ODHCPD_SEG.ndproxy_routing" 2>/dev/null)" = "0" ] || return 1
	if [ -n "$lannet" ]; then
		[ "$(uci -q get "dhcp.$lannet.ndp" 2>/dev/null)" = "relay" ] || return 1
	fi
	return 0
}

odhcpd_restart() {
	[ -x "$ODHCPD_INIT" ] || {
		log "$ODHCPD_INIT is missing; the LAN will have no ndp relay"
		return 1
	}
	"$ODHCPD_INIT" restart >/dev/null 2>&1 || true
	# odhcpd has to come back up before it will answer anything, and a client
	# that asks in the meantime simply gets retried by the module
	sleep 2
	return 0
}

# The relay section carries one more option, and it is the difference between a
# LAN that works and one that stops working the moment a client joins it.
#
# odhcpd installs a /128 route for every address it sees a neighbour entry for,
# on the interface that entry is on, gated only by the `learn_routes` field:
#
#   ndp.c  netevent NEIGH6_ADD  ->  setup_route(addr, iface, add)
#          setup_route():  if (iface->learn_routes)
#                              netlink_setup_route(addr, 128, iface->ifindex,
#                                                  NULL, 1024, add)
#
# `learn_routes` defaults to 1 and is set by the `ndproxy_routing` option.
#
# Neighbour entries appear on the wan device for addresses that live on the
# bridge, for two reasons that are both by design here: a LAN packet forwarded
# at layer 3 leaves carrying the client's own source address, so the module
# learns client <-> our MAC from outbound traffic alone; and the relay itself
# echoes DAD solicitations out that device (handle_solicit -> ping6(target, c),
# and ping6 is not gated by learn_routes).  So the wan device accumulates:
#
#   240e:479:1660:2000:54fb:6ff:fe95:670a dev usb0 proto static metric 1024
#   240e:479:1660:2000:1::1               dev usb0 proto static metric 1024
#
# the second of which is this router's own address on the bridge.  A /128 beats
# the /64 this script installs on br-lan no matter what the metrics are, because
# longest prefix wins before metric is ever consulted, so every packet for one
# of those hosts goes out the mobile link, meets the module's own neighbour
# entry pointing back at us, and dies there.  Measured: 100% loss in both
# directions for a client that had been working a moment earlier, and moving
# that single route to br-lan by hand brought it back immediately.
#
# With the option off the route is never installed and the /64 on the bridge
# carries those hosts the way it should.  Nothing else is lost: learn_routes
# gates the route and not the relay, and handle_solicit's echo out the wan
# device is unconditional.  Measured end to end with the option off and every
# stale /128 cleared: router -> client 2/2, client -> internet 3/3.
#
# It goes on the wan section and not the LAN one because the route was installed
# for the interface the neighbour entry was on, and that is the wan device.  The
# LAN section keeps the default; a /128 learned on the bridge points at the
# bridge, which is where those hosts actually are.
odhcpd_configure() {
	local wanseg="$1" lannet="$2"

	[ -f "$ODHCPD_STATE" ] || odhcpd_state_save "$wanseg" "$lannet"

	if odhcpd_configured "$wanseg" "$lannet"; then
		return 0
	fi

	uci -q set "dhcp.$ODHCPD_SEG=dhcp"
	uci -q set "dhcp.$ODHCPD_SEG.interface=$wanseg"
	uci -q set "dhcp.$ODHCPD_SEG.master=1"
	uci -q set "dhcp.$ODHCPD_SEG.ndp=relay"
	uci -q set "dhcp.$ODHCPD_SEG.ndproxy_routing=0"
	# this section is here to relay ndp and nothing else: no addresses to
	# hand out upstream, no ra to send upstream
	uci -q set "dhcp.$ODHCPD_SEG.ra=disabled"
	uci -q set "dhcp.$ODHCPD_SEG.dhcpv6=disabled"
	uci -q set "dhcp.$ODHCPD_SEG.ignore=1"
	[ -n "$lannet" ] && uci -q set "dhcp.$lannet.ndp=relay"

	if ! uci -q commit dhcp; then
		log "could not commit the odhcpd configuration"
		return 1
	fi
	odhcpd_restart || return 1
	log "odhcpd ndp relay: upstream $wanseg, LAN ${lannet:-none}; neighbour route learning off"
	return 0
}

odhcpd_unconfigure() {
	local lannet lanndp
	lannet="$(odhcpd_state_field lannet)"
	lanndp="$(odhcpd_state_field lanndp)"

	uci -q delete "dhcp.$ODHCPD_SEG" 2>/dev/null || true
	if [ -n "$lannet" ]; then
		if [ -n "$lanndp" ]; then
			uci -q set "dhcp.$lannet.ndp=$lanndp"
		else
			uci -q delete "dhcp.$lannet.ndp" 2>/dev/null || true
		fi
	fi
	uci -q commit dhcp 2>/dev/null || true
	odhcpd_state_clear

	# only restart if we had actually changed something
	if [ -n "$lannet" ]; then
		odhcpd_restart || true
		log "odhcpd ndp relay removed"
	fi
	return 0
}

# Earlier versions of this plugin proxied with ndppd.  It is not used any more,
# and a leftover daemon from one of those builds would keep answering on its
# own and mask whether this version works, so a stale one is stopped.
legacy_ndppd_stop() {
	[ -f "$LEGACY_NDPPD_PID" ] || return 0
	kill "$(cat "$LEGACY_NDPPD_PID" 2>/dev/null)" 2>/dev/null || true
	rm -f "$LEGACY_NDPPD_PID"
	return 0
}

# ---------------------------------------------------------------- apply -----

remove_prefix() {
	local pfx="$1" lan="$2"
	[ -n "$pfx" ] || return 0
	ip -6 route del "$pfx::/64" dev "$lan" metric "$ROUTE_METRIC" 2>/dev/null || true
	ip -6 addr del "$pfx:1::1/64" dev "$lan" 2>/dev/null || true
}

# Host routes for the operator's own /64 sitting on the wan device are the one
# thing that can undo everything apply() just did, and turning the option off
# does not remove the ones already in the kernel -- odhcpd forgets an address
# when its neighbour entry goes, and a neighbour entry for a host that is still
# passing traffic does not go.  So they are removed here, every apply, which
# also makes the fix self-healing on a device that upgrades into it.
#
# Metrics are deliberately not part of the test.  There are two ways odhcpd can
# put a /128 on this device and they do not carry the same one: the neighbour
# path uses 1024, and the echo it sends to prime the module's cache adds a
# temporary one at 128 that it removes a moment later -- except when it is
# interrupted between the two, and then it stays.  Both are wrong in the same
# way and both go.
#
# The operator prefix bounds the deletion, so nothing else in the table can be
# touched, and the device bounds it again so no bridge route is ever a
# candidate.  A /128 for the module's own address or for a host on the operator
# side is not useful either -- it falls back to the /64 route on the wan device,
# which still reaches them -- so removing it costs nothing there too.
wan_route_cleanup() {
	local pfx="$1" dev="$2" a n=0
	[ -n "$pfx" ] && [ -n "$dev" ] || return 0
	for a in $(ip -6 route show 2>/dev/null | awk -v p="$pfx:" -v d="$dev" '
			substr($1, 1, length(p)) == p && $2 == "dev" && $3 == d &&
			index($1, "/") == 0 { print $1 }'); do
		a="${a%/128}"
		ip -6 route del "$a/128" dev "$dev" 2>/dev/null || true
		n=$((n + 1))
	done
	[ "$n" = 0 ] || log "removed $n stale /128 route(s) from $dev inside $pfx::/64"
	return 0
}

# The proxy entry that lets the module resolve the router's own LAN address;
# see the header.  One address, on the mobile device only, created and
# destroyed strictly in step with the bridge address it stands for.  Both
# helpers are total: every caller runs under set -e and a duplicate add or a
# missing delete must not abort an apply.
proxy_add() {
	local addr="$1" dev="$2"
	[ -n "$addr" ] && [ -n "$dev" ] || return 0
	[ -e "/proc/sys/net/ipv6/conf/$dev" ] || return 0
	sysctl -qw "net.ipv6.conf.$dev.proxy_ndp=1" 2>/dev/null || true
	ip neigh add proxy "$addr" dev "$dev" 2>/dev/null || true
	return 0
}

proxy_del() {
	local addr="$1" dev="$2"
	[ -n "$addr" ] && [ -n "$dev" ] || return 0
	ip neigh del proxy "$addr" dev "$dev" 2>/dev/null || true
	return 0
}

apply() {
	local dev lan pfx old pfx6 gw6 wanseg lannet

	if ! enabled; then
		teardown
		return 0
	fi

	lan="$(lan_dev)"
	legacy_ndppd_stop

	if ! dev="$(wan_dev)"; then
		log "no mobile link found; leaving the LAN prefix as it is"
		return 0
	fi

	if ! pfx="$(wan_prefix "$dev")"; then
		# The link is up but has no usable /64 yet.  Tear the old prefix down
		# rather than leave it behind: a stale /64 on the LAN is a prefix the
		# clients keep using for an address the network no longer routes.
		old="$(state_prefix)"
		if [ -n "$old" ]; then
			log "operator prefix is gone; withdrawing $old::/64 from $lan"
			proxy_del "$old:1::1" "$(state_wandev)"
			remove_prefix "$old" "$lan"
			state_clear
		fi
		return 0
	fi

	old="$(state_prefix)"

	# Before either branch, because a stale host route on the wan device is a
	# more specific match than the /64 route that is about to be installed: if
	# it were cleared afterwards, the window in between would still be broken.
	#
	# Only the current prefix needs this.  A host route left over from a
	# previous prefix has no /64 route to compete with any more -- that one is
	# removed along with the prefix -- so it sends those packets to the default
	# route on the same device the /128 did, and the host is no worse off.
	wan_route_cleanup "$pfx" "$dev"

	if [ "$old" = "$pfx" ]; then
		# Same prefix: only make sure the pieces are still there.  Cheaper and
		# far less disruptive than tearing down and rebuilding on every
		# hotplug event, and it is the common case.
		if ! ip -6 addr show dev "$lan" 2>/dev/null | grep -q "$pfx:1::1/64"; then
			log "prefix $pfx::/64 still current but its LAN address is missing; reinstalling"
			remove_prefix "$pfx" "$lan"
		else
			# Everything is already in place -- but one field of the record
			# may not be: the device this prefix came from was added to the
			# state file after the state file itself, so an install that has
			# been running since before then has the prefix and no device.
			#
			# That matters more than it looks.  withdraw treats a missing
			# device as "not told, therefore yes, withdraw" -- the right
			# default for a hotplug event, and the wrong one for a record
			# that merely predates the field: it would take the LAN's IPv6
			# away on an ifdown of anything at all.  Fill it in on the first
			# apply that notices, which is also the upgrade path.
			[ -n "$(state_wandev)" ] || state_set "$pfx" "$dev"

			# The router-address proxy belongs to this same "already
			# installed" check: an install made before it existed has
			# the address and no proxy, and router-originated v6 keeps
			# blackholing until something puts it back.
			proxy_add "$pfx:1::1" "$dev"

			wanseg="$(net_seg_for "$dev" 2>/dev/null || echo '')"
			lannet="$(net_seg_for "$lan" 2>/dev/null || echo '')"
			if [ -n "$wanseg" ]; then
				odhcpd_configure "$wanseg" "$lannet"
			else
				log "no network section owns $dev; leaving odhcpd alone"
			fi
			return 0
		fi
	fi

	if [ -n "$old" ] && [ "$old" != "$pfx" ]; then
		log "operator prefix changed $old -> $pfx"
		proxy_del "$old:1::1" "$(state_wandev)"
		remove_prefix "$old" "$lan"
	fi

	pfx6="$pfx::/64"
	gw6="$pfx:1::1"

	# noprefixroute keeps the kernel from adding its own connected route, so the
	# only thing that changes in the fib is the one route below -- at metric 100
	# it wins against the wan interface's own /64 at 256, which is what makes
	# inbound traffic for the LAN route to br-lan instead of dying on usb0.
	if ! ip -6 addr add "$gw6/64" dev "$lan" nodad noprefixroute 2>/dev/null; then
		log "could not add $gw6/64 to $lan"
		return 1
	fi
	if ! ip -6 route add "$pfx6" dev "$lan" metric "$ROUTE_METRIC" 2>/dev/null; then
		# route add is the one step whose failure would silently blackhole the
		# link, so undo the address rather than leave a half-installed prefix.
		log "could not route $pfx6 to $lan; undoing the address"
		ip -6 addr del "$gw6/64" dev "$lan" 2>/dev/null || true
		return 1
	fi

	state_set "$pfx" "$dev"
	proxy_add "$gw6" "$dev"

	# odhcpd picks the address up off netlink by itself and starts advertising
	# it.  What it cannot guess is that it should also answer neighbour
	# discovery for the hosts behind this router.
	log "operator $pfx6 is now the LAN prefix ($gw6 on $lan, route metric $ROUTE_METRIC)"

	wanseg="$(net_seg_for "$dev" 2>/dev/null || echo '')"
	lannet="$(net_seg_for "$lan" 2>/dev/null || echo '')"
	if [ -n "$wanseg" ]; then
		odhcpd_configure "$wanseg" "$lannet"
	else
		log "no network section owns $dev; the LAN has the prefix but inbound will not resolve"
	fi
}

teardown() {
	local lan old
	lan="$(lan_dev)"
	old="$(state_prefix)"
	if [ -n "$old" ]; then
		proxy_del "$old:1::1" "$(state_wandev)"
		remove_prefix "$old" "$lan"
		state_clear
		log "removed $old::/64 from $lan"
	fi
	odhcpd_unconfigure
	legacy_ndppd_stop
	return 0
}

# ------------------------------------------------------------- withdraw -----

# Take the prefix OFF the LAN and touch nothing else.
#
# apply deliberately leaves the prefix alone when it cannot find a mobile link,
# on the reasoning that one blind moment is not evidence the prefix is gone --
# and that is the right call, because hotplug events arrive in bursts and a
# teardown/rebuild cycle on every one of them would be worse than waiting.
#
# But it is exactly the wrong answer at the one event that means it: the mobile
# interface going down.  odhcpd reads the prefixes it advertises straight off
# the interface address list, so while the operator address is still on br-lan
# every router advertisement keeps carrying it -- and the clients keep renewing
# an address for a network they can no longer reach, because the default route
# they would send through left with the wan address.  What they get is silence,
# renewed indefinitely.  That presents as "the LAN has IPv6 but nothing works",
# which is a search with no obvious starting point.
#
# Withdrawing stops the renewal: the address is gone, so the next advertisement
# has no such prefix in it, and the clients age the address out instead of
# renewing it.  Measured, the withdrawal is not accompanied by an immediate
# advertisement with a zero lifetime -- odhcpd does not retract, it simply
# stops mentioning, and the clients' own valid lifetime (5400 s at the defaults)
# is what retires the address.
#
# What it deliberately does NOT do is cut anyone off.  Measured on this board,
# a client whose prefix has just been withdrawn from the bridge still passes
# traffic, because:
#
#   * its outbound packets leave by the wan's default route, which is
#     source-qualified (from <prefix>::/64) and does not consult the /64 route
#     this script installs at all;
#   * return traffic reaches it over the neighbour entry the module holds for
#     it, which the module built from the client's own outbound packets --
#     those leave carrying the client's address, because nothing here is
#     translated.  No host route is involved: measured with every /128 on the
#     wan device cleared, and the option that installs them off, both
#     directions pass.
#
# That is the right shape for this entry point.  It is called when a link is
# going away and coming back, possibly within seconds; taking the router's
# claim to the /64 out of the table is the honest thing to do, while cutting
# every LAN host's IPv6 off for the duration of a reconnect is not.
#
# Two further differences from teardown:
#
#   * the odhcpd relay configuration is left alone.  Restarting odhcpd resets
#     every RA and DHCPv6 state on the LAN, and doing that on every reconnect
#     is a visible blink.  A relay that has nothing to answer for a few seconds
#     is not a problem either -- the module retries -- so it is left running,
#     untouched.
#
#   * it does not require the mobile device to still exist.  That is the whole
#     point: by the time ifdown runs, the device may already be gone, and
#     that is precisely the case apply declines to act on.
#
# Usage: withdraw [<device the event is about>]
#
# The optional argument is the DEVICE from the hotplug event, and it answers one
# question honestly: is this event about the mobile link at all?  netifd fires
# ifdown for every interface it knows, and this board has four names pointed at
# the radio (wan, wan6, ecm, ecm6) of which one owns the device carrying the
# prefix.  An ifdown on the others must not take the LAN's IPv6 away.
#
# The comparison is against the device recorded when this prefix was installed,
# not against one probed now.  Probing at withdraw time asks the wrong question:
# the address may already be off the device, which is exactly the state that
# makes the probe fail.  The recorded device is the one the prefix came from.
#
# No argument means "not told", and that is treated as yes -- withdrawing a
# prefix whose link is fine costs one apply on the next event, whereas failing
# to withdraw leaves the LAN on a dead prefix with no visible cause.  Those two
# are not equally bad.
withdraw() {
	local evdev="$1" lan old olddev
	lan="$(lan_dev)"
	old="$(state_prefix)"
	[ -n "$old" ] || return 0

	if [ -n "$evdev" ]; then
		olddev="$(state_wandev)"
		if [ -n "$olddev" ] && [ "$evdev" != "$olddev" ]; then
			log "ifdown on $evdev is not the mobile link ($olddev); keeping $old::/64"
			return 0
		fi
	fi

	remove_prefix "$old" "$lan"
	proxy_del "$old:1::1" "$(state_wandev)"
	state_clear
	log "withdrew $old::/64 from $lan; the mobile link is going down"
	return 0
}

status() {
	local dev lan pfx old wandev wanseg lannet relay learn
	lan="$(lan_dev)"
	dev="$(wan_dev 2>/dev/null || echo '?')"
	old="$(state_prefix)"
	wandev="$(state_wandev)"
	pfx="$(wan_prefix "$dev" 2>/dev/null || echo '')"
	wanseg="$(net_seg_for "$dev" 2>/dev/null || echo '')"
	lannet="$(net_seg_for "$lan" 2>/dev/null || echo '')"
	relay="$(uci -q get "dhcp.$ODHCPD_SEG.ndp" 2>/dev/null || echo '')"
	learn="$(uci -q get "dhcp.$ODHCPD_SEG.ndproxy_routing" 2>/dev/null || echo '')"

	printf 'enabled        %s\n' "$(enabled && echo yes || echo no)"
	printf 'wan device     %s\n' "$dev"
	printf 'lan device     %s\n' "$lan"
	printf 'operator /64   %s\n' "${pfx:+$pfx::/64}"
	printf 'installed      %s\n' "${old:+$old::/64}${wandev:+ (from $wandev)}"
	printf 'lan address    %s\n' "$(ip -6 addr show dev "$lan" scope global 2>/dev/null | awk '/inet6/ {print $2}' | grep -v '^fd' | tr '\n' ' ')"
	printf 'lan route      %s\n' "$(ip -6 route show "$pfx::/64" 2>/dev/null | head -1)"
	printf 'wan network    %s\n' "${wanseg:-?}"
	printf 'lan network    %s\n' "${lannet:-?}"
	printf 'odhcpd relay   %s\n' "${relay:-not configured}"
	if [ "$learn" = "0" ]; then
		printf 'route learning off, as it has to be\n'
	else
		printf 'route learning %s\n' "${learn:-unset (default is on)} -- host routes will land on $dev and break the LAN"
	fi
	printf 'stray /128s    %s\n' "$(ip -6 route show 2>/dev/null | grep "^${pfx}:" | grep "dev $dev" | grep -v '/' | wc -l)"
	if [ -n "$old" ] && [ -n "$wandev" ] && \
	   ip -6 neigh show proxy dev "$wandev" 2>/dev/null | grep -q "${old}:1::1"; then
		printf 'router proxy   %s\n' "present on $wandev"
	else
		printf 'router proxy   %s\n' "absent - router-originated v6 will blackhole"
	fi
	printf 'odhcpd section %s\n' "$(uci -q show "dhcp.$ODHCPD_SEG" 2>/dev/null | tr '\n' ' ' | cut -c1-150)"
	return 0
}

case "$1" in
	apply)    lock; apply ;;
	withdraw) lock; withdraw "$2" ;;
	teardown) lock; teardown ;;
	status)   status ;;
	*)
		echo "usage: $0 apply|withdraw [device]|teardown|status" >&2
		exit 1
		;;
esac
