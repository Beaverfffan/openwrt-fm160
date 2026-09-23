#
# Fibocom qmi_wwan_f, vendored into this tree.  See NOTICE.md for provenance.
#
# Upstream ships this file as driver/fibocom_QMI_WWAN/Makefile of the QModem
# feed.  The KernelPackage body below is upstream's, verbatim.  Only the three
# PKG_LICENSE*/PKG_MAINTAINER lines and this header are ours -- they are marked
# LOCAL so a re-vendor can diff them out.
#
# Deliberately NOT changed, because each one would change behaviour:
#
#   * KCONFIG keeps the bare symbols (CONFIG_USB_USBNET, CONFIG_USB_NET_DRIVERS,
#     CONFIG_USB_WDM) as upstream wrote them.  In include/kernel.mk:246 a bare
#     symbol is a *condition*, not an assignment:
#         $(filter m y,$(foreach c,$(filter-out %=y %=n %=m,$(KCONFIG)),$($(c))))
#     i.e. "build this module if these symbols are m or y".  This tree has all
#     three =m, so the condition is met and no kernel config is rewritten --
#     which matters here, because rewriting it costs a full kernel relink.
#   * AUTOLOAD stays at 82.  mainline's kmod-usb-net-qmi-wwan uses
#     $(call AutoProbe,qmi_wwan) -> priority 0 -> the file is installed as plain
#     "qmi_wwan" with no numeric prefix, and "82-qmi_wwan_f" sorts before it.
#     Both drivers claim (0x2cb7, 0x0104, 4); loading first is what lets the
#     Fibocom driver own the interface.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=qmi_wwan_f
PKG_VERSION:=1.0
PKG_RELEASE:=5

# LOCAL -- upstream omits these.  The licence is read off the driver source
# itself (GPL-2.0 header plus MODULE_LICENSE("GPL")); the *repository* LICENSE
# is MPL-2.0 with a non-commercial restriction, and NOTICE.md spells that out.
PKG_LICENSE:=GPL-2.0
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=Tom <fjrcn@outlook.com>

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define KernelPackage/qmi_wwan_f
  SUBMENU:=WWAN Support
  TITLE:=Fibocom Linux USB QMI WWAN Driver
  DEPENDS:=+kmod-usb-net +kmod-usb-wdm
  KCONFIG:= \
	CONFIG_USB_SUPPORT=y \
	CONFIG_USB_USBNET \
	CONFIG_USB_NET_DRIVERS \
	CONFIG_USB_WDM
  FILES:=$(PKG_BUILD_DIR)/qmi_wwan_f.ko
  AUTOLOAD:=$(call AutoLoad,82,qmi_wwan_f)
endef

define KernelPackage/qmi_wwan_f/description
  Fibocom Linux USB QMI WWAN Driver
endef

EXTRA_CFLAGS+= \
	-Wno-unused-function \
	-Wno-missing-prototypes \
	-Wno-missing-declarations

MAKE_OPTS:= \
	ARCH="$(LINUX_KARCH)" \
	CROSS_COMPILE="$(TARGET_CROSS)" \
	CXXFLAGS="$(TARGET_CXXFLAGS)" \
	M="$(PKG_BUILD_DIR)" \
	ccflags-y="$(EXTRA_CFLAGS)" \
	$(EXTRA_KCONFIG)

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C "$(LINUX_DIR)" \
		$(MAKE_OPTS) \
		modules
endef

$(eval $(call KernelPackage,qmi_wwan_f))
