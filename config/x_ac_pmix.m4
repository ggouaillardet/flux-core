AC_DEFUN([X_AC_PMIX], [

    AC_ARG_WITH([pmix], AS_HELP_STRING([--with-pmix\[=PATH\]],
                 [Build with PMIx bootstrap support]))

    pmix_support=0
    pmix_cppflags=
    AS_IF([test "x$with_pmix" != "xno"],
          [AS_IF([test "x$with_pmix" != "xyes"],
                 [CPPFLAGS_save=$CPPFLAGS
                  CPPFLAGS="-I$with_pmix $CPPFLAGS"])
           AC_CHECK_HEADERS([pmix.h], [pmix_support=1])
           AS_IF([test "x$with_pmix" != "xyes"],
                 [pmix_cppflags="-I$with_pmix"
                  CPPFLAGS=$CPPFLAGS_save])
           AS_IF([test $pmix_support -eq 0 && test "x$with_pmix" != "x"],
                 [AC_MSG_ERROR([PMIx support requested but could not be supported])])
           AS_IF([test $pmix_support -eq 1],
                 [AC_DEFINE([HAVE_PMIX], [1], [PMIx bootstrap support])])
          ])
    AC_SUBST(pmix_cppflags, $pmix_cppflags)
    AM_CONDITIONAL([HAVE_PMIX], [test $pmix_support -eq 1])
])
