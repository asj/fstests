#
# Copyright (c) 2003-2005 Silicon Graphics, Inc.  All Rights Reserved.
#

TOPDIR = ../..
include $(TOPDIR)/include/builddefs
include $(TOPDIR)/include/buildgrouplist

EXT4_DIR = ext4
TARGET_DIR = $(PKG_LIB_DIR)/$(TESTS_DIR)/$(EXT4_DIR)
DIRT = group.list

default: $(DIRT)

include $(BUILDRULES)

install: default
	$(INSTALL) -m 755 -d $(TARGET_DIR)
	$(INSTALL) -m 755 $(TESTS) $(TARGET_DIR)
	$(INSTALL) -m 644 group.list $(TARGET_DIR)
	$(INSTALL) -m 644 $(OUTFILES) $(TARGET_DIR)

# Nothing.
install-dev install-lib:
