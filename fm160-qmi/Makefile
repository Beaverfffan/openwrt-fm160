include $(TOPDIR)/rules.mk

PKG_NAME:=fm160-qmi
PKG_VERSION:=1.0.2
PKG_RELEASE:=1

PKG_MAINTAINER:=Beaverfffan
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define Package/fm160-qmi
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=WWAN
  TITLE:=Fibocom FM160 open-source QMI dialer (uqmi based)
  DEPENDS:=+uqmi
  PKGARCH:=all
endef

define Package/fm160-qmi/description
  Open-source QMI dialer for the Fibocom FM160 module, built on OpenWrt's
  uqmi. Replicates the vendor fibocom-dial QMI init sequence
  (SET_DATA_FORMAT/qmap aggregation, BIND_MUX, pinned WDS client) with no
  vendor binaries. IPv4 is delegated to netifd (dhcp on wwan0); IPv6 is
  fetched from QMI runtime settings.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./files $(PKG_BUILD_DIR)/
endef

define Build/Compile
	true
endef

define Package/fm160-qmi/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/files/usr/sbin/fm160-qmi $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/files/usr/sbin/fm160-qmi-at $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/files/etc/init.d/fm160-qmi $(1)/etc/init.d/
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/files/etc/config/fm160-qmi $(1)/etc/config/
endef

define Package/fm160-qmi/conffiles
/etc/config/fm160-qmi
endef

$(eval $(call BuildPackage,fm160-qmi))
