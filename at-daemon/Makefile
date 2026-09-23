include $(TOPDIR)/rules.mk

# Vendored from FUjr/QModem (application/ubus_at_daemon).  The upstream copy
# includes ../../version.mk from the QModem feed; we carry a local one so this
# directory builds as a standalone package.
include ./version.mk

PKG_NAME:=ubus-at-daemon
PKG_RELEASE:=$(QMODEM_RELEASE)
PKG_VERSION:=$(QMODEM_VERSION)
PKG_LICENSE:=MPL-2.0
PKG_MAINTAINER:=Tom <fjrcn@outlook.com>

include $(INCLUDE_DIR)/package.mk

define Package/$(PKG_NAME)
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Ubus AT Command Daemon
  DEPENDS:=+libubus +libubox +libblobmsg-json +libjson-c
endef

define Package/$(PKG_NAME)/description
  A ubus daemon for handling AT commands with OpenWRT
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		$(TARGET_CONFIGURE_OPTS) \
		CFLAGS="$(TARGET_CFLAGS) -I$(STAGING_DIR)/usr/include" \
		LDFLAGS="$(TARGET_LDFLAGS) -lubus -lubox -lblobmsg_json -ljson-c"
endef

define Package/$(PKG_NAME)/conffiles
/etc/config/ubus-at-daemon
endef

define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ubus-at-daemon $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/ubus-at-daemon $(1)/etc/init.d/
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/ubus-at-daemon $(1)/etc/config/
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
