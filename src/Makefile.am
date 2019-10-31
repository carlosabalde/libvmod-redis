AM_CFLAGS = $(VARNISHAPI_CFLAGS) $(CODE_COVERAGE_CFLAGS) -Wall -DEV_COMPAT3=1
AM_LDFLAGS = $(VARNISHAPI_LIBS) $(VMOD_LDFLAGS) $(CODE_COVERAGE_LDFLAGS)

vmod_LTLIBRARIES = libvmod_redis.la

libvmod_redis_la_SOURCES = \
	crc16.c crc16.h \
	sha1.c sha1.h sha1_config.h \
	cluster.c cluster.h \
	core.c core.h \
	sentinel.c sentinel.h \
	solarisfixes.h \
	vmod_redis.c

nodist_libvmod_redis_la_SOURCES = \
	vcc_redis_if.c \
	vcc_redis_if.h

dist_man_MANS = vmod_redis.3

@BUILD_VMOD_REDIS@

AM_TESTS_ENVIRONMENT = \
	PATH="$(VMOD_TEST_PATH)" \
	LD_LIBRARY_PATH="$(VARNISH_LIBRARY_PATH)"
TEST_EXTENSIONS = .vtc
VTC_LOG_COMPILER = $(abs_srcdir)/tests/runner.sh varnishtest
AM_VTC_LOG_FLAGS = -v -Dvmod_redis="$(VMOD_REDIS)"

TESTS = @VMOD_TESTS@

EXTRA_DIST = \
	tests/hashslot-keys.txt \
	tests/runner.sh \
	vmod_redis.vcc \
	$(VMOD_TESTS)
