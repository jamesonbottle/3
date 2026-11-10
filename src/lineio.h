#pragma once
// lineio.h - 64-byte line store/load/verify at selectable width and cache behaviour. Header-only (templates + inline hot path).
#include <type_traits>
#include "types.h"

// ------------------------------------------------------------------ line I/O at selectable width / cache behaviour
// W = access width in bytes (8 scalar, 16 SSE2, 32 AVX2); NT = non-temporal stores; FL = clflush after each access
// so verification really comes from DRAM instead of the cache hierarchy.
template <int W, int NT, int FL> struct IO {
    static inline void store(void* d, const Line& s) {
        CBAR();
        if constexpr (W == 32) {
            __m256i a = _mm256_load_si256((const __m256i*)&s), b = _mm256_load_si256((const __m256i*)&s.w[4]);
            if constexpr (NT) { _mm256_stream_si256((__m256i*)d, a); _mm256_stream_si256((__m256i*)d + 1, b); }
            else { _mm256_store_si256((__m256i*)d, a); _mm256_store_si256((__m256i*)d + 1, b); }
        } else if constexpr (W == 16) {
            for (int i = 0; i < 4; i++) {
                __m128i a = _mm_load_si128((const __m128i*)&s + i);
                if constexpr (NT) _mm_stream_si128((__m128i*)d + i, a); else _mm_store_si128((__m128i*)d + i, a);
            }
        } else {
            for (int i = 0; i < 8; i++) {
                if constexpr (NT) _mm_stream_si64((long long*)d + i, (long long)s.w[i]); else ((volatile u64*)d)[i] = s.w[i];
            }
        }
        if constexpr (FL && !NT) _mm_clflush(d);
        CBAR();
    }
    static inline void load(const void* a, Line& t) {
        if constexpr (W == 32) {
            _mm256_store_si256((__m256i*)&t, _mm256_load_si256((const __m256i*)a));
            _mm256_store_si256((__m256i*)&t.w[4], _mm256_load_si256((const __m256i*)a + 1));
        } else if constexpr (W == 16) {
            for (int i = 0; i < 4; i++) _mm_store_si128((__m128i*)&t + i, _mm_load_si128((const __m128i*)a + i));
        } else { for (int i = 0; i < 8; i++) t.w[i] = ((const volatile u64*)a)[i]; }
    }
    static inline bool verify(const void* a, const Line& e, Line& act) {
        CBAR();
        Line t; load(a, t);
        u64 acc = 0; for (int i = 0; i < 8; i++) acc |= t.w[i] ^ e.w[i];
        if (acc) act = t;
        if constexpr (FL) _mm_clflush(a);
        CBAR();
        return acc == 0;
    }
};
template <int V> using IC = std::integral_constant<int, V>;
template <class F> inline void dispatch(int width, bool nt, bool fl, F&& f) {
    auto d3 = [&](auto w) {
        if (nt) { if (fl) f(w, IC<1>{}, IC<1>{}); else f(w, IC<1>{}, IC<0>{}); }
        else { if (fl) f(w, IC<0>{}, IC<1>{}); else f(w, IC<0>{}, IC<0>{}); }
    };
    if (width == 8) d3(IC<8>{}); else if (width == 16) d3(IC<16>{}); else d3(IC<32>{});
}
