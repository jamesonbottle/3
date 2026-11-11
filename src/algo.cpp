// algo.cpp - shared algo helpers and the algo registry (algos()).
#include "algo.h"
#include "lineio.h"

namespace ddr5 {

OrderSpec ospec(const Params& P, const char* key, const char* def, const Logger& log) {
    OrderSpec s;
    if (!Order::parse(P.str(key, def), s.k, s.rev)) {
        log.emitf("bad %s\n", key);
    }
    return s;
}
std::vector<u64> stride_list(const Params& P, bool needed) {
    std::vector<u64> o;
    if (!needed) {
        o.push_back(1);
        return o;
    }
    for (u64 b : P.nums("stride", "4K")) {
        o.push_back(std::max<u64>(1, b / 64));
    }
    return o;
}

// ------------------------------------------------------------------ disturb traffic (manufacturer-style "disturb
// between write and read") Heavy random traffic on a DIFFERENT window of the same thread's chunk while the victim
// window sits written. Row activations, refresh collisions and bus noise happen around data that was just stored; the
// victim is verified afterwards. mode 0 = reads, 1 = writes (scribble), 2 = read+write. Needs window_mb with at least
// two windows per chunk.
static volatile u64 g_sink = 0;
void disturb_traffic(Run& R, const View& d, int mode, u64 passes, u64 seed) {
    Order o;
    o.init(Order::RANDOM, d.n, 1, seed, R.salt);
    for (u64 p = 0; p < passes && !R.stop; p++)
        walk(R, o, false, [&](u64 i) {
            u8* a = d.base + i * 64;
            if (mode != 1) {
                Line t;
                IO<32, 0, 0>::load(a, t);
                _mm_clflush(a);
                g_sink = g_sink ^ t.w[0];
            }
            if (mode >= 1) {
                Line l;
                for (int k = 0; k < 8; k++)
                    l.w[k] = splitmix(seed + i * 8 + k + p);
                IO<32, 0, 1>::store(a, l);
            }
        });
}
int parse_dmode(const std::string& s) {
    return s == "r" || s == "read" ? 0 : s == "w" || s == "write" ? 1 : s == "rw" ? 2 : -1;
}
bool disturb_possible(Run& R, u64 wl, const char* what) {
    if (wl && R.layout.chunks[0].lines / wl >= 2)
        return true;
    R.log.emitf("%s: disturb needs window_mb small enough for >=2 windows per thread chunk; disturb disabled\n", what);
    return false;
}

const std::map<std::string, AlgoFn>& algos() {
    static const std::map<std::string, AlgoFn> table = {
        {"fill", algo_fill},       {"march", algo_march},        {"hammer", algo_hammer}, {"copy", algo_copy},
        {"chase", algo_chase},     {"rwstorm", algo_rwstorm},    {"evolve", algo_evolve}, {"probe", algo_probe},
        {"pagemix", algo_pagemix}, {"coherency", algo_coherency}};
    return table;
}

// ------------------------------------------------------------------ the keys each algo understands
// Everything the algo_*.cpp (and the helpers it calls) reads from Params: build_vars() for the data keys, stride_list()
// for `stride`, Run::window_lines() for `window_mb`, ospec() for `order`/`verify_order`. A key in suite.cfg that is not
// listed here would be ignored without a word, so run_suite reports it and the unit tests fail on it. Keep this table
// in step with the P.* reads until M7b-7j replace it with the typed XOpts::from() functions; an algo that already has
// one (fill: FillOpts) takes its own keys from XOpts::keys(), next to the reads.
using Keys = std::set<std::string>;
static Keys keys_of(std::initializer_list<Keys> parts) {
    Keys all;
    for (const Keys& p : parts)
        all.insert(p.begin(), p.end());
    return all;
}
const Keys& algo_keys(const std::string& algo) {
    static const Keys none;
    static const Keys common = {"algo", "quick", "enabled"};
    static const Keys data = {"data",    "seed",    "seeds",    "sat_width", "style",
                              "victims", "pattern", "altshift", "modulo"};
    static const Keys window = {"window_mb"};
    static const Keys stride = {"stride"};
    static const std::map<std::string, Keys> table = {
        {"fill", keys_of({common, data, window, stride, FillOpts::keys()})},
        {"march", keys_of({common, data, window, stride, MarchOpts::keys()})},
        {"evolve", keys_of({common, data, window, stride, EvolveOpts::keys()})},
        {"rwstorm", keys_of({common, data, window, stride, RwstormOpts::keys()})},
        {"copy", keys_of({common, data, window, CopyOpts::keys()})},
        {"hammer", keys_of({common, data, window, HammerOpts::keys()})},
        {"chase", keys_of({common, window, ChaseOpts::keys()})},
        {"probe", keys_of({common, Keys{"samples", "stride"}})},
        {"pagemix", keys_of({common, Keys{"seconds"}})},
        {"coherency", keys_of({common, Keys{"inc", "lines", "seconds"}})}};
    auto i = table.find(algo);
    return i == table.end() ? none : i->second;
}

// A key the test block set itself must be one its algo understands. A key inherited from [default] (width, flush)
// reaches every test, so it only has to be understood by some algo.
std::vector<std::string> unknown_keys(const TestDef& t) {
    std::vector<std::string> bad;
    std::string algo = lower(t.p.str("algo", "fill"));
    if (!algos().count(algo))
        return bad; // run_suite skips a test with an unknown algo; the unit tests check the algo names of the built-in
                    // suite
    const Keys& mine = algo_keys(algo);
    for (const auto& kv : t.p.m) {
        const std::string& k = kv.first;
        bool ok = mine.count(k) > 0;
        if (!ok && !t.own.count(k))
            for (const auto& a : algos())
                if (algo_keys(a.first).count(k)) {
                    ok = true;
                    break;
                }
        if (!ok)
            bad.push_back(k);
    }
    return bad;
}

} // namespace ddr5
