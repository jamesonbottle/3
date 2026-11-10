#pragma once
// order.h - address orders (seq/stride/random/bitrev/gray/butterfly).
#include <string>
#include "types.h"

// ------------------------------------------------------------------ address orders
// An order is a bijection q -> line index over the part it covers; leftover tail lines are visited sequentially.
struct Order {
    enum K { SEQ, STRIDE, RANDOM, BITREV, GRAY, BUTTERFLY } k = SEQ;
    u64 n = 0, m = 0, S = 1, cnt = 1;
    int bits = 0, h = 0;
    u64 fk[4] = {};
    static bool parse(const std::string& s, K& k, bool& rev);
    void init(K kk, u64 nn, u64 stride, u64 seed);
    inline u64 fe(u64 x) const {
        u64 mask = (1ull << h) - 1, L = x >> h, R = x & mask;
        for (int r = 0; r < 4; r++) {
            u64 f = splitmix(R ^ fk[r]) & mask;
            u64 t = L ^ f;
            L = R;
            R = t;
        }
        return (L << h) | R;
    }
    inline u64 at(u64 i) const {
        switch (k) {
        case SEQ:
            return i;
        case STRIDE:
            return (i % cnt) * S + i / cnt;
        case RANDOM: {
            if (!bits)
                return i;
            u64 x = i;
            do
                x = fe(x);
            while (x >= n);
            return x;
        }
        case BITREV:
            return bits ? rev64(i) >> (64 - bits) : 0;
        case GRAY:
            return i ^ (i >> 1);
        case BUTTERFLY:
            return (i & 1) ? n - 1 - (i >> 1) : (i >> 1);
        }
        return i;
    }
    inline u64 pos(u64 q, bool down) const {
        if (down)
            q = n - 1 - q;
        return q < m ? at(q) : q;
    }
};
