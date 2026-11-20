/*
 * The part of FreeBSD's <machine/asm.h> (sys/amd64/include/asm.h) that memchr.S, memcmp.S and memmove.S use: the
 * entry and end of a global function, without FreeBSD's profiling hooks.
 */
#ifndef WG_FREEBSD_MACHINE_ASM_H
#define WG_FREEBSD_MACHINE_ASM_H

#define CNAME(csym) csym

#define _START_ENTRY .text; .p2align 4,0x90

#define _ENTRY(x) _START_ENTRY; \
	.globl CNAME(x); .type CNAME(x),@function; CNAME(x):; \
	.cfi_startproc

#define ENTRY(x) _ENTRY(x)
#define ALTENTRY(x) _ENTRY(x)

#define END(x) .size x, . - x; .cfi_endproc

#endif
