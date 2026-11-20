/*
 * Replaces FreeBSD's lib/libc/amd64/amd64_archlevel.h.
 *
 * FreeBSD assembles a scalar and an SSE2 ("baseline") version of memchr and memcmp and picks one when the program
 * loads, through an indirect function (IFUNC). musl does not support IFUNCs, and every x86-64 CPU has SSE2, so here
 * the function's name is simply an alias of its baseline version. The scalar version is still assembled, as a local
 * symbol that nothing calls.
 */
#ifndef WG_FREEBSD_AMD64_ARCHLEVEL_H
#define WG_FREEBSD_AMD64_ARCHLEVEL_H

#include <machine/asm.h>

#define ARCHFUNCS(func) \
	.globl CNAME(func); \
	.type CNAME(func), @function; \
	.set CNAME(func), func##_baseline

#define ARCHFUNC(func, level)
#define NOARCHFUNC
#define ENDARCHFUNCS(func)

#define ARCHENTRY(func, level) \
	_START_ENTRY; \
	.type func##_##level, @function; \
	func##_##level:; \
	.cfi_startproc

#define ARCHEND(func, level) END(func##_##level)

#endif
