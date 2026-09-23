include $(TOPDIR)/rules.mk

PKG_NAME:=fibocom-dial
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/fibocom-dial
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Fibocom Dial (vendor QMI stack, QMAP aggregation)
  DEPENDS:=+libpthread
endef

define Package/fibocom-dial/description
  Fibocom's own QMI dialer suite (source package from coolsnowwolf/lede):
  fibocom-dial daemon (SET_DATA_FORMAT/BIND_MUX/QMAP flow, dual-stack,
  built-in udhcpc), fibo_qmimsg_server (QMI channel proxy for multi-PDN)
  and multi-pdn-manager (up to 8 concurrent PDNs).
  Pairs with the vendor qmi_wwan_f kernel driver; keep disabled unless the
  module runs in QMI mode (GTUSBMODE 32) with fm160d ECM autodial off.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Package/fibocom-dial/install
	$(INSTALL_DIR) $(1)/usr/bin $(1)/etc/init.d $(1)/etc/config $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fibocom-dial $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fibo_qmimsg_server $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/multi-pdn-manager $(1)/usr/bin
	$(INSTALL_BIN) ./files/etc/init.d/fibocom-dial $(1)/etc/init.d/
	$(INSTALL_CONF) ./files/etc/config/fibocom-dial $(1)/etc/config/
	$(INSTALL_BIN) ./files/usr/sbin/fm160-dial-ctl $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/usr/sbin/qmap-up.sh $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/usr/sbin/qmap-down.sh $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/usr/sbin/mbim-up.sh $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/usr/sbin/mbim-down.sh $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/usr/sbin/mbim-set-ip.sh $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/usr/sbin/fm160-keepalive $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/usr/sbin/fm160-oplog $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/etc/init.d/fm160-dial $(1)/etc/init.d/
endef

define Package/fibocom-dial/postinst
#!/bin/sh
[ -n "$$IPKG_INSTROOT" ] || /etc/init.d/fm160-dial enable
endef

define Package/fibocom-dial/conffiles
/etc/config/fibocom-dial
endef

$(eval $(call BuildPackage,fibocom-dial))
