#pragma once
// algo.h - what the ten algo_*.cpp files share, plus the registry.
#include <map>
#include <set>
#include <string>
#include <vector>
#include "order.h"
#include "params.h"
#include "run.h"
#include "seed.h"
#include "types.h"

namespace ddr5 {

using AlgoFn = void (*)(Run&, const Params&);

void algo_fill(Run& R, const Params& P);
void algo_march(Run& R, const Params& P);
void algo_hammer(Run& R, const Params& P);
void algo_copy(Run& R, const Params& P);
void algo_chase(Run& R, const Params& P);
void algo_rwstorm(Run& R, const Params& P);
void algo_evolve(Run& R, const Params& P);
void algo_probe(Run& R, const Params& P);
void algo_pagemix(Run& R, const Params& P);
void algo_coherency(Run& R, const Params& P);

const std::map<std::string, AlgoFn>& algos(); // registry: name -> test algorithm; used by the suite and the unit tests
// The test keys (src/suite_data.cpp) an algo understands (empty for an unknown algo name), and the keys of a test that
// its algo would silently ignore: a misspelling, or a key meant for another algo. See algo.cpp.
const std::set<std::string>& algo_keys(const std::string& algo);
std::vector<std::string> unknown_keys(const TestDef& t);
// Problems with the VALUES of a test (not a number, not a flag, unknown order, unparsable march element, ...), worded
// for the user and prefixed with the test name; empty when the test is usable. Called by run_suite before any memory
// is allocated, and by the unit tests for the whole built-in suite.
std::vector<std::string> bad_values(const TestDef& t);

// ------------------------------------------------------------------ shared helpers (algo.cpp)
struct OrderSpec {
    Order::K k = Order::SEQ;
    bool rev = false;
};
OrderSpec ospec(const Params& P, const char* key, const char* def, const Logger& log);
std::vector<u64> stride_list(const Params& P, bool needed);
void disturb_traffic(Run& R, const View& d, int mode, u64 passes, u64 seed);
int parse_dmode(const std::string& s);
bool disturb_possible(Run& R, u64 wl, const char* what);

// ------------------------------------------------------------------ march elements (algo_march.cpp)
struct MOp {
    char t;
    int v;
};
struct MEl {
    char dir;
    bool delay;
    u64 ms;
    bool dist = false;
    int dmode = 0;
    u64 dpasses = 1;
    std::vector<MOp> ops;
};
// "label=elements | label2=elements2": several marches in one test
struct MVar {
    std::string label;
    std::vector<MEl> els;
};
// `where` (Params::where(), e.g. "test 'x': ") prefixes every message
std::vector<MVar> parse_march_variants(const std::string& str, const Logger& log, const std::string& where = "");

// ------------------------------------------------------------------ algo: evolve (data that changes from pass to pass)
// Every other data test writes a pattern once, or rewrites IDENTICAL data (rwstorm), so a write that never lands leaves
// exactly the bytes the test expects: a lost write is invisible to it. Here every line has its own update period
// p(line) in 1..maxp passes and after pass P must hold generation P / p(line); its data is a pure function of (line,
// generation, seed), so nothing is stored on the side. A pass rewrites only the lines whose period divides P, then
// verifies EVERY line:
//   - a rewritten line that still shows its previous generation = a lost / misdirected write (looks perfectly valid in
//   a static test)
//   - a line that was NOT rewritten but changed = a stray write, disturb or leak from the traffic around it
// Memory therefore holds a mix of generations at all times, and old data must survive while its neighbours are being
// rewritten.
// High 64 bits of a * b: maps a 64-bit hash onto 0..b-1 without the 64-bit division a runtime `%` costs.
inline u64 mulhi64(u64 a, u64 b) {
#if defined(_MSC_VER)
    return __umulh(a, b);
#else
    return (u64)(((unsigned __int128)a * b) >> 64);
#endif
}
// The period of a line, uniform in 1..maxp. Multiply-high instead of `% maxp` (AUDIT_PLAN D3): the data it picks
// differs from older versions for the same seed, which does not matter because nothing is stored between runs.
inline u64 evo_period(u64 g, u64 seed, u64 maxp) {
    return 1 + mulhi64(splitmix(g ^ seed), maxp);
}
// The part of a line's data that depends only on its generation; evo_make_k adds the line.
inline u64 evo_key(u64 seed, u64 gen) {
    return splitmix(seed ^ (gen * 0xD6E8FEB86659FD93ull));
}
inline void evo_make_k(u64 g, u64 k, Rng rng, Line& o) { // rng: Run::rng
    for (int j = 0; j < 8; j++)
        o.w[j] = mix64(k + (g << 3) + j, rng); // follows the selected RNG (Advanced > RNG / --rng)
}
inline void evo_make(u64 g, u64 gen, u64 seed, Rng rng, Line& o) {
    evo_make_k(g, evo_key(seed, gen), rng, o);
}
// What a pass needs per line, as a table indexed by the line's period. Pass p rewrites a line of period `per` when
// p % per == 0, and the line then holds generation p / per, i.e. key evo_key(seed, p / per). Both depend only on (p,
// per), and per takes at most maxp values, so they are computed once per pass instead of dividing for every line twice
// (write walk and verify walk). Periods above kMax fall back to the direct arithmetic (a table that large would cost
// more than it saves on a small window).
struct EvoPass {
    static constexpr u64 kMax = 1024;
    u64 seed = 0, maxp = 1, p = 0;
    std::vector<u64> k;  // k[per] = evo_key(seed, p / per)
    std::vector<u8> due; // due[per] = (p % per == 0)
    bool tab = false;
    void init(u64 seed_, u64 maxp_) {
        seed = seed_;
        maxp = maxp_;
        tab = maxp <= kMax;
        if (tab) {
            k.assign(maxp + 1, 0);
            due.assign(maxp + 1, 0);
        }
    }
    void set(u64 pass) {
        p = pass;
        if (!tab)
            return;
        for (u64 per = 1; per <= maxp; per++) {
            k[per] = evo_key(seed, p / per);
            due[per] = p % per == 0;
        }
    }
    bool is_due(u64 per) const { return tab ? due[per] != 0 : p % per == 0; }
    u64 key(u64 per) const { return tab ? k[per] : evo_key(seed, p / per); }
};

// ------------------------------------------------------------------ typed test options
// One struct per algo, parsed ONCE before R.parallel: the algo body reads fields, never Params. from() is the only
// place that reads the algo's own keys; keys() lists them (it feeds algo_keys(), so a misspelled key is reported).
// Defaults here equal the defaults the algo used with P.num/P.flag before the conversion. Messages for a bad value
// (e.g. "bad order") are emitted by from(), in the same order as before.
struct FillOpts {
    OrderSpec write, verify;   // order, verify_order (defaults to order)
    bool strided = false;      // either order is a stride order
    std::vector<u64> strides;  // stride, in lines; {1} when not strided
    bool both = false;         // run every data variant normal and inverted
    bool cross = false;        // verify the window of the next thread
    int width = 32;            // bus width of the accesses in bits
    bool nt = false;           // non-temporal stores
    bool flush = true;         // clflush after access
    u64 reads = 1;             // verify passes per job (>= 1)
    std::vector<u64> dwell_ms; // idle time between write and verify; never empty
    std::vector<int> disturb;  // disturb modes (parse_dmode); {-1} = none
    u64 disturb_passes = 2;    // passes of disturb traffic (>= 1)
    static const std::set<std::string>& keys();
    static FillOpts from(const Params& P, const Logger& log);
};

struct MarchOpts {
    std::vector<MVar> variants; // march; empty = nothing to run, and then NOTHING below is parsed
    std::vector<std::pair<std::string, OrderSpec>> orders; // order words that parsed (never empty when variants is not)
    std::vector<u64> strides;                              // stride, in lines; {1} unless an order is a stride order
    std::vector<u64> bursts;                               // burst: lines per element step; never empty
    bool anyDist = false;                                  // some element is a disturb element X(...)
    int width = 32;
    bool nt = false, flush = true;
    static const std::set<std::string>& keys();
    static MarchOpts from(const Params& P, const Logger& log);
};

struct HammerOpts {
    std::vector<u64> rowstride;  // distance between aggressor rows, bytes (empty = no jobs, as before)
    std::vector<u64> aggressors; // aggressor rows per round (n-sided)
    u64 acts = 400000;           // activations per aggressor per round
    u64 rounds = 24;             // rounds per job
    static const std::set<std::string>& keys();
    static HammerOpts from(const Params& P);
};

struct CopyOpts {
    std::vector<std::string> methods = {"memcpy", "movsb",
                                        "ntcopy"}; // method: copy engines to run (case kept for labels)
    u64 block = 64 * 1024;                         // block: bytes per copy call (>= 64)
    static const std::set<std::string>& keys();
    static CopyOpts from(const Params& P);
};

struct ChaseOpts {
    std::vector<u64> mlp = {12}; // mlp: independent chains per thread (never empty; each >= 1 when used)
    u64 seeds = 2;               // seeds: chain layouts per mlp value (>= 1)
    static const std::set<std::string>& keys();
    static ChaseOpts from(const Params& P);
};

struct RwstormOpts {
    OrderSpec write{Order::RANDOM, false}; // order (the writer's order)
    OrderSpec verify;                      // verify_order (the reader's order, default seq)
    std::vector<u64> strides;              // stride, in lines; {1} unless an order is a stride order
    int width = 32;
    bool nt = false, flush = true;
    u64 passes = 4; // passes of rewrite / concurrent verify (>= 1)
    static const std::set<std::string>& keys();
    static RwstormOpts from(const Params& P, const Logger& log);
};

struct EvolveOpts {
    OrderSpec write{Order::RANDOM, false}; // order (generation writes)
    OrderSpec verify;                      // verify_order (default seq)
    std::vector<u64> strides;              // stride, in lines; {1} unless an order is a stride order
    int width = 32;
    bool nt = false, flush = true;
    bool cross = false; // verify the window of the next thread
    u64 passes = 16;    // passes (>= 1)
    u64 period = 8;     // longest update period of a line, in passes (>= 1)
    static const std::set<std::string>& keys();
    static EvolveOpts from(const Params& P, const Logger& log);
};

struct ProbeOpts {
    std::vector<u64> stride; // stride: address distances to time, bytes (default 64 B .. 16 MB)
    u64 samples = 20000;     // samples per stride (>= 1000)
    static const std::set<std::string>& keys();
    static ProbeOpts from(const Params& P);
};

struct PagemixOpts {
    double seconds = 30; // seconds: length of the copy / invert / refill / check phase
    static const std::set<std::string>& keys();
    static PagemixOpts from(const Params& P);
};

struct CoherencyOpts {
    std::vector<u64> lines = {2, 8, 64}; // lines: shared cache lines per round (each clamped to 1..64 by the algo)
    u64 inc = 1000;                      // inc: increments per thread per round (>= 1)
    double seconds = 8;                  // seconds per `lines` value
    static const std::set<std::string>& keys();
    static CoherencyOpts from(const Params& P);
};

} // namespace ddr5
