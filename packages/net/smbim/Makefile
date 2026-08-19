# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=smbim
PKG_VERSION:=0.1.0
PKG_RELEASE:=4

PKG_MAINTAINER:=pccr10001
PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/smbim
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=WWAN
  TITLE:=Small event-driven MBIM client for netifd
  URL:=https://github.com/pccr10001/smbim
  DEPENDS:=+libmbim +glib2
  CONFLICTS:=smbim-go
endef

define Package/smbim/description
 A focused native C libmbim data connection manager with one-second command
 timeouts, retries, indication monitoring, and netifd integration.
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) $(CURDIR)/src/. $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-I$(STAGING_DIR)/usr/include/glib-2.0 \
		-I$(STAGING_DIR)/usr/lib/glib-2.0/include \
		-I$(STAGING_DIR)/usr/include/libmbim-glib \
		-fPIE -Wall -Wextra -Wformat=2 -Werror=format-security \
		-o $(PKG_BUILD_DIR)/smbimd \
		$(PKG_BUILD_DIR)/main.c $(PKG_BUILD_DIR)/mbim.c \
		$(TARGET_LDFLAGS) -pie -Wl,-z,noexecstack -L$(STAGING_DIR)/usr/lib \
		-lmbim-glib -lgio-2.0 -lgobject-2.0 -lglib-2.0
endef

define Package/smbim/install
	$(INSTALL_DIR) $(1)/usr/bin $(1)/lib/netifd/proto $(1)/usr/libexec
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/smbimd $(1)/usr/bin/smbimd
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/files/lib/netifd/proto/smbim.sh \
		$(1)/lib/netifd/proto/smbim.sh
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/files/usr/libexec/smbim-netifd \
		$(1)/usr/libexec/smbim-netifd
endef

$(eval $(call BuildPackage,smbim))
