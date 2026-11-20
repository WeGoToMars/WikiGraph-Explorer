/*
 * The part of FreeBSD's <machine/param.h> that memcmp.S uses. It reads whole 16-byte blocks past the ends of its
 * buffers only where that cannot cross into the next page. x86-64 pages are 4 KiB, on Linux as on FreeBSD (larger
 * pages are multiples of it).
 */
#ifndef WG_FREEBSD_MACHINE_PARAM_H
#define WG_FREEBSD_MACHINE_PARAM_H

#define PAGE_SIZE 4096

#endif
