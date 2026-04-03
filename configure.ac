AC_PREREQ([2.69])
AC_INIT([kerchunk], [1.0.2], [brian@mcalester.net])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall dist-xz])
AM_SILENT_RULES([yes])

AC_PROG_CC
AM_PROG_AR

LT_PREREQ([2.4])
LT_INIT([dlopen])

# C11 check
AX_CHECK_COMPILE_FLAG([-std=c11], [CFLAGS="$CFLAGS -std=c11"],
  [AC_MSG_ERROR([C11 support required])])

# Platform
AC_CANONICAL_HOST
case "$host_os" in
  linux*)
    AC_DEFINE([_GNU_SOURCE], [1], [Enable GNU extensions])
    PLATFORM_CFLAGS="-D_GNU_SOURCE"
    EXPORT_DYNAMIC="-Wl,-export-dynamic"
    ;;
  darwin*)
    PLATFORM_CFLAGS="-D_POSIX_C_SOURCE=200809L"
    EXPORT_DYNAMIC="-Wl,-export_dynamic"
    ;;
  *)
    PLATFORM_CFLAGS="-D_POSIX_C_SOURCE=200809L"
    EXPORT_DYNAMIC=""
    ;;
esac
AC_SUBST([PLATFORM_CFLAGS])
AC_SUBST([EXPORT_DYNAMIC])

# Required: pthreads, libm, libdl
AX_PTHREAD([], [AC_MSG_ERROR([pthreads required])])
AC_CHECK_LIB([m], [sin], [], [AC_MSG_ERROR([libm required])])
AC_CHECK_LIB([dl], [dlopen], [], [AC_MSG_ERROR([libdl required])])

# Required: PortAudio
PKG_CHECK_MODULES([PORTAUDIO], [portaudio-2.0])

# Required: libplcode (from system pkg-config)
PKG_CHECK_MODULES([PLCODE], [plcode])

# Required: libcurl
PKG_CHECK_MODULES([CURL], [libcurl])

# Required: OpenSSL (for mod_web/mongoose)
PKG_CHECK_MODULES([OPENSSL], [openssl])

# Required: ALSA (for mixer control)
PKG_CHECK_MODULES([ALSA], [alsa])

# Optional: librtlsdr
PKG_CHECK_MODULES([RTLSDR], [librtlsdr], [have_rtlsdr=yes], [have_rtlsdr=no])
AM_CONDITIONAL([HAVE_RTLSDR], [test "x$have_rtlsdr" = "xyes"])
if test "x$have_rtlsdr" = "xyes"; then
  AC_DEFINE([HAVE_RTLSDR], [1], [Have librtlsdr])
fi

# Optional: libaprs
PKG_CHECK_MODULES([LIBAPRS], [libaprs], [have_libaprs=yes], [have_libaprs=no])
AM_CONDITIONAL([HAVE_LIBAPRS], [test "x$have_libaprs" = "xyes"])
if test "x$have_libaprs" = "xyes"; then
  AC_DEFINE([HAVE_LIBAPRS], [1], [Have libaprs])
fi

# Optional: libpocsag
PKG_CHECK_MODULES([LIBPOCSAG], [libpocsag], [have_libpocsag=yes], [have_libpocsag=no])
AM_CONDITIONAL([HAVE_LIBPOCSAG], [test "x$have_libpocsag" = "xyes"])
if test "x$have_libpocsag" = "xyes"; then
  AC_DEFINE([HAVE_LIBPOCSAG], [1], [Have libpocsag])
fi

# Optional: libflex
PKG_CHECK_MODULES([LIBFLEX], [libflex], [have_libflex=yes], [have_libflex=no])
AM_CONDITIONAL([HAVE_LIBFLEX], [test "x$have_libflex" = "xyes"])
if test "x$have_libflex" = "xyes"; then
  AC_DEFINE([HAVE_LIBFLEX], [1], [Have libflex])
fi

# Optional: libnemo_normalize
PKG_CHECK_MODULES([NEMO], [nemo_normalize], [have_nemo=yes], [have_nemo=no])
AM_CONDITIONAL([HAVE_NEMO], [test "x$have_nemo" = "xyes"])
if test "x$have_nemo" = "xyes"; then
  AC_DEFINE([HAVE_NEMO_NORMALIZE], [1], [Have libnemo_normalize])
fi

# Install paths
kerchunkmoddir='${prefix}/lib/kerchunk'
AC_SUBST([kerchunkmoddir])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

# Summary
AC_MSG_NOTICE([])
AC_MSG_NOTICE([kerchunk $PACKAGE_VERSION configuration:])
AC_MSG_NOTICE([  librtlsdr:        $have_rtlsdr])
AC_MSG_NOTICE([  libaprs:          $have_libaprs])
AC_MSG_NOTICE([  libpocsag:        $have_libpocsag])
AC_MSG_NOTICE([  libflex:          $have_libflex])
AC_MSG_NOTICE([  libnemo_normalize: $have_nemo])
AC_MSG_NOTICE([  module dir:       $kerchunkmoddir])
AC_MSG_NOTICE([])
