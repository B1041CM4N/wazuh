# Makefile for Wazuh API
# Copyright (C) 2015-2020, Wazuh Inc.
# May 3, 2017
#
# Syntax: make [ all | backup | install | restore | service ]

OSSEC_GROUP       = ossec
INSTALLDIR       ?= /var/ossec

RM_FILE        = rm -f
INSTALL_DIR    = install -o root -g ${OSSEC_GROUP} -m 0750 -d
INSTALL_RW_DIR = install -o root -g ${OSSEC_GROUP} -m 0770 -d
INSTALL_EXEC   = install -o root -g ${OSSEC_GROUP} -m 0750
INSTALL_FILE   = install -o root -g ${OSSEC_GROUP} -m 0640
INSTALL_CONFIG_FILE   = install -o root -g ${OSSEC_GROUP} -m 0660
PYTHON_BIN     = $(INSTALLDIR)/framework/python/bin/python3


.PHONY: all backup install restore

all: backup install restore

backup:
	# Backup previous configuration
	. ../api/service/inst-api-functions.sh; backup_old_api ${REVISION}

install:
    # Copy files and create folders
	$(INSTALL_DIR) $(INSTALLDIR)/api
	$(INSTALL_RW_DIR) $(INSTALLDIR)/api/configuration
	$(INSTALL_RW_DIR) $(INSTALLDIR)/api/configuration/ssl
	$(INSTALL_RW_DIR) $(INSTALLDIR)/api/configuration/security

	$(INSTALL_CONFIG_FILE) api/configuration/api.yaml ${INSTALLDIR}/api/configuration

	$(INSTALL_DIR) $(INSTALLDIR)/api/scripts
	$(INSTALL_FILE) scripts/wazuh-apid.py ${INSTALLDIR}/api/scripts

    # Install scripts/%.py on $(INSTALLDIR)/bin/%
	$(foreach script,$(wildcard scripts/*),$(INSTALL_EXEC) wrappers/generic_wrapper.sh $(patsubst scripts/%.py,$(INSTALLDIR)/bin/%,$(script));)

restore:
    # Restore old API config
	. ../api/service/inst-api-functions.sh; restore_old_api ${REVISION}
