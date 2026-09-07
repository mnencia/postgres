#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_upgrade_replica
#
# Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
#
# src/bin/pg_upgrade_replica/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_upgrade_replica - rebuild a standby against an upgraded primary"
PGAPPICON = win32

subdir = src/bin/pg_upgrade_replica
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

OBJS = \
	$(WIN32RES) \
	fetch.o \
	forge_manifest.o \
	manifest.o \
	pg_upgrade_replica.o \
	reuse.o \
	subprocess.o

all: pg_upgrade_replica

pg_upgrade_replica: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_upgrade_replica$(X) '$(DESTDIR)$(bindir)/pg_upgrade_replica$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_upgrade_replica$(X)'

clean distclean:
	rm -f pg_upgrade_replica$(X) $(OBJS)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
