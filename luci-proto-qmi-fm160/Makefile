# SPDX-License-Identifier: GPL-2.0-or-later
#
# luci-proto-qmi-fm160 - LuCI protocol definition + netifd handler for the
# Fibocom FM160 QMAP data link (vendor CLI implementation, not uqmi).

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-proto-qmi-fm160
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_BUILD_DEPENDS:=luci-base/host

PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/luci-proto-qmi-fm160
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=5. Interfaces
  TITLE:=LuCI support for FM160 QMAP (vendor CLI) protocol
  DEPENDS:=+luci-base +fibocom-dial
endef

define Package/luci-proto-qmi-fm160/description
  netifd protocol handler and LuCI protocol chip for the FM160 QMAP data
  link (QMI control channel + wwan0.x data channels).  Dialing is done
  entirely through Fibocom vendor CLI commands (qmap-up.sh / qmap-down.sh);
  the native uqmi/libqmi stack is not used.  Dial parameters (APN, IP stack,
  QMAP channel count) are managed by the FM160 dial page (luci-app-fm160)
  via /etc/config/fm160.
endef

define Build/Compile
endef

define Package/luci-proto-qmi-fm160/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/protocol
	$(INSTALL_DATA) ./htdocs/luci-static/resources/protocol/qmi_fm160.js $(1)/www/luci-static/resources/protocol/
	$(INSTALL_DIR) $(1)/lib/netifd/proto
	$(INSTALL_BIN) ./files/lib/netifd/proto/qmi_fm160.sh $(1)/lib/netifd/proto/
endef

$(eval $(call BuildPackage,luci-proto-qmi-fm160))
