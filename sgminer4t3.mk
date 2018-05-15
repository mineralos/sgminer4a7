################################################################################
#
# sgminer
#
################################################################################

SGMINER4T3_VERSION = fc33786a4b8699c95db382ab6fd4969a527cd5dd
SGMINER4T3_SITE = $(call github,ckolivas,sgminer4t3,$(SGMINER4T3_VERSION))
SGMINER4T3_DEPENDENCIES = host-pkgconf jansson mcompat libcurl
SGMINER4T3_AUTORECONF = YES
SGMINER4T3_CONF_ENV += LDFLAGS="$(TARGET_LDFLAGS) -lmcompat_drv "
SGMINER4T3_CONF_ENV += LIBS='-lmcompat_drv'
SGMINER4T3_CONF_OPTS = --enable-coinflex --with-system-jansson --without-curses --enable-curl

ifeq ($(BR2_INIT_SYSTEMD),y)
SGMINER4T3_CONF_OPTS += --enable-libsystemd
endif

define SGMINER4T3_INSTALL_INIT_SYSTEMD
	$(INSTALL) -D -m 0644 $(SGMINER4T3_PKGDIR)/sgminer4t3.service \
		$(TARGET_DIR)/usr/lib/systemd/system/cgminer.service
	mkdir -p $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants
	ln -fs ../../../../usr/lib/systemd/system/cgminer.service \
		$(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/cgminer.service
endef

define RENAME_SGMINER4T3_BIN
	mv $(TARGET_DIR)/usr/bin/sgminer $(TARGET_DIR)/usr/bin/cgminer
endef

define INSTALL_SGMINER4T3_CONFIG
	$(INSTALL) -D -m 0644 $(SGMINER4T3_PKGDIR)/cgminer.conf.default \
		$(TARGET_DIR)/etc/cgminer.conf.default
endef

define INSTALL_T3_HWREVISION
	$(INSTALL) -D -m 0644 $(SGMINER4T3_PKGDIR)/hwrevision \
		$(TARGET_DIR)/etc/hwrevision
endef

define SGMINER4T3_POST_RSYNC
        cp $(SRCDIR)/.git/refs/heads/master \
                $(@D)/cgminer_git.hash
endef

SGMINER4T3_POST_RSYNC_HOOKS += SGMINER4T3_POST_RSYNC

define INSTALL_SGMINER4T3_VERSION
        $(INSTALL) -D -m 0644 $(@D)/cgminer_git.hash \
                $(TARGET_DIR)/etc/cgminer_git.hash
endef


SGMINER4T3_POST_INSTALL_TARGET_HOOKS += RENAME_SGMINER4T3_BIN INSTALL_SGMINER4T3_CONFIG INSTALL_T3_HWREVISION INSTALL_SGMINER4T3_VERSION

$(eval $(autotools-package))
