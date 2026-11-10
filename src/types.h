#pragma once
// types.h - tiny base included by everything: fixed-width names, the 64-byte Line, bit helpers, compiler barrier.
#include <cstdint>
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#define CBAR() _ReadWriteBarrier()
#else
#define CBAR() asm volatile("" ::: "memory")
#endif

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

struct alignas(64) Line {
    u64 w[8];
};
static_assert(sizeof(Line) == 64, "Line must be one 64B burst (BL16 x 32-bit DDR5 subchannel)");

inline u64 splitmix(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
inline int pc32(u32 v) {
#if defined(_MSC_VER)
    return (int)__popcnt(v);
#else
    return __builtin_popcount(v);
#endif
}
inline int clz64(u64 v) {
#if defined(_MSC_VER)
    unsigned long i;
    _BitScanReverse64(&i, v);
    return 63 - (int)i;
#else
    return __builtin_clzll(v);
#endif
}
inline u64 rev64(u64 x) {
    x = ((x >> 1) & 0x5555555555555555ull) | ((x & 0x5555555555555555ull) << 1);
    x = ((x >> 2) & 0x3333333333333333ull) | ((x & 0x3333333333333333ull) << 2);
    x = ((x >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((x & 0x0F0F0F0F0F0F0F0Full) << 4);
    x = ((x >> 8) & 0x00FF00FF00FF00FFull) | ((x & 0x00FF00FF00FF00FFull) << 8);
    x = ((x >> 16) & 0x0000FFFF0000FFFFull) | ((x & 0x0000FFFF0000FFFFull) << 16);
    return (x >> 32) | (x << 32);
}
