AM_CPPFLAGS = @VMOD_INCLUDES@ -Wall -Wno-typedef-redefinition -Werror -D_GNU_SOURCE=1

vmoddir = @VMOD_DIR@
vmod_LTLIBRARIES = libvmod_redis.la

libvmod_redis_la_LDFLAGS = -module -export-dynamic -avoid-version
libvmod_redis_la_LIBADD = -lhiredis

libvmod_redis_la_SOURCES = \
    config.h \
    crc16.c crc16.h \
    sha1.c sha1.h \
    cluster.c cluster.h \
    core.c core.h \
    sentinel.c sentinel.h \
    vmod_redis.c

nodist_libvmod_redis_la_SOURCES = \
	vcc_if.c \
	vcc_if.h

vmod_redis.lo: vcc_if.c vcc_if.h

vcc_if.c: vcc_if.h

vcc_if.h: @VMODTOOL@ $(top_srcdir)/src/vmod_redis.vcc
	@VMODTOOL@ $(top_srcdir)/src/vmod_redis.vcc

VMOD_TESTS = $(top_srcdir)/src/tests/*.vtc
.PHONY: $(VMOD_TESTS)

$(top_srcdir)/src/tests/*.vtc: libvmod_redis.la
	tests/runner.sh @VARNISHTEST@ $@ -Dvarnishd=@VARNISHD@ -Dvmod_topbuild=$(abs_top_builddir)

check: $(VMOD_TESTS)

EXTRA_DIST = \
	vmod_redis.vcc \
	$(VMOD_TESTS)

CLEANFILES = \
	$(builddir)/vcc_if.c \
	$(builddir)/vcc_if.h \
	$(builddir)/vmod_redis.rst \
	$(builddir)/vmod_redis.man.rst
