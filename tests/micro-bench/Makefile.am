include $(top_srcdir)/build/autotools/Makefile.am.silent

noinst_PROGRAMS = \
	test-text \
	test-picking \
	test-text-perf

INCLUDES = \
	-I$(top_srcdir)/ \
	-I$(top_srcdir)/clutter \
	-I$(top_srcdir)/clutter/cogl \
	-I$(top_builddir)/clutter \
	-I$(top_builddir)/clutter/cogl
LDADD = $(top_builddir)/clutter/libclutter-@CLUTTER_SONAME_INFIX@-@CLUTTER_API_VERSION@.la
AM_CFLAGS = \
	$(CLUTTER_CFLAGS) \
	$(MAINTAINER_CFLAGS) \
	-DG_DISABLE_SINGLE_INCLUDES \
	-DTESTS_DATA_DIR=\""$(top_srcdir)/tests/data/"\"

AM_LDFLAGS = $(CLUTTER_LIBS)

test_text_SOURCES = test-text.c
test_picking_SOURCES = test-picking.c
test_text_perf_SOURCES = test-text-perf.c

