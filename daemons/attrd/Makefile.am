#
# Copyright 2004-2018 Andrew Beekhof <andrew@beekhof.net>
#
# This source code is licensed under the GNU General Public License version 2
# or later (GPLv2+) WITHOUT ANY WARRANTY.
#

include $(top_srcdir)/Makefile.common

halibdir	= $(CRM_DAEMON_DIR)

halib_PROGRAMS	= pacemaker-attrd

noinst_HEADERS  = pacemaker-attrd.h

pacemaker_attrd_CFLAGS	= $(CFLAGS_HARDENED_EXE)
pacemaker_attrd_LDFLAGS	= $(LDFLAGS_HARDENED_EXE)

pacemaker_attrd_LDADD	= $(top_builddir)/lib/cluster/libcrmcluster.la	\
			  $(top_builddir)/lib/pengine/libpe_rules.la	\
			  $(top_builddir)/lib/common/libcrmcommon.la	\
			  $(top_builddir)/lib/cib/libcib.la		\
			  $(top_builddir)/lib/lrmd/liblrmd.la		\
			  $(CLUSTERLIBS)

pacemaker_attrd_SOURCES	= pacemaker-attrd.c attrd_commands.c 	\
			  attrd_utils.c attrd_alerts.c

clean-generic:
	rm -f *.log *.debug *.xml *~

if BUILD_LEGACY_LINKS
install-exec-hook:
	cd $(DESTDIR)$(CRM_DAEMON_DIR) && rm -f attrd && $(LN_S) pacemaker-attrd attrd

uninstall-hook:
	cd $(DESTDIR)$(CRM_DAEMON_DIR) && rm -f attrd
endif
