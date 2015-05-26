AM_CPPFLAGS = -I$(VARNISHSRC)/include -I$(VARNISHSRC) -Wall -Werror

vmoddir = $(VMODDIR)
vmod_LTLIBRARIES = libvmod_redis.la

libvmod_redis_la_LDFLAGS = -module -export-dynamic -avoid-version
libvmod_redis_la_LIBADD = -lhiredis

libvmod_redis_la_SOURCES = \
    vcc_if.c vcc_if.h \
    config.h \
    crc16.c crc16.h \
    sha1.c sha1.h \
    cluster.c cluster.h \
    core.c core.h \
    vmod_redis.c

vcc_if.c vcc_if.h: $(VARNISHSRC)/lib/libvmod_std/vmod.py $(top_srcdir)/src/vmod_redis.vcc
	@PYTHON@ $(VARNISHSRC)/lib/libvmod_std/vmod.py $(top_srcdir)/src/vmod_redis.vcc

VMOD_TESTS = tests/*.vtc
.PHONY: $(VMOD_TESTS)

tests/*.vtc:
	tests/runner.sh $(VARNISHSRC)/bin/varnishtest/varnishtest $@ -Dvarnish_source=$(VARNISHSRC) -Dvarnishd=$(VARNISHSRC)/bin/varnishd/varnishd -Dvmod_topbuild=$(abs_top_builddir)

check: $(VMOD_TESTS)

EXTRA_DIST = \
	vmod_redis.vcc \
	$(VMOD_TESTS)

CLEANFILES = $(builddir)/vcc_if.c $(builddir)/vcc_if.h
