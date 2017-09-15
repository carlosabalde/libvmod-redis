AC_PREREQ([2.68])
AC_INIT([libvmod-redis], [6.0], [], [vmod-redis])
AC_COPYRIGHT([Copyright (c) 2014-2017 Carlos Abalde <carlos.abalde@gmail.com>])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_SRCDIR(src/vmod_redis.vcc)
AC_CONFIG_HEADER([config.h])

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
	AS_HELP_STRING(
		[--with-rst2man=PATH],
		[Location of rst2man (auto)]),
	[RST2MAN="$withval"],
	AC_CHECK_PROGS(RST2MAN, [rst2man rst2man.py], []))

m4_ifndef([VARNISH_PREREQ], AC_MSG_ERROR([Need varnish.m4 -- see README.rst]))

VARNISH_PREREQ([5.2], [5.3])
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

AC_CONFIG_FILES([
	Makefile
	src/Makefile
])
AC_OUTPUT
