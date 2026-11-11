// Deterministic unit tests for the engine's pure logic (no memory is stressed). Build and run: tools/selftest.sh
//   cmake -S . -B build/cmake && cmake --build build/cmake --target unit_tests && build/cmake/unit_tests
#include <cmath>
#include <set>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include "algo.h"
#include "gen.h"
#include "log.h"
#include "order.h"
#include "params.h"
#include "run.h"
#include "seed.h"
#include "suite.h"
#include "types.h"

using namespace ddr5;

static int g_fail = 0;
static Logger test_log; // sink set in main(): parser diagnostics are printed, not written to a real log
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ++g_fail;                                                                                                  \
            printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);                                                     \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

// Every address order must visit each of the n lines exactly once, ascending and descending.
static void test_orders() {
    const u64 ns[] = {1, 2, 3, 4, 5, 7, 64, 100, 1000, 4096, 5000, 32768, 32768 + 17, 65536 + 4095};
    const struct {
        Order::K k;
        const char* name;
    } kinds[] = {{Order::SEQ, "seq"},       {Order::STRIDE, "stride"}, {Order::RANDOM, "random"},
                 {Order::BITREV, "bitrev"}, {Order::GRAY, "gray"},     {Order::BUTTERFLY, "butterfly"}};
    for (auto& kd : kinds)
        for (u64 n : ns)
            for (u64 stride : {1ull, 4ull, 64ull, 1000ull})
                for (int down = 0; down < 2; down++) {
                    if (kd.k != Order::STRIDE && stride != 1)
                        continue;
                    Order o;
                    o.init(kd.k, n, stride, 12345, 0);
                    std::vector<u8> seen(n, 0);
                    u64 dup = 0, oob = 0;
                    for (u64 q = 0; q < n; q++) {
                        u64 p = o.pos(q, down != 0);
                        if (p >= n) {
                            oob++;
                            continue;
                        }
                        if (seen[p]++)
                            dup++;
                    }
                    CHECK(!dup && !oob, "order=%s n=%llu stride=%llu down=%d dup=%llu out-of-range=%llu", kd.name,
                          (unsigned long long)n, (unsigned long long)stride, down, (unsigned long long)dup,
                          (unsigned long long)oob);
                }
}

// Window rotation: the windows of one chunk together cover every line (no never-tested tail); the disturb window stays
// in bounds and never overlaps.
static void test_windows() {
    u8* fake = (u8*)0x100000;
    for (u64 lines :
         {4096ull * 2, 4096ull * 3, 4096ull * 5 + 123, 32768ull * 7 + 4096, 1000000ull, 8388608ull + 4096 * 3 + 1}) {
        for (u64 wl : {4096ull, 8192ull, 65536ull, 524288ull, 3000000ull}) {
            if (wl >= lines)
                continue;
            Run R(test_log);
            R.layout.T = 1;
            R.layout.chunks.push_back({fake, lines, 0});
            u64 nw = (lines + wl - 1) / wl;
            std::vector<u8> cov(lines, 0);
            for (u64 ctr = 0; ctr < nw; ctr++) {
                View v = R.view(0, wl, ctr);
                u64 s = (u64)(v.base - fake) / 64;
                CHECK(v.n == wl && s + wl <= lines && v.g == s, "view lines=%llu wl=%llu ctr=%llu start=%llu",
                      (unsigned long long)lines, (unsigned long long)wl, (unsigned long long)ctr,
                      (unsigned long long)s);
                for (u64 i = s; i < s + wl && i < lines; i++)
                    cov[i] = 1;
            }
            u64 missed = 0;
            for (u64 i = 0; i < lines; i++)
                missed += !cov[i];
            CHECK(missed == 0, "lines=%llu wl=%llu: %llu lines never covered by any window", (unsigned long long)lines,
                  (unsigned long long)wl, (unsigned long long)missed);
            if (lines / wl >= 2)
                for (u64 ctr = 0; ctr < nw * 2;
                     ctr++) { // disturb is only used when >= 2 whole windows fit (disturb_possible)
                    View v = R.view(0, wl, ctr), d = R.dview(0, wl, ctr);
                    u64 sv = (u64)(v.base - fake) / 64, sd = (u64)(d.base - fake) / 64;
                    bool inb = sd + wl <= lines, disjoint = sd + wl <= sv || sv + wl <= sd;
                    CHECK(inb && disjoint && d.g == sd, "dview lines=%llu wl=%llu ctr=%llu victim=%llu disturb=%llu",
                          (unsigned long long)lines, (unsigned long long)wl, (unsigned long long)ctr,
                          (unsigned long long)sv, (unsigned long long)sd);
                }
        }
    }
}

// The 'sat' pattern table: every selectable (pattern, width) pair yields a non-empty, finite pattern and inverse data
// is the exact complement.
static void test_generators() {
    for (auto& sp : sat_table())
        CHECK(sp.len >= 1 && sp.len <= 64, "pattern %s len=%d", sp.name, sp.len);
    Gen g;
    g.kind = 1;
    g.seed = 99;
    Line a{}, b{}, c{};
    g.make(7, nullptr, false, a);
    g.make(7, nullptr, false, b);
    g.make(7, nullptr, true, c);
    bool same = !memcmp(&a, &b, 64), comp = true;
    for (int j = 0; j < 8; j++)
        comp &= (c.w[j] == ~a.w[j]);
    CHECK(same, "hash data must be a pure function of the line index");
    CHECK(comp, "inverted data must be the exact complement");
    Gen h;
    h.kind = 1;
    h.seed = 99;
    Line d{};
    h.make(8, nullptr, false, d);
    CHECK(memcmp(&a, &d, 64) != 0, "neighbouring lines must get different hash data");
}

// Every selectable hash mixer must avalanche: flipping one input bit flips each output bit with probability 1/2.
// xorshift64* used to fail this badly (a bare multiply as its finalizer: 308 of the 4096 input-bit -> output-bit pairs
// never flipped, 1450 were biased), so seeds and high address bits barely influenced the low bits of 'hash' data. 8192
// samples put the noise at ~0.006, so 0.06 cannot fail by chance.
static void test_mixers() {
    const char* names[] = {"splitmix64", "murmur3 fmix64", "xorshift64*"};
    const int N = 8192;
    static u32 cnt[64][64];
    for (int mode = 0; mode < 3; mode++) {
        memset(cnt, 0, sizeof cnt);
        u64 rs = 0x1234;
        for (int n = 0; n < N; n++) {
            rs = splitmix(rs);
            u64 h0 = mix64(rs, mode);
            for (int i = 0; i < 64; i++) {
                u64 d = h0 ^ mix64(rs ^ (1ull << i), mode);
                for (int j = 0; j < 64; j++)
                    cnt[i][j] += (d >> j) & 1;
            }
        }
        int bad = 0, dead = 0;
        double worst = 0;
        for (int i = 0; i < 64; i++)
            for (int j = 0; j < 64; j++) {
                double dev = std::fabs((double)cnt[i][j] / N - 0.5);
                worst = std::max(worst, dev);
                bad += dev > 0.06;
                dead += cnt[i][j] == 0;
            }
        CHECK(!bad && !dead,
              "%s: %d of 4096 input->output bit pairs biased by more than 0.06 (worst %.3f), %d never flip",
              names[mode], bad, worst, dead);
    }
}

// One run seed must determine every random choice (hash seeds, random permutations), and different seeds / loops / runs
// must give different ones.
static void test_seed() {
    CHECK(salt_for(5, 1) == salt_for(5, 1), "salt must be a pure function of (seed, loop)");
    CHECK(salt_for(5, 1) != salt_for(5, 2) && salt_for(5, 1) != salt_for(6, 1) && salt_for(0, 1) != salt_for(0, 2),
          "salt must change with the seed and with the loop");
    CHECK(fresh_seed() != fresh_seed(), "two runs without --seed must not get the same seed");
    Params p;
    p.m["data"] = "hash";
    p.m["seeds"] = "3";
    struct Sig {
        std::vector<u64> hash, perm;
    };
    auto sig = [&](u64 seed, u64 loop) {
        u64 salt = salt_for(seed, loop);
        Sig s;
        for (auto& v : build_vars(p, salt, 0, test_log))
            s.hash.push_back(v.g.seed);
        Order o;
        o.init(Order::RANDOM, 5000, 1, 1, salt);
        for (u64 i = 0; i < 64; i++)
            s.perm.push_back(o.at(i));
        return s;
    };
    Sig a = sig(42, 1), b = sig(42, 1), c = sig(43, 1), d = sig(42, 2);
    CHECK(a.hash == b.hash && a.perm == b.perm,
          "same seed and loop must reproduce the hash seeds and the random permutation");
    CHECK(a.hash != c.hash && a.perm != c.perm,
          "a different seed must change the hash seeds and the random permutation");
    CHECK(a.hash != d.hash && a.perm != d.perm,
          "a different loop must change the hash seeds and the random permutation");
    CHECK(std::set<u64>(a.hash.begin(), a.hash.end()).size() == a.hash.size(),
          "the seeds of one run must differ from each other");
}

// Evolving data: generation rules and the property that makes a lost write visible (the old generation never equals the
// new one).
static void test_evolve() {
    const u64 seed = 0xABCDEF, maxp = 8, passes = 16, lines = 20000;
    u64 hist[9] = {};
    for (u64 g = 0; g < lines; g++) {
        u64 per = evo_period(g, seed, maxp);
        CHECK(per >= 1 && per <= maxp, "line %llu: period %llu out of range", (unsigned long long)g,
              (unsigned long long)per);
        hist[per <= maxp ? per : 0]++;
        u64 prev = 0, rewrites = 0;
        for (u64 p = 1; p <= passes; p++) {
            u64 gen = p / per;
            bool updated = p % per == 0;
            CHECK(gen >= prev && gen - prev <= 1 && updated == (gen != prev),
                  "line %llu pass %llu: generation %llu -> %llu, rewritten=%d", (unsigned long long)g,
                  (unsigned long long)p, (unsigned long long)prev, (unsigned long long)gen, (int)updated);
            rewrites += updated;
            prev = gen;
        }
        CHECK(rewrites == passes / per && rewrites >= 1, "line %llu (period %llu) rewritten %llu times in %llu passes",
              (unsigned long long)g, (unsigned long long)per, (unsigned long long)rewrites, (unsigned long long)passes);
    }
    for (u64 per = 1; per <= maxp; per++)
        CHECK(hist[per] > lines / maxp * 3 / 4 && hist[per] < lines / maxp * 5 / 4,
              "period %llu chosen by %llu of %llu lines (expected ~%llu)", (unsigned long long)per,
              (unsigned long long)hist[per], (unsigned long long)lines, (unsigned long long)(lines / maxp));
    for (int mode = 0; mode < 3;
         mode++) { // the data follows the selected RNG, so every mixer must keep (line, generation) pairs apart
        std::set<u64> seen;
        Line a{}, b{}, c{};
        evo_make(7, 3, seed, mode, a);
        evo_make(7, 3, seed, mode, b);
        evo_make(7, 2, seed, mode, c);
        CHECK(!memcmp(&a, &b, 64), "rng %d: evolving data must be a pure function of (line, generation, seed)", mode);
        CHECK(memcmp(&a, &c, 64) != 0,
              "rng %d: the previous generation must differ from the new one (otherwise a lost write goes unseen)",
              mode);
        size_t dup = 0;
        for (u64 g = 0; g < 500; g++)
            for (u64 gen = 0; gen < 16; gen++) {
                Line l{};
                evo_make(g, gen, seed, mode, l);
                dup += !seen.insert(l.w[0]).second;
            }
        CHECK(!dup, "rng %d: %zu (line, generation) pairs produced the same first qword", mode, dup);
    }
}

// Every style / pattern name used by suite.cfg expands to at least one variant (a typo would silently drop a test).
static void test_suite_parses() {
    Cfg cfg;
    std::vector<TestDef> tests;
    std::istringstream is(suite_text());
    parse_cfg_stream(is, "<built-in>", cfg, tests, test_log);
    CHECK(tests.size() == 28, "expected 28 tests, got %zu", tests.size());
    size_t quick = 0;
    for (auto& t : tests)
        quick += t.p.flag("quick", false);
    CHECK(quick == 12, "expected 12 quick tests, got %zu", quick);
    for (auto& t : tests) {
        std::string algo = lower(t.p.str("algo", "fill"));
        CHECK(algos().count(algo) == 1, "test '%s': unknown algo '%s'", t.name.c_str(), algo.c_str());
        if (algo == "fill" || algo == "march" || algo == "rwstorm" || algo == "evolve" || algo == "copy" ||
            algo == "chase")
            CHECK(!build_vars(t.p, 0, 0, test_log).empty(), "test '%s': no data variants (typo in data/style/pattern?)",
                  t.name.c_str());
        if (algo == "march")
            CHECK(!parse_march_variants(t.p.str("march"), test_log).empty(), "test '%s': march parses to nothing",
                  t.name.c_str());
    }
}

// A misspelled key in suite.cfg is ignored without a word, so every key of the built-in suite must be one its algo
// reads, and the check itself must catch the usual mistakes.
static void test_suite_keys() {
    Cfg cfg;
    std::vector<TestDef> tests;
    std::istringstream is(suite_text());
    parse_cfg_stream(is, "<built-in>", cfg, tests, test_log);
    for (auto& t : tests)
        for (auto& k : unknown_keys(t))
            CHECK(false, "test '%s': key '%s' is not read by algo '%s' (typo in suite.cfg?)", t.name.c_str(), k.c_str(),
                  t.p.str("algo", "fill").c_str());
    for (auto& a : algos())
        CHECK(!algo_keys(a.first).empty() && algo_keys(a.first).count("algo"), "algo '%s' has no key list",
              a.first.c_str());

    const char* bad_cfg = "[default]\nwidth = 32\nwidht = 32\n"                            // typo in [default]
                          "[test]\nname = a\nalgo = march\nwindw_mb = 8\nwindow_mb = 8\n"  // typo in a test block
                          "[test]\nname = b\nalgo = pagemix\nwindow_mb = 8\nseconds = 5\n" // key of another algo
                          "[test]\nname = c\nalgo = chase\nmlp = 4\n";                     // width is inherited: fine
    Cfg c2;
    std::vector<TestDef> t2;
    std::istringstream is2(bad_cfg);
    parse_cfg_stream(is2, "<test>", c2, t2, test_log);
    CHECK(t2.size() == 3, "expected 3 tests, got %zu", t2.size());
    if (t2.size() == 3) {
        CHECK(unknown_keys(t2[0]) == std::vector<std::string>({"widht", "windw_mb"}), "march test: wrong key report");
        CHECK(unknown_keys(t2[1]) == std::vector<std::string>({"widht", "window_mb"}),
              "pagemix test: wrong key report");
        CHECK(unknown_keys(t2[2]) == std::vector<std::string>({"widht"}), "chase test: wrong key report");
    }
}

// Every key an XOpts lists must change what XOpts::from() returns (a stale entry would hide a typo of that key), and
// the sample table must have a non-default value for each listed key and no key that is not listed.
static void check_opts_keys(const char* algo, const std::set<std::string>& keys,
                            const std::map<std::string, std::string>& sample,
                            const std::function<std::string(const Params&)>& sig,
                            const std::map<std::string, std::string>& base = {}) {
    Params b0;
    b0.m = base; // a config that makes every other key matter (march: needs at least one element)
    std::string d = sig(b0);
    for (const std::string& k : keys) {
        auto it = sample.find(k);
        CHECK(it != sample.end(), "%s: test has no sample value for key '%s'", algo, k.c_str());
        if (it == sample.end())
            continue;
        Params one = b0;
        one.m[k] = it->second;
        CHECK(sig(one) != d, "%s key '%s' changes nothing in %sOpts::from", algo, k.c_str(), algo);
    }
    for (auto& kv : sample)
        CHECK(keys.count(kv.first), "%s: sample key '%s' is not in %sOpts::keys()", algo, kv.first.c_str(), algo);
}

// FillOpts::from() replaced the P.* reads of algo_fill: it must give the old defaults and the old clamps/fallbacks, and
// every key it lists must change something (a stale entry in keys() would hide a typo of that key).
static std::string fill_sig(const FillOpts& o) {
    std::string s = std::to_string((int)o.write.k) + (o.write.rev ? "r " : "f ") + std::to_string((int)o.verify.k) +
                    (o.verify.rev ? "r " : "f ");
    s += std::to_string(o.strided) + std::to_string(o.both) + std::to_string(o.cross) + std::to_string(o.nt) +
         std::to_string(o.flush) + " w" + std::to_string(o.width) + " r" + std::to_string(o.reads) + " p" +
         std::to_string(o.disturb_passes) + " s";
    for (u64 v : o.strides)
        s += std::to_string(v) + ",";
    s += " d";
    for (u64 v : o.dwell_ms)
        s += std::to_string(v) + ",";
    s += " m";
    for (int v : o.disturb)
        s += std::to_string(v) + ",";
    return s;
}
static void test_fill_opts() {
    Params none;
    FillOpts d = FillOpts::from(none, test_log);
    CHECK(fill_sig(d) == "0f 0f 00001 w32 r1 p2 s1, d0, m-1,", "fill defaults changed: %s", fill_sig(d).c_str());

    Params p;
    p.m = {{"order", "down"},
           {"verify_order", "stride"},
           {"stride", "4K 8K"},
           {"both", "yes"},
           {"cross", "1"},
           {"width", "64"},
           {"nt", "on"},
           {"flush", "no"},
           {"reads", "0"},
           {"dwell_ms", "5, 10"},
           {"disturb", "r bogus rw"},
           {"disturb_passes", "0"}};
    FillOpts o = FillOpts::from(p, test_log);
    CHECK(fill_sig(o) == "0r 1f 11110 w64 r1 p1 s64,128, d5,10, m0,2,", "fill options misparsed: %s",
          fill_sig(o).c_str()); // reads/disturb_passes 0 clamp to 1, an unknown disturb word is dropped
    Params e;
    e.m = {{"dwell_ms", ""}, {"disturb", "bogus"}};
    CHECK(fill_sig(FillOpts::from(e, test_log)) == fill_sig(d), "empty dwell_ms / unknown-only disturb must fall back");
    // verify_order follows order unless it is set itself
    Params f;
    f.m = {{"order", "random"}};
    FillOpts of = FillOpts::from(f, test_log);
    CHECK(of.write.k == Order::RANDOM && of.verify.k == Order::RANDOM, "verify_order must default to order");

    check_opts_keys("Fill", FillOpts::keys(),
                    {{"both", "1"},
                     {"cross", "1"},
                     {"disturb", "rw"},
                     {"disturb_passes", "5"},
                     {"dwell_ms", "7"},
                     {"flush", "0"},
                     {"nt", "1"},
                     {"order", "rev"},
                     {"reads", "3"},
                     {"verify_order", "random"},
                     {"width", "128"}},
                    [](const Params& p) { return fill_sig(FillOpts::from(p, test_log)); });
}

// MarchOpts::from(): defaults, parsing, and the one ordering that matters: with no march elements nothing after the
// march is parsed, so a bad `order` is not reported (algo_march prints "march: no elements" and returns).
static std::string march_sig(const MarchOpts& o) {
    std::string s = "v";
    for (auto& v : o.variants)
        s += v.label + ":" + std::to_string(v.els.size()) + ",";
    s += " o";
    for (auto& on : o.orders)
        s += on.first + ":" + std::to_string((int)on.second.k) + (on.second.rev ? "r" : "f") + ",";
    s += " s";
    for (u64 v : o.strides)
        s += std::to_string(v) + ",";
    s += " b";
    for (u64 v : o.bursts)
        s += std::to_string(v) + ",";
    return s + " " + std::to_string(o.anyDist) + std::to_string(o.nt) + std::to_string(o.flush) + " w" +
           std::to_string(o.width);
}
static void test_march_opts() {
    std::string said;
    Logger cap;
    cap.sink = [&](const std::string& t) {
        said += t;
    };
    Params empty;
    empty.m = {{"order", "bogus"}, {"burst", "4"}};
    MarchOpts e = MarchOpts::from(empty, cap);
    CHECK(e.variants.empty() && e.orders.empty() && e.bursts.empty() && said.empty(),
          "no march elements: nothing after `march` may be parsed or reported (said '%s')", said.c_str());

    Params d;
    d.m = {{"march", "u(w0) v(r0,w1) X(r2) D(10)"}};
    MarchOpts o = MarchOpts::from(d, cap);
    CHECK(march_sig(o) == "vmarch1:4, oseq:0f, s1, b1, 101 w32", "march defaults changed: %s", march_sig(o).c_str());

    Params p;
    p.m = {{"march", "a=u(w0) | b=v(r0)"},
           {"order", "stride bogus down"},
           {"stride", "4K"},
           {"burst", "2, 8"},
           {"width", "64"},
           {"nt", "1"},
           {"flush", "0"}};
    said.clear();
    MarchOpts q = MarchOpts::from(p, cap);
    CHECK(march_sig(q) == "va:1,b:1, ostride:1f,down:0r, s64, b2,8, 010 w64", "march options misparsed: %s",
          march_sig(q).c_str());
    CHECK(said == "bad order 'bogus'\n", "march: wrong message for a bad order: '%s'", said.c_str());

    check_opts_keys(
        "March", MarchOpts::keys(),
        {{"burst", "4"}, {"flush", "0"}, {"march", "u(w0) v(r0)"}, {"nt", "1"}, {"order", "rev"}, {"width", "64"}},
        [&](const Params& p2) { return march_sig(MarchOpts::from(p2, cap)); }, {{"march", "u(w0)"}});
}

// HammerOpts::from(): defaults, parsing.
static std::string hammer_sig(const HammerOpts& o) {
    std::string s = "r";
    for (u64 v : o.rowstride)
        s += std::to_string(v) + ",";
    s += " a";
    for (u64 v : o.aggressors)
        s += std::to_string(v) + ",";
    return s + " acts" + std::to_string(o.acts) + " rounds" + std::to_string(o.rounds);
}
static void test_hammer_opts() {
    CHECK(hammer_sig(HammerOpts::from(Params())) == "r262144,1048576, a2,9, acts400000 rounds24",
          "hammer defaults changed: %s", hammer_sig(HammerOpts::from(Params())).c_str());
    Params p;
    p.m = {{"rowstride", "64K, 2M"}, {"aggressors", "3"}, {"acts", "1K"}, {"rounds", "0"}};
    CHECK(hammer_sig(HammerOpts::from(p)) == "r65536,2097152, a3, acts1024 rounds0", "hammer options misparsed: %s",
          hammer_sig(HammerOpts::from(p)).c_str());
    check_opts_keys("Hammer", HammerOpts::keys(),
                    {{"acts", "5"}, {"aggressors", "3"}, {"rounds", "2"}, {"rowstride", "64K"}},
                    [](const Params& q) { return hammer_sig(HammerOpts::from(q)); });
}

// CopyOpts::from(): defaults, the block clamp, method words keep their case (labels print them).
static std::string copy_sig(const CopyOpts& o) {
    std::string s = "m";
    for (auto& v : o.methods)
        s += v + ",";
    return s + " block" + std::to_string(o.block);
}
static void test_copy_opts() {
    CHECK(copy_sig(CopyOpts::from(Params())) == "mmemcpy,movsb,ntcopy, block65536", "copy defaults changed: %s",
          copy_sig(CopyOpts::from(Params())).c_str());
    Params p;
    p.m = {{"method", "ntcopy MemCpy"}, {"block", "1K"}};
    CHECK(copy_sig(CopyOpts::from(p)) == "mntcopy,MemCpy, block1024", "copy options misparsed: %s",
          copy_sig(CopyOpts::from(p)).c_str());
    Params tiny;
    tiny.m = {{"block", "0"}};
    CHECK(CopyOpts::from(tiny).block == 64, "copy block must clamp to one line");
    check_opts_keys("Copy", CopyOpts::keys(), {{"block", "4K"}, {"method", "movsb"}},
                    [](const Params& q) { return copy_sig(CopyOpts::from(q)); });
}

// ChaseOpts::from(): defaults, the empty-list fallback and the seeds clamp.
static std::string chase_sig(const ChaseOpts& o) {
    std::string s = "mlp";
    for (u64 v : o.mlp)
        s += std::to_string(v) + ",";
    return s + " seeds" + std::to_string(o.seeds);
}
static void test_chase_opts() {
    CHECK(chase_sig(ChaseOpts::from(Params())) == "mlp12, seeds2", "chase defaults changed: %s",
          chase_sig(ChaseOpts::from(Params())).c_str());
    Params p;
    p.m = {{"mlp", "4 16"}, {"seeds", "0"}};
    CHECK(chase_sig(ChaseOpts::from(p)) == "mlp4,16, seeds1", "chase options misparsed: %s",
          chase_sig(ChaseOpts::from(p)).c_str());
    Params e;
    e.m = {{"mlp", ""}};
    CHECK(chase_sig(ChaseOpts::from(e)) == "mlp12, seeds2", "an empty mlp list must fall back to 12");
    check_opts_keys("Chase", ChaseOpts::keys(), {{"mlp", "4"}, {"seeds", "5"}},
                    [](const Params& q) { return chase_sig(ChaseOpts::from(q)); });
}

// RwstormOpts::from(): defaults (writer order random, reader order seq), parsing, clamp.
static std::string rwstorm_sig(const RwstormOpts& o) {
    std::string s = std::to_string((int)o.write.k) + (o.write.rev ? "r " : "f ") + std::to_string((int)o.verify.k) +
                    (o.verify.rev ? "r" : "f") + " s";
    for (u64 v : o.strides)
        s += std::to_string(v) + ",";
    return s + " w" + std::to_string(o.width) + " nt" + std::to_string(o.nt) + " fl" + std::to_string(o.flush) + " p" +
           std::to_string(o.passes);
}
static void test_rwstorm_opts() {
    CHECK(rwstorm_sig(RwstormOpts::from(Params(), test_log)) == "2f 0f s1, w32 nt0 fl1 p4",
          "rwstorm defaults changed: %s", rwstorm_sig(RwstormOpts::from(Params(), test_log)).c_str());
    Params p;
    p.m = {{"order", "stride"}, {"verify_order", "down"}, {"stride", "4K 8K"}, {"width", "16"},
           {"nt", "1"},         {"flush", "0"},           {"passes", "0"}};
    CHECK(rwstorm_sig(RwstormOpts::from(p, test_log)) == "1f 0r s64,128, w16 nt1 fl0 p1",
          "rwstorm options misparsed: %s", rwstorm_sig(RwstormOpts::from(p, test_log)).c_str());
    check_opts_keys(
        "Rwstorm", RwstormOpts::keys(),
        {{"flush", "0"}, {"nt", "1"}, {"order", "rev"}, {"passes", "9"}, {"verify_order", "random"}, {"width", "64"}},
        [](const Params& q) { return rwstorm_sig(RwstormOpts::from(q, test_log)); });
}

int main() {
    test_log.sink = [](const std::string& s) {
        printf("  (engine) %s", s.c_str());
    }; // keep parser diagnostics visible, and keep them off the real log
    test_orders();
    test_windows();
    test_generators();
    test_mixers();
    test_seed();
    test_evolve();
    test_suite_parses();
    test_suite_keys();
    test_fill_opts();
    test_march_opts();
    test_hammer_opts();
    test_copy_opts();
    test_chase_opts();
    test_rwstorm_opts();
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    puts("unit tests: all checks passed");
    return 0;
}
