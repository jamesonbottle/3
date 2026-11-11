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
// The suite.cfg keys an algo understands (empty for an unknown algo name), and the keys of a test that its algo would
// silently ignore: a misspelling, or a key meant for another algo. See algo.cpp.
const std::set<std::string>& algo_keys(const std::string& algo);
std::vector<std::string> unknown_keys(const TestDef& t);

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
std::vector<MVar> parse_march_variants(const std::string& str, const Logger& log);

// ------------------------------------------------------------------ evolving data (algo_evolve.cpp)
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
inline u64 evo_period(u64 g, u64 seed, u64 maxp) {
    return 1 + splitmix(g ^ seed) % maxp;
}
inline void evo_make(u64 g, u64 gen, u64 seed, int rng, Line& o) { // rng: Run::rng
    u64 k = splitmix(seed ^ (gen * 0xD6E8FEB86659FD93ull));
    for (int j = 0; j < 8; j++)
        o.w[j] = mix64(k + (g << 3) + j, rng); // follows the selected RNG (Advanced > RNG / --rng)
}

// ------------------------------------------------------------------ typed test options (M7)
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

} // namespace ddr5
