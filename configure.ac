AC_PREREQ([2.68])
AC_INIT([libvmod-redis], [trunk], [], [vmod-redis])
AC_COPYRIGHT([Copyright (c) Carlos Abalde <carlos.abalde@gmail.com>])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_SRCDIR(src/vmod_redis.vcc)
AC_CONFIG_HEADER([config.h])

AC_GNU_SOURCE

AM_INIT_AUTOMAKE([1.12 -Wall -Werror foreign parallel-tests])
AM_SILENT_RULES([yes])
AM_PROG_AR

LT_PREREQ([2.2.6])
LT_INIT([dlopen disable-static])

ax_enable_compile_warnings=error
AX_CODE_COVERAGE
AX_COMPILER_FLAGS_CFLAGS
AX_PTHREAD(,[AC_MSG_ERROR([Could not configure pthreads support])])

LIBS="$PTHREAD_LIBS $LIBS -lhiredis -lev"
CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
CC="$PTHREAD_CC"

AC_ARG_WITH([rst2man],
    [
        AS_HELP_STRING(
            [--with-rst2man=PATH],
            [Location of rst2man (auto)])
    ],
    [RST2MAN="$withval"],
    [AC_CHECK_PROGS(RST2MAN, [rst2man rst2man.py], [])])

m4_ifndef([VARNISH_PREREQ], AC_MSG_ERROR([Need varnish.m4 -- see README.rst]))

#VARNISH_PREREQ([5.0], [5.1])
VARNISH_VMODS([redis])

VMOD_TESTS="$(cd $srcdir/src && echo tests/*.vtc)"
AC_SUBST(VMOD_TESTS)

PKG_CHECK_VAR([LIBVARNISHAPI_LIBDIR], [varnishapi], [libdir])
AC_SUBST([VARNISH_LIBRARY_PATH],
	[$LIBVARNISHAPI_LIBDIR:$LIBVARNISHAPI_LIBDIR/varnish])

# Check for libhiredis.
AC_CHECK_LIB(
	hiredis, redisCommand, [],
	[AC_MSG_ERROR([libvmod-redis requires libhiredis.])])

# Check for libev3 (ev_loop) / libev4 (ev_run).
# -DEV_COMPAT3=1 used during build.
AC_CHECK_LIB(
	ev, ev_loop, [],
	[AC_CHECK_LIB(
		ev, ev_run, [],
		[AC_MSG_ERROR([libvmod-redis requires libev.])])])

# Check for TLS support: --enable-tls / --disable-tls
AC_ARG_ENABLE(
    tls,
    [
        AS_HELP_STRING(
            [--enable-tls],
            [enable TLS support (default is YES)])
    ],
    [],
    [enable_tls=yes])
AC_MSG_CHECKING([for TLS support])
if test "x$enable_tls" = xyes; then
    AC_MSG_RESULT([enabled])
    AC_DEFINE(
        [TLS_ENABLED],
        [1],
        [TLS enabled])

	AC_CHECK_LIB(
		crypto, CRYPTO_new_ex_data, [],
		[AC_MSG_ERROR([libvmod-redis requires libcrypto. Try --disable-tls.])])

	AC_CHECK_LIB(
		ssl, SSL_CTX_new, [],
		[AC_MSG_ERROR([libvmod-redis requires libssl. Try --disable-tls.])])

	AC_CHECK_LIB(
		hiredis_ssl, redisInitiateSSL, [],
		[AC_MSG_ERROR([libvmod-redis requires libhiredis_ssl. Try --disable-tls.])])

    AC_CHECK_LIB(
        varnish-sslhelper, VSSLH_status, [], [])
else
    AC_MSG_RESULT([disabled])
fi

AC_CONFIG_FILES([
	Makefile
	src/Makefile
])
AC_OUTPUT
