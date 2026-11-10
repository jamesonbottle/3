// order.cpp - Order::parse and Order::init.
#include "order.h"
#include <algorithm>
#include "params.h"
#include "seed.h"

bool Order::parse(const std::string& s, K& k, bool& rev) {
    std::string v = lower(s);
    rev = false;
    if (v == "seq" || v == "up")
        k = SEQ;
    else if (v == "rev" || v == "down") {
        k = SEQ;
        rev = true;
    } else if (v == "stride")
        k = STRIDE;
    else if (v == "random")
        k = RANDOM;
    else if (v == "bitrev")
        k = BITREV;
    else if (v == "gray")
        k = GRAY;
    else if (v == "butterfly")
        k = BUTTERFLY;
    else
        return false;
    return true;
}
void Order::init(K kk, u64 nn, u64 stride, u64 seed) {
    k = kk;
    n = nn;
    m = nn;
    S = std::max<u64>(1, stride);
    switch (k) {
    case STRIDE:
        S = std::min(S, n);
        cnt = n / S;
        m = cnt * S;
        break;
    case RANDOM:
        bits = n > 1 ? 64 - clz64(n - 1) : 0;
        if (bits & 1)
            bits++;
        h = bits / 2;
        for (int i = 0; i < 4; i++)
            fk[i] = splitmix(seed + g_salt * 7919 + 0x1234 * (i + 1));
        break;
    case BITREV:
    case GRAY:
        bits = n > 1 ? 63 - clz64(n) : 0;
        m = 1ull << bits;
        break;
    default:
        break;
    }
}
