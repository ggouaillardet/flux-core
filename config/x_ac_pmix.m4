AC_DEFUN([X_AC_PMIX], [
	AC_ARG_WITH([pmix], AS_HELP_STRING([--with-pmix\[=PATH\]],
          [Build with PMIx bootstrap support]))

	AC_MSG_CHECKING([whether to build PMIx boostrap support])
	AC_ARG_ENABLE([pmix-bootstrap],
	  AS_HELP_STRING([--enable-pmix-bootstrap],
	  [Whether to enable PMIx bootstrap support]),,
	  enable_pmix_bootstrap=no)

	AC_MSG_RESULT($enable_pmix_bootstrap)

        AS_IF([test x$enable_pmix_bootstrap = xyes],
          [AS_IF([test x$with_pmix = no],
	     [AC_MSG_ERROR([--enable-pmix-bootstrap and --without-pmix are incompatible options])])

	   pmix_cppflags=
	   AS_IF([test "x$with_pmix" != "xyes"],
	     [CPPFLAGS_save=$CPPFLAGS
	      CPPFLAGS="-I$with_pmix $CPPFLAGS"])
	   AC_CHECK_HEADERS([pmix.h], [pmix_support=1], [pmix_support=0])
           AS_IF([test "x$with_pmix" != "xyes"],
             [pmix_cppflags="-I$with_pmix"
              CPPFLAGS=$CPPFLAGS_save])
           AS_IF([test $pmix_support -eq 1],
             [AC_DEFINE([HAVE_LIBPMIX], [1], [PMIx bootstrap support])],
             [AC_MSG_ERROR([PMIx support requested but could not be supported])])
          ])
    AC_SUBST(LIBPMIX_CFLAGS, $pmix_cppflags)
])
