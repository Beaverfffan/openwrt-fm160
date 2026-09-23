# SPDX-License-Identifier: GPL-2.0-or-later
#
# luci-proto-ecm-fm160 - LuCI protocol definition + netifd handler for the
# Fibocom FM160 ECM data link (module self-dial + module-side DHCP/RA).

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-proto-ecm-fm160
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_BUILD_DEPENDS:=luci-base/host

PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/luci-proto-ecm-fm160
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=5. Interfaces
  TITLE:=LuCI support for FM160 ECM protocol
  DEPENDS:=+luci-base +fibocom-dial
endef

define Package/luci-proto-ecm-fm160/description
  netifd protocol handler and LuCI protocol chip for the FM160 ECM data
  link.  In ECM mode the module performs the PDP activation itself; the
  router runs a DHCP client on usb0 for IPv4 and kernel SLAAC (module RA)
  for IPv6.  USB profile switching is done by the FM160 dial page
  (luci-app-fm160) via /etc/config/fm160.
endef

define Build/Compile
endef

define Package/luci-proto-ecm-fm160/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/protocol
	$(INSTALL_DATA) ./htdocs/luci-static/resources/protocol/ecm_fm160.js $(1)/www/luci-static/resources/protocol/
	$(INSTALL_DIR) $(1)/lib/netifd/proto
	$(INSTALL_BIN) ./files/lib/netifd/proto/ecm_fm160.sh $(1)/lib/netifd/proto/
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/usr/sbin/ecm-dhcp.sh $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,luci-proto-ecm-fm160))
