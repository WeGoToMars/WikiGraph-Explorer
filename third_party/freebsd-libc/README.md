# FreeBSD's x86-64 memchr, memcmp and memcpy

Used only by the static Linux build on x86-64 (`WIKIGRAPH_STATIC`, see `CMakeLists.txt`). musl implements these
functions in portable C, a few bytes per step: building the English graph spent 10% of its CPU time in musl's
`memcpy`. Linked into the executable, these take the place of musl's.

## Where they come from

`lib/libc/amd64/string/` in [freebsd-src](https://github.com/freebsd/freebsd-src), tag `release/15.1.0`
(commit `96841ea08dcfa84b954a32dc5ae1a26c28966cf4`), copied unmodified:

| File | SHA-256 |
| --- | --- |
| `memchr.S` | `7d2d504894b21f4aca409322b13afd166a5a82a0cf5f46f89e38438ce1de78aa` |
| `memcmp.S` | `49e1f4c2fa8c41176f6ef5b3ab43ba2c5fcd8b926ef171a6ea58f611edc17955` |
| `memcpy.S` | `e0f7bd59794383a7681e41330681bedd218607c82713c57dc0ee02ed75b9953f` |
| `memmove.S` | `8e54c19a4f44d1d1169cfff0d6f31f86767633db743fc5eae75af2b5d484c013` |

`memcpy.S` assembles `memmove.S` as `memcpy`; `memmove.S` is not built on its own.

To update them, copy the four files from a newer release, update the commit and the checksums above, and run the
static build's tests.

## The headers in `include/`

They replace the FreeBSD headers that the files include, with only what the files use:

- `machine/asm.h`: the entry and end of a function, without FreeBSD's profiling hooks.
- `machine/param.h`: `PAGE_SIZE`, which `memcmp.S` uses to avoid reading across a page boundary.
- `amd64_archlevel.h`: FreeBSD assembles a scalar and an SSE2 version of `memchr` and `memcmp` and picks one when
  the program loads. musl cannot do that (it has no IFUNC support), and every x86-64 CPU has SSE2, so the function's
  name is simply the SSE2 version.

## Licence

The two-clause BSD licence, in the comment at the top of each file. Copyright The FreeBSD Foundation; written by
Robert Clausecker (`memchr`, `memcmp`) and Mateusz Guzik (`memcmp`, `memmove`).
