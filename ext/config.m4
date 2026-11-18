PHP_ARG_ENABLE([crash-reporter],
  [whether to enable the FrankenPHP crash reporter],
  [AS_HELP_STRING([--enable-crash-reporter], [Enable the FrankenPHP crash reporter])],
  [no])

if test "$PHP_CRASH_REPORTER" != "no"; then
  case $host_os in
    linux*) ;;
    *) AC_MSG_ERROR([crash_reporter supports Linux only]) ;;
  esac
  dnl crash_report.h is shared with the Go module at the root of the repository.
  PHP_NEW_EXTENSION(crash_reporter, crash_reporter.c handler.c, $ext_shared,, [-I@ext_srcdir@/.. -fvisibility=hidden])
fi
