# SPDX-License-Identifier: GPL-2.0-or-later
#
# luci-app-fm160 - LuCI (JavaScript) front end for fm160d.
#
# This package deliberately includes $(INCLUDE_DIR)/package.mk instead of the
# LuCI feed's luci.mk: luci.mk is reached with a relative include
# ("include ../../luci.mk") which only resolves for packages sitting two levels
# inside the luci feed.  A third-party package dropped into <tree>/package/
# would resolve that to <tree>/luci.mk and fail.  package.mk keeps the package
# self-contained.
#
# The one thing given up by that choice is luci.mk's automatic .po -> .lmo
# translation packaging, so the block at the bottom of this file re-implements
# it.  It is a re-implementation, not a copy, and the five things it has to get
# right are the five things luci.mk hardcodes -- all of them silent when wrong:
#
#   * the catalog directory is /usr/lib/lua/luci/i18n (LUCI_LIBRARYDIR, i.e.
#     /usr/lib/lua/luci + /i18n).  Not /usr/lib/luci/i18n.  dispatcher.uc
#     passes this path to load_catalog() and the controller hardcodes it in
#     admin/index.uc, so a file installed anywhere else is never found.
#   * the file name is <basename>.<language>.lmo, where <basename> is the
#     package name with the "luci-app-" prefix stripped (fm160), and
#     <language> is the *alias*, not the po directory name: po/zh_Hans is
#     installed as fm160.zh-cn.lmo.  load_catalog() globs "*.zh-cn.lmo", so a
#     wrong alias means no match and no error.
#   * the translation ships as its own hidden package, not as part of the app.
#   * that package carries DEFAULT:=LUCI_LANG_<po directory>||(ALL&&m).  A
#     HIDDEN package with no DEFAULT is generated as a bare "tristate" whose
#     only default is "y if DEFAULT_<itself>", a symbol nothing defines, so
#     nothing can ever select it: it compiles for no image and the failure
#     looks exactly like a translation that was never written.  This is the one
#     of the five that has no symptom on the device at all, beyond English.
#   * that package's uci-defaults must create the luci.languages entry, because
#     dispatcher.uc decides an 'auto' language request by looking for the
#     *existence* of uci.languages.<lang>.

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-fm160
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=Beaverfffan
PKGARCH:=all

# po2lmo is built by luci-base's HostBuild and lands in $(STAGING_DIR_HOSTPKG)/bin.
PKG_BUILD_DEPENDS:=luci-base/host

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-fm160
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI interface for the Fibocom FM160
  DEPENDS:=+luci-base +fm160d +fibocom-dial
  PKGARCH:=all
endef

define Package/luci-app-fm160/description
 LuCI JavaScript interface for a single Fibocom FM160 module: module status,
 signal and cell information, band/cell locking with the carrier-aggregation
 view, and an AT console.  Dialing, SMS and GNSS pages are added by the
 following milestones.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

# htdocs/ is plain LuCI JavaScript and root/ holds menu.d + acl.d, so there is
# nothing to compile -- the package is arch-independent.
define Package/luci-app-fm160/install
	$(CP) ./root/* $(1)/
	$(INSTALL_DIR) $(1)/www
	$(CP) ./htdocs/* $(1)/www/
	# $(CP) preserves checkout permissions, which git does not guarantee for
	# the executable bits below - reinstall them explicitly so the scripts
	# actually run on the device.
	$(INSTALL_BIN) ./root/usr/sbin/fm160-mwan3-sync $(1)/usr/sbin/fm160-mwan3-sync
	$(INSTALL_BIN) ./root/etc/hotplug.d/iface/32-fm160-mwan3 $(1)/etc/hotplug.d/iface/32-fm160-mwan3
	$(INSTALL_BIN) ./root/etc/uci-defaults/99-fm160-mwan3 $(1)/etc/uci-defaults/99-fm160-mwan3
endef

$(eval $(call BuildPackage,luci-app-fm160))

# ---------------------------------------------------------------------------
# Translations
# ---------------------------------------------------------------------------
# Where LuCI actually looks.  dispatcher.uc:110 and controller/admin/index.uc:126
# both pass '/usr/lib/lua/luci/i18n' to load_catalog().
FM160_LUCI_LIBDIR:=/usr/lib/lua/luci

# One entry per shipped translation: the .lmo suffix, which is the string LuCI
# calls the language's alias and the only thing load_catalog() ever sees.  Two
# lookups hang off it -- the po/ directory it is compiled from, and the label
# luci.languages records -- so adding a language is one entry here plus one
# FM160_PO.<alias>, one FM160_LANG_TITLE.<alias> and the po/ directory itself.
#
# The alias is the key, rather than the po directory name, because of how the
# $(foreach) at the bottom hands it over.  $(call) does not trim its arguments
# and a backslash-newline inside a call expands to a space, so a continued call
# passes $(1) the string " zh-cn" -- and that space then travels into the
# package name (an invalid Kconfig symbol, which kills `make defconfig` on the
# whole tree), the installed .lmo file name (fm160. zh-cn.lmo, which
# load_catalog() never matches against its "*.zh-cn.lmo" glob) and the uci
# default (luci.languages. zh_cn, which is not valid uci syntax).  Nothing about
# any of that looks wrong in the Makefile, so the call is kept on one line and
# given exactly one argument to pass.
#
# zh_Hans is the only translation that ships.  An earlier revision of this list
# also carried Traditional Chinese, which was an intention and never a
# deliverable: there is no po/zh_Hant, so it asked the build for a translation
# package with nothing in it.  The two checks further down turn the mismatch
# into a hard error instead of an empty package, which is why it could not
# survive here -- but an error that only appears once make reaches this
# directory is a poor way to find out, so tools/i18n/check.py asserts the same
# invariant from the repository alone, before anything is built.
FM160_TRANSLATIONS:=zh-cn

# The po/ directory each alias is compiled from, in OpenWrt's spelling of the
# locale.  Kept as a separate lookup so that the entry above stays a single
# word, with nothing in it for a call to split on.
FM160_PO.zh-cn:=zh_Hans

# The label recorded in luci.languages.  Only the *existence* of that option is
# ever read (dispatcher.uc uses it to resolve 'auto' Accept-Language), never the
# value, but keep it the string the base translation uses so anything that ever
# does display it agrees.
FM160_LANG_TITLE.zh-cn:=简体中文 (Simplified Chinese)

# Both lookups are required, and an empty one is silent on its own: no FM160_PO
# compiles nothing and no FM160_LANG_TITLE puts a blank name in the language
# picker.  One line each for the same reason the call above is one line.
$(foreach a,$(FM160_TRANSLATIONS),$(if $(FM160_PO.$(a)),,$(error FM160_PO.$(a) is empty)))
$(foreach a,$(FM160_TRANSLATIONS),$(if $(FM160_LANG_TITLE.$(a)),,$(error FM160_LANG_TITLE.$(a) is empty)))

FM160_DECLARED_PO:=$(sort $(foreach a,$(FM160_TRANSLATIONS),$(FM160_PO.$(a))))
FM160_PO_LANGS:=$(sort $(filter-out templates,$(notdir $(wildcard ./po/*))))

# Both directions, because each one fails silently on its own: a declared alias
# with no po/ builds an empty package, and a po/ directory that nothing declares
# sits in the tree looking maintained while never reaching a device.
$(if $(filter-out $(FM160_PO_LANGS),$(FM160_DECLARED_PO)),\
  $(error luci-app-fm160: FM160_PO names $(filter-out $(FM160_PO_LANGS),$(FM160_DECLARED_PO)), which is not a po/<locale>))
$(if $(filter-out $(FM160_DECLARED_PO),$(FM160_PO_LANGS)),\
  $(error luci-app-fm160: po/$(filter-out $(FM160_DECLARED_PO),$(FM160_PO_LANGS)) exists but no FM160_TRANSLATIONS entry names it, so it would never be packaged))

# $(1) = the alias: the lmo suffix and the package name suffix in one
define BuildFm160Translation
  define Package/luci-i18n-fm160-$(1)
    SECTION:=luci
    CATEGORY:=LuCI
    SUBMENU:=3. Applications
    TITLE:=luci-app-fm160 - $(1) translation
    HIDDEN:=1
    DEFAULT:=LUCI_LANG_$(FM160_PO.$(1))||(ALL&&m)
    DEPENDS:=luci-app-fm160
    PKGARCH:=all
  endef

  define Package/luci-i18n-fm160-$(1)/description
   $(FM160_LANG_TITLE.$(1)) translation for the FM160 LuCI application.
  endef

  define Package/luci-i18n-fm160-$(1)/install
	$$(INSTALL_DIR) $$(1)/etc/uci-defaults
	echo "uci set luci.languages.$(subst -,_,$(1))='$(FM160_LANG_TITLE.$(1))'; uci commit luci" \
		> $$(1)/etc/uci-defaults/luci-i18n-fm160-$(1)
	$$(INSTALL_DIR) $$(1)$(FM160_LUCI_LIBDIR)/i18n
	$(foreach po,$(wildcard ./po/$(FM160_PO.$(1))/*.po), \
		po2lmo $(po) \
			$$(1)$(FM160_LUCI_LIBDIR)/i18n/$(basename $(notdir $(po))).$(1).lmo;)
  endef

  # luci.mk ships this too, and it is the difference between "installed" and
  # "in effect": /etc/uci-defaults is only swept at first boot, so an image gets
  # the luci.languages entry for free, but the same package installed later with
  # opkg would sit on an unrun script until the next reboot.  IPKG_INSTROOT is
  # set while an image is being assembled, and empty on a live system.
  define Package/luci-i18n-fm160-$(1)/postinst
	[ -n "$$$${IPKG_INSTROOT}" ] || {
		[ -f /etc/uci-defaults/luci-i18n-fm160-$(1) ] && (. /etc/uci-defaults/luci-i18n-fm160-$(1)) && rm -f /etc/uci-defaults/luci-i18n-fm160-$(1)
		exit 0
	}
  endef

  $$(eval $$(call BuildPackage,luci-i18n-fm160-$(1)))
endef

$(foreach a,$(FM160_TRANSLATIONS),$(eval $(call BuildFm160Translation,$(a))))
