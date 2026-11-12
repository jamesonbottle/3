// algo.cpp - shared algo helpers and the algo registry (algos()).
#include "algo.h"
#include "gen.h"
#include "lineio.h"

namespace ddr5 {

OrderSpec ospec(const Params& P, const char* key, const char* def, const Logger& log) {
    OrderSpec s;
    if (!Order::parse(P.str(key, def), s.k, s.rev)) {
        log.emitf("%skey '%s': unknown address order '%s'\n", P.where().c_str(), key, P.str(key, def).c_str());
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
        o.push_back(std::max<u64>(1, b / kLineBytes));
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
            u8* a = d.base + i * kLineBytes;
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
// A key in suite_data.cpp that no algo reads would be ignored without a word, so run_suite reports it and the unit
// tests fail on it. Each algo's own keys come from its XOpts::keys() (algo.h), which sits next to the XOpts::from()
// that reads them (the unit tests check that every listed key changes the result). The sets below belong to shared
// helpers that take Params: build_vars() for the data keys, stride_list() for `stride`, Run::window_lines() for
// `window_mb`; they must be kept in step with those helpers by hand.
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
        {"probe", keys_of({common, ProbeOpts::keys()})},
        {"pagemix", keys_of({common, PagemixOpts::keys()})},
        {"coherency", keys_of({common, CoherencyOpts::keys()})}};
    auto i = table.find(algo);
    return i == table.end() ? none : i->second;
}

// A key the test set itself must be one its algo understands. A key inherited from kDefaults in suite_data.cpp
// (width, flush) reaches every test, so it only has to be understood by some algo.
std::vector<std::string> unknown_keys(const TestDef& t) {
    std::vector<std::string> bad;
    const std::string algo = t.algo();
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

// ------------------------------------------------------------------ bad values
// Runs the option parsing of the test's algo (and the shared helpers it uses) once, on a copy, with every message
// captured, and returns what it complained about: values that are not numbers / flags, unknown orders, march elements
// that do not parse, unknown styles. Empty = every value of the test is usable. It reads the same keys the algo will
// read (it calls the algo's own XOpts::from), so there is no second list of which key holds a number to keep in step.
std::vector<std::string> bad_values(const TestDef& t) {
    std::vector<std::string> out;
    const std::string algo = t.algo();
    if (!algos().count(algo))
        return out;
    Params P = t.p;
    P.test = t.name;
    P.bad.clear();
    Logger cap;
    cap.sink = [&](const std::string& s) {
        out.push_back(trim(s));
    };
    if (algo == "fill")
        FillOpts::from(P, cap);
    else if (algo == "march")
        MarchOpts::from(P, cap);
    else if (algo == "evolve")
        EvolveOpts::from(P, cap);
    else if (algo == "rwstorm")
        RwstormOpts::from(P, cap);
    else if (algo == "hammer")
        HammerOpts::from(P);
    else if (algo == "copy")
        CopyOpts::from(P);
    else if (algo == "chase")
        ChaseOpts::from(P);
    else if (algo == "probe")
        ProbeOpts::from(P);
    else if (algo == "pagemix")
        PagemixOpts::from(P);
    else if (algo == "coherency")
        CoherencyOpts::from(P);
    const Keys& mine = algo_keys(algo);
    if (mine.count("data"))
        build_vars(P, 0, Rng::Default, cap);
    if (mine.count("stride"))
        stride_list(P, true); // from() reads it only when an order needs it; a bad value is wrong either way
    if (mine.count("window_mb"))
        P.num("window_mb", 0);
    out.insert(out.end(), P.bad.begin(), P.bad.end());
    return out;
}

} // namespace ddr5
