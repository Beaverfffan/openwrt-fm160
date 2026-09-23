# SPDX-License-Identifier: GPL-2.0-or-later
#
# fm160d - policy layer for the Fibocom FM160.
#
# The transport (ubus object "at-daemon") is provided by the separate package
# "ubus-at-daemon", vendored in ../at-daemon.

include $(TOPDIR)/rules.mk

PKG_NAME:=fm160d
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=Beaverfffan

include $(INCLUDE_DIR)/package.mk

define Package/fm160d
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Fibocom FM160 modem manager daemon (policy layer)
  DEPENDS:=+libubus +libubox +libblobmsg-json +ubus-at-daemon
endef

define Package/fm160d/description
 fm160d is the policy layer for a single Fibocom FM160 module.  It owns the
 modem's AT port through ubus-at-daemon, runs a tiered polling scheduler that
 is deliberately gentle on the Qualcomm AT parser, and exposes one ubus object
 ("fm160") that the LuCI front end talks to.  All AT traffic on the device is
 funnelled through this daemon.

 It also carries the LAN's IPv6 for an ECM link: /etc/init.d/fm160-prefix and
 /usr/libexec/fm160-prefix-lan.sh put the operator's /64 on the LAN bridge so
 that odhcpd can advertise it, and give odhcpd an ndp relay on the mobile
 interface so inbound traffic can resolve.  The relay matters because the modem
 resolves every destination one address at a time; odhcpd is used rather than a
 separate proxy daemon because it already tracks the LAN, covers addresses it
 has never seen, and needs no per-client entries to keep in step.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

# -Wextra is worth keeping for -Wformat-truncation, which caught five silent
# truncations in this code.  -Wno-unused-parameter only mutes the ubus method
# handlers, whose signature is fixed by the API and cannot shrink to the
# argument subset each handler actually uses.
FM160D_CFLAGS = $(TARGET_CFLAGS) -Wall -Wextra -Wno-unused-parameter

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		$(TARGET_CONFIGURE_OPTS) \
		CFLAGS="$(FM160D_CFLAGS) -I$(STAGING_DIR)/usr/include" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/fm160d/conffiles
/etc/config/fm160
endef

define Package/fm160d/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fm160d $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/fm160d $(1)/etc/init.d/
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/fm160 $(1)/etc/config/
	$(INSTALL_DIR) $(1)/etc/hotplug.d/tty
	$(INSTALL_BIN) ./files/etc/hotplug.d/tty/10-fm160-port $(1)/etc/hotplug.d/tty/
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/etc/uci-defaults/99-fm160 $(1)/etc/uci-defaults/
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) ./files/usr/libexec/fm160-preflight.sh $(1)/usr/libexec/
	$(INSTALL_DIR) $(1)/lib/netifd/proto
	$(INSTALL_BIN) ./files/lib/netifd/proto/fm160.sh $(1)/lib/netifd/proto/

	# The LAN's IPv6.  The init script is enabled by uci-defaults and does
	# nothing at all until fm160.main.lan_ipv6 is set, so enabling it is not a
	# behaviour change for anyone who never turns the option on.
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) ./files/usr/libexec/fm160-prefix-lan.sh $(1)/usr/libexec/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/fm160-prefix $(1)/etc/init.d/
	$(INSTALL_DIR) $(1)/etc/hotplug.d/iface
	$(INSTALL_BIN) ./files/etc/hotplug.d/iface/31-fm160-prefix $(1)/etc/hotplug.d/iface/
endef

$(eval $(call BuildPackage,fm160d))
