// memcpy, memchr and memcmp on 16-byte vectors, for the static Linux build on arm64 (x86-64 uses FreeBSD's, in
// third_party/freebsd-libc).
//
// musl implements them in portable C, a few bytes per step (memcmp one byte at a time). Building the English graph
// spent 10% of its CPU time in musl's memcpy, against 2% in glibc's. These use GCC's vector extensions, which
// compile to SSE2 on x86-64 and to NEON on arm64: both are part of the base instruction set, so no CPU check is
// needed. Linked into the executable as an object file, they are used instead of the C library's.
//
// CMakeLists.txt compiles this file with -fno-builtin and -fno-tree-loop-distribute-patterns (the compiler could
// otherwise turn the loops below into calls to the functions they implement) and without LTO.

#include <stddef.h>
#include <stdint.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "StringFunctions.c assumes a little-endian CPU"
#endif

typedef unsigned char v16 __attribute__((vector_size(16)));
typedef uint64_t u64x2 __attribute__((vector_size(16)));

// __builtin_memcpy with a constant size compiles to a plain load or store
static inline v16 load16(const unsigned char* p) {
    v16 v;
    __builtin_memcpy(&v, p, 16);
    return v;
}
static inline void store16(unsigned char* p, v16 v) { __builtin_memcpy(p, &v, 16); }
static inline uint64_t load8(const unsigned char* p) {
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    return v;
}
static inline void store8(unsigned char* p, uint64_t v) { __builtin_memcpy(p, &v, 8); }
static inline uint32_t load4(const unsigned char* p) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return v;
}
static inline void store4(unsigned char* p, uint32_t v) { __builtin_memcpy(p, &v, 4); }

// Index of the first byte of a comparison result (each byte 0 or 0xFF) that is set, or 16 if none is
static inline unsigned first_set(v16 eq) {
#if defined(__SSE2__)
    const unsigned m = (unsigned)_mm_movemask_epi8((__m128i)eq);
    return m != 0 ? (unsigned)__builtin_ctz(m) : 16;
#else
    const u64x2 w = (u64x2)eq;
    if (w[0] != 0) return (unsigned)__builtin_ctzll(w[0]) / 8;
    if (w[1] != 0) return 8 + (unsigned)__builtin_ctzll(w[1]) / 8;
    return 16;
#endif
}

void* memcpy(void* restrict dst, const void* restrict src, size_t n) {
    unsigned char* d = dst;
    const unsigned char* s = src;
    // Up to 64 bytes: two or four copies from both ends, which overlap in the middle
    if (n <= 16) {
        if (n >= 8) {
            const uint64_t a = load8(s), b = load8(s + n - 8);
            store8(d, a);
            store8(d + n - 8, b);
        } else if (n >= 4) {
            const uint32_t a = load4(s), b = load4(s + n - 4);
            store4(d, a);
            store4(d + n - 4, b);
        } else if (n > 0) {
            const unsigned char a = s[0], b = s[n / 2], c = s[n - 1];
            d[0] = a;
            d[n / 2] = b;
            d[n - 1] = c;
        }
        return dst;
    }
    if (n <= 32) {
        const v16 a = load16(s), b = load16(s + n - 16);
        store16(d, a);
        store16(d + n - 16, b);
        return dst;
    }
    if (n <= 64) {
        const v16 a = load16(s), b = load16(s + 16), c = load16(s + n - 32), e = load16(s + n - 16);
        store16(d, a);
        store16(d + 16, b);
        store16(d + n - 32, c);
        store16(d + n - 16, e);
        return dst;
    }
    // Longer: the first 16 bytes, then 64 bytes per step to 16-byte aligned destinations, then the last 64 bytes
    const v16 tail0 = load16(s + n - 64), tail1 = load16(s + n - 48), tail2 = load16(s + n - 32),
              tail3 = load16(s + n - 16);
    unsigned char* const tail = d + n - 64;
    store16(d, load16(s));
    const size_t skip = 16 - ((uintptr_t)d & 15);
    d += skip;
    s += skip;
    n -= skip;
    for (; n > 64; n -= 64, d += 64, s += 64) {
        const v16 a = load16(s), b = load16(s + 16), c = load16(s + 32), e = load16(s + 48);
        store16(d, a);
        store16(d + 16, b);
        store16(d + 32, c);
        store16(d + 48, e);
    }
    store16(tail, tail0);
    store16(tail + 16, tail1);
    store16(tail + 32, tail2);
    store16(tail + 48, tail3);
    return dst;
}

void* memchr(const void* src, int c, size_t n) {
    const unsigned char* s = src;
    const unsigned char ch = (unsigned char)c;
    const v16 needle = (v16){0} + ch;
    for (; n >= 16; n -= 16, s += 16) {
        const unsigned i = first_set((v16)(load16(s) == needle));
        if (i < 16) return (void*)(s + i);
    }
    for (; n > 0; --n, ++s)
        if (*s == ch) return (void*)s;
    return NULL;
}

int memcmp(const void* left, const void* right, size_t n) {
    const unsigned char* a = left;
    const unsigned char* b = right;
    for (; n >= 16; n -= 16, a += 16, b += 16) {
        const unsigned i = first_set((v16)(load16(a) != load16(b)));
        if (i < 16) return a[i] - b[i];
    }
    if (n >= 8) {
        const uint64_t x = load8(a) ^ load8(b);
        if (x != 0) {
            const unsigned i = (unsigned)__builtin_ctzll(x) / 8;
            return a[i] - b[i];
        }
        n -= 8;
        a += 8;
        b += 8;
    }
    for (; n > 0; --n, ++a, ++b)
        if (*a != *b) return *a - *b;
    return 0;
}
