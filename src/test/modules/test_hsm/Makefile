# src/test/modules/test_hsm/Makefile
#
# TAP tests for the HSM block-device storage manager (src/backend/storage/smgr/hsm.c).
# The tests use sparse regular files as "devices", so they need no root
# privileges; HSM accesses a file exactly like a block device (pread/pwrite/fsync).

TAP_TESTS = 1

EXTRA_INSTALL = contrib/amcheck

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_hsm
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
