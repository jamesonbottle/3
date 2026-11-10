// algo.cpp - shared algo helpers and the ALGOS table.
#include "algo.h"
#include "lineio.h"

OrderSpec ospec(const Params& P, const char* key, const char* def) {
    OrderSpec s; if (!Order::parse(P.str(key, def), s.k, s.rev)) { emitf("bad %s\n", key); } return s;
}
std::vector<u64> stride_list(const Params& P, bool needed) {
    std::vector<u64> o; if (!needed) { o.push_back(1); return o; }
    for (u64 b : P.nums("stride", "4K")) { o.push_back(std::max<u64>(1, b / 64)); }
    return o;
}

// ------------------------------------------------------------------ disturb traffic (manufacturer-style "disturb between write and read")
// Heavy random traffic on a DIFFERENT window of the same thread's chunk while the victim window sits written. Row activations,
// refresh collisions and bus noise happen around data that was just stored; the victim is verified afterwards.
// mode 0 = reads, 1 = writes (scribble), 2 = read+write. Needs window_mb with at least two windows per chunk.
static volatile u64 g_sink = 0;
void disturb_traffic(Run& R, const View& d, int mode, u64 passes, u64 seed) {
    Order o; o.init(Order::RANDOM, d.n, 1, seed);
    for (u64 p = 0; p < passes && !R.stop; p++)
        walk(R, o, false, [&](u64 i) {
            u8* a = d.base + i * 64;
            if (mode != 1) { Line t; IO<32, 0, 0>::load(a, t); _mm_clflush(a); g_sink = g_sink ^ t.w[0]; }
            if (mode >= 1) { Line l; for (int k = 0; k < 8; k++) l.w[k] = splitmix(seed + i * 8 + k + p); IO<32, 0, 1>::store(a, l); }
        });
}
int parse_dmode(const std::string& s) { return s == "r" || s == "read" ? 0 : s == "w" || s == "write" ? 1 : s == "rw" ? 2 : -1; }
bool disturb_possible(Run& R, u64 wl, const char* what) {
    if (wl && R.chunks[0].lines / wl >= 2) return true;
    emitf("%s: disturb needs window_mb small enough for >=2 windows per thread chunk; disturb disabled\n", what);
    return false;
}

const std::map<std::string, AlgoFn> ALGOS = {{"fill", algo_fill}, {"march", algo_march}, {"hammer", algo_hammer}, {"copy", algo_copy},
                                              {"chase", algo_chase}, {"rwstorm", algo_rwstorm}, {"evolve", algo_evolve}, {"probe", algo_probe},
                                              {"pagemix", algo_pagemix}, {"coherency", algo_coherency}};
