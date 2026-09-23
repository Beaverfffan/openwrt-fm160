# SPDX-License-Identifier: GPL-2.0-or-later
#
# luci-proto-mbim-fm160 - LuCI protocol definition + netifd handler for the
# Fibocom FM160 MBIM data link (vendor CLI implementation, not umbim).
#
# Like luci-app-fm160 this uses package.mk directly instead of the LuCI feed's
# luci.mk so the package stays self-contained when dropped into <tree>/package/.

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-proto-mbim-fm160
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_BUILD_DEPENDS:=luci-base/host

PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/luci-proto-mbim-fm160
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=5. Interfaces
  TITLE:=LuCI support for FM160 MBIM (vendor CLI) protocol
  DEPENDS:=+luci-base +fibocom-dial
endef

define Package/luci-proto-mbim-fm160/description
  netifd protocol handler and LuCI protocol chip for the FM160 MBIM data
  link.  Dialing is done entirely through Fibocom vendor CLI commands
  (mbim-up.sh / mbim-down.sh); the native umbim/libmbim stack is not used.
  Dial parameters (APN, IP stack) are managed by the FM160 dial page
  (luci-app-fm160) via /etc/config/fm160.
endef

define Build/Compile
endef

define Package/luci-proto-mbim-fm160/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/protocol
	$(INSTALL_DATA) ./htdocs/luci-static/resources/protocol/mbim_fm160.js $(1)/www/luci-static/resources/protocol/
	$(INSTALL_DIR) $(1)/lib/netifd/proto
	$(INSTALL_BIN) ./files/lib/netifd/proto/mbim_fm160.sh $(1)/lib/netifd/proto/
endef

$(eval $(call BuildPackage,luci-proto-mbim-fm160))
