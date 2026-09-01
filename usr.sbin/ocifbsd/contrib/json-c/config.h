/*
 * json-c config.h — hand-written for the FreeBSD base build (replaces the
 * cmake/autoconf-generated file). Vendored copy; see COPYING (MIT).
 */
#ifndef JSONC_CONFIG_H
#define JSONC_CONFIG_H

/* Headers present in FreeBSD base */
#define HAVE_ENDIAN_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYSLOG_H 1
#define HAVE_SYS_CDEFS_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_RANDOM_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_DLFCN_H 1

/* Functions present in FreeBSD libc */
#define HAVE_ARC4RANDOM 1
#define HAVE_GETRANDOM 1
#define HAVE_GETRUSAGE 1
#define HAVE_OPEN 1
#define HAVE_REALLOC 1
#define HAVE_SETLOCALE 1
#define HAVE_USELOCALE 1
#define HAVE_SNPRINTF 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOULL 1
#define HAVE_VASPRINTF 1
#define HAVE_VPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_VSYSLOG 1

/* Math decls */
#define HAVE_DECL_INFINITY 1
#define HAVE_DECL_ISINF 1
#define HAVE_DECL_ISNAN 1
#define HAVE_DECL_NAN 1

/* Compiler features (clang/gcc) */
#define HAVE_ATOMIC_BUILTINS 1
#define HAVE___THREAD 1
#define SPEC___THREAD __thread
#define ENABLE_THREADING 1

/* newlocale on FreeBSD does not need an extra freelocale dance */
#define NEWLOCALE_NEEDS_FREELOCALE 0

/* strtoll/strtoull naming */
#define STDC_HEADERS 1

/* Package identity */
#define PACKAGE_NAME "json-c"
#define PACKAGE_VERSION "0.18"
#define PACKAGE_STRING "json-c 0.18"

/* Type sizes (LP64 amd64/arm64) */
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_SIZE_T 8
#define SIZEOF_SSIZE_T 8
#define SIZEOF_INT64_T 8

#endif /* JSONC_CONFIG_H */
