# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-proto-smbim
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_MAINTAINER:=pccr10001
PKG_LICENSE:=MIT
PKGARCH:=all

include $(INCLUDE_DIR)/package.mk

define Package/luci-proto-smbim
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=Protocols
  TITLE:=LuCI support for the smbim protocol
  DEPENDS:=+luci-base +smbim
endef

define Package/luci-proto-smbim/description
 LuCI protocol configuration support for smbim cellular connections.
endef

define Build/Compile
endef

define Package/luci-proto-smbim/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/protocol
	$(INSTALL_DATA) ./htdocs/luci-static/resources/protocol/smbim.js \
		$(1)/www/luci-static/resources/protocol/smbim.js
endef

$(eval $(call BuildPackage,luci-proto-smbim))
