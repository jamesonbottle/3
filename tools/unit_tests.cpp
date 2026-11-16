// Deterministic unit tests for the engine's pure logic (no memory is stressed). Build and run: tools/selftest.sh
//   cmake -S . -B build/cmake && cmake --build build/cmake --target unit_tests && build/cmake/unit_tests
#include <cmath>
#include <set>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <stdexcept>
#include <system_error>
#include "algo.h"
#include "cpu_check.h"
#include "coverage.h"
#include "cpu_set.h"
#include "flush.h"
#include "lineio.h"
#include "gen.h"
#include "log.h"
#include "order.h"
#include "params.h"
#include "run.h"
#include "seed.h"
#include "sizing.h"
#include "start_guard.h"
#include "stop_gate.h"
#include "suite.h"
#include "types.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // SetThreadExecutionState, to look at the state keep_awake() sets
#endif

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
    for (auto& sp : sat_table()) {
        CHECK(sp.len >= 1 && sp.len <= 64 && !(sp.len & (sp.len - 1)), "pattern %s len=%d (must be a power of two)",
              sp.name, sp.len);
        // the mask in sat_word must give exactly what the plain modulo of the pattern's definition gives
        u64 bad = 0;
        for (int rep = 0; rep < 4; rep++)
            for (u64 idx = 0; idx < 4096; idx++)
                bad += sat_word(sp, idx, rep) != sp.w[(idx >> rep) % (u64)sp.len];
        CHECK(!bad, "pattern %s: sat_word differs from the modulo form in %llu cases", sp.name,
              (unsigned long long)bad);
    }
    Gen g;
    g.kind = 1;
    g.seed = 99;
    Line a{}, b{}, c{};
    g.make(7, nullptr, false, a);
    g.make(7, nullptr, false, b);
    g.make(7, nullptr, true, c);
    bool same = !memcmp(&a, &b, kLineBytes), comp = true;
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
    const Rng rngs[] = {Rng::SplitMix, Rng::Murmur, Rng::XorShift};
    const int N = 8192;
    static u32 cnt[64][64];
    for (int mode = 0; mode < 3; mode++) {
        memset(cnt, 0, sizeof cnt);
        u64 rs = 0x1234;
        for (int n = 0; n < N; n++) {
            rs = splitmix(rs);
            u64 h0 = mix64(rs, rngs[mode]);
            for (int i = 0; i < 64; i++) {
                u64 d = h0 ^ mix64(rs ^ (1ull << i), rngs[mode]);
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
        for (auto& v : build_vars(p, salt, Rng::Default, test_log))
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
    // The per-pass table (EvoPass) must give exactly the direct arithmetic, with the table (maxp <= kMax) and without
    // it (the fallback above kMax), and every period must stay inside 1..maxp for any maxp, including 1.
    for (u64 mp : {(u64)1, (u64)2, (u64)8, EvoPass::kMax, EvoPass::kMax + 1, (u64)5000}) {
        EvoPass ep;
        ep.init(seed, mp);
        CHECK(ep.tab == (mp <= EvoPass::kMax), "maxp %llu: table mode is wrong", (unsigned long long)mp);
        u64 bad = 0, outside = 0;
        for (u64 pass = 0; pass <= 40; pass++) {
            ep.set(pass);
            for (u64 per = 1; per <= mp; per += (mp > 64 ? 37 : 1)) {
                Line a{}, b{};
                bad += ep.is_due(per) != (pass % per == 0);
                bad += ep.key(per) != evo_key(seed, pass / per);
                evo_make_k(11, ep.key(per), Rng::SplitMix, a);
                evo_make(11, pass / per, seed, Rng::SplitMix, b);
                bad += memcmp(&a, &b, kLineBytes) != 0;
            }
        }
        for (u64 g = 0; g < 5000; g++) {
            u64 per = evo_period(g, seed, mp);
            outside += per < 1 || per > mp;
        }
        CHECK(!bad, "maxp %llu: EvoPass differs from the direct arithmetic in %llu cases", (unsigned long long)mp,
              (unsigned long long)bad);
        CHECK(!outside, "maxp %llu: %llu periods outside 1..maxp", (unsigned long long)mp, (unsigned long long)outside);
    }
    const Rng rngs[] = {Rng::SplitMix, Rng::Murmur, Rng::XorShift};
    for (int mode = 0; mode < 3;
         mode++) { // the data follows the selected RNG, so every mixer must keep (line, generation) pairs apart
        std::set<u64> seen;
        Line a{}, b{}, c{};
        evo_make(7, 3, seed, rngs[mode], a);
        evo_make(7, 3, seed, rngs[mode], b);
        evo_make(7, 2, seed, rngs[mode], c);
        CHECK(!memcmp(&a, &b, kLineBytes), "rng %d: evolving data must be a pure function of (line, generation, seed)",
              mode);
        CHECK(memcmp(&a, &c, kLineBytes) != 0,
              "rng %d: the previous generation must differ from the new one (otherwise a lost write goes unseen)",
              mode);
        size_t dup = 0;
        for (u64 g = 0; g < 500; g++)
            for (u64 gen = 0; gen < 16; gen++) {
                Line l{};
                evo_make(g, gen, seed, rngs[mode], l);
                dup += !seen.insert(l.w[0]).second;
            }
        CHECK(!dup, "rng %d: %zu (line, generation) pairs produced the same first qword", mode, dup);
    }
}

// Every style / pattern name used by the built-in suite expands to at least one variant (a typo would silently drop a
// test).
static void test_suite_parses() {
    std::vector<TestDef> tests = builtin_suite();
    CHECK(tests.size() == 28, "expected 28 tests, got %zu", tests.size());
    size_t quick = 0;
    for (auto& t : tests)
        quick += t.p.flag("quick", false);
    CHECK(quick == 12, "expected 12 quick tests, got %zu", quick);
    for (auto& t : tests) {
        const std::string algo = t.algo();
        CHECK(algos().count(algo) == 1, "test '%s': unknown algo '%s'", t.name.c_str(), algo.c_str());
        if (algo == "fill" || algo == "march" || algo == "rwstorm" || algo == "evolve" || algo == "copy" ||
            algo == "chase")
            CHECK(!build_vars(t.p, 0, Rng::Default, test_log).empty(),
                  "test '%s': no data variants (typo in data/style/pattern?)", t.name.c_str());
        if (algo == "march")
            CHECK(!parse_march_variants(t.p.str("march"), test_log).empty(), "test '%s': march parses to nothing",
                  t.name.c_str());
    }
}

// A misspelled key in the suite is ignored without a word, so every key of the built-in suite must be one its algo
// reads, and the check itself must catch the usual mistakes.
static void test_suite_keys() {
    std::vector<TestDef> tests = builtin_suite();
    for (auto& t : tests)
        for (auto& k : unknown_keys(t))
            CHECK(false, "test '%s': key '%s' is not read by algo '%s' (typo in suite_data.cpp?)", t.name.c_str(),
                  k.c_str(), t.algo().c_str());
    for (auto& a : algos())
        CHECK(!algo_keys(a.first).empty() && algo_keys(a.first).count("algo"), "algo '%s' has no key list",
              a.first.c_str());

    // a typo in a key every test inherits, one in a test's own keys, and a key that belongs to another algo
    auto mk = [](const char* algo, std::initializer_list<const char*> own) {
        TestDef t;
        t.p.m["width"] = "32";
        t.p.m["widht"] = "32"; // typo in the inherited defaults
        t.p.m["algo"] = algo;
        for (const char* k : own) {
            t.p.m[k] = "8";
            t.own.insert(k);
        }
        return t;
    };
    std::vector<TestDef> t2 = {mk("march", {"windw_mb", "window_mb"}), mk("pagemix", {"window_mb", "seconds"}),
                               mk("chase", {"mlp"})};
    if (t2.size() == 3) {
        CHECK(unknown_keys(t2[0]) == std::vector<std::string>({"widht", "windw_mb"}), "march test: wrong key report");
        CHECK(unknown_keys(t2[1]) == std::vector<std::string>({"widht", "window_mb"}),
              "pagemix test: wrong key report");
        CHECK(unknown_keys(t2[2]) == std::vector<std::string>({"widht"}), "chase test: wrong key report");
    }
}

// --cache (Settings::cacheMode) must reach every algo that has a flush / nt key, evolve included, and must not add the
// key to an algo that lacks it. Checked over the whole built-in suite for each of the three overriding modes.
// The flush instruction is chosen once per run: Auto / ClFlushOpt use CLFLUSHOPT exactly when the CPU reports it,
// ClFlush never does, and --cache bypass style accesses (IO<..., FL=1>) keep the data intact in every mode.
// The AVX2 gate (E3): the decision from raw CPUID / XGETBV data, and the real reading of this machine.
static void test_avx2_check() {
    const unsigned avxBits = (1u << 27) | (1u << 28); // OSXSAVE + AVX in CPUID.1:ECX
    const unsigned avx2Bit = 1u << 5;                 // CPUID.(7,0):EBX
    const unsigned long long ymm = 0x7;               // XCR0: x87 + SSE + YMM state
    using S = Avx2Status;
    CHECK(avx2_decide(true, avxBits, avx2Bit, ymm) == S::Ok, "a CPU with AVX2 and YMM state enabled must pass");
    CHECK(avx2_decide(true, avxBits, avx2Bit, 0x3) == S::NoOs, "YMM state off in XCR0 must be NoOs");
    CHECK(avx2_decide(true, avxBits, avx2Bit, 0x5) == S::NoOs, "SSE state off in XCR0 must be NoOs");
    CHECK(avx2_decide(true, avxBits, avx2Bit, 0xE7) == S::Ok, "extra XCR0 bits (AVX-512 state) must not matter");
    CHECK(avx2_decide(true, (1u << 28), avx2Bit, ymm) == S::NoOs, "no OSXSAVE must be NoOs (XGETBV would fault)");
    CHECK(avx2_decide(true, avxBits, 0, ymm) == S::NoCpu, "no AVX2 bit must be NoCpu");
    CHECK(avx2_decide(true, (1u << 27), avx2Bit, ymm) == S::NoCpu, "AVX2 bit without the AVX bit must be NoCpu");
    CHECK(avx2_decide(false, avxBits, avx2Bit, ymm) == S::NoCpu, "no CPUID leaf 7 must be NoCpu, whatever EBX holds");
    CHECK(avx2_decide(true, 0, 0, 0) == S::NoCpu, "all zero (an old CPU) must be NoCpu");
    CHECK(avx2_message(S::Ok) == nullptr, "Ok has no message");
    CHECK(avx2_message(S::NoCpu) && avx2_message(S::NoOs) &&
              strcmp(avx2_message(S::NoCpu), avx2_message(S::NoOs)) != 0 && strstr(avx2_message(S::NoCpu), "AVX2") &&
              strstr(avx2_message(S::NoOs), "operating system"),
          "the two failures need different texts that name the problem");
    // This test program is itself compiled for AVX2 and has been running AVX2 code since main(), so the machine must
    // say Ok.
    CHECK(avx2_status() == S::Ok, "this machine runs AVX2 code, so avx2_status() must be Ok");
}

static void test_flush_mode() {
    const bool has = cpu_has_clflushopt();
    set_flush_mode(FlushMode::ClFlush);
    CHECK(!flush_uses_opt() && !strcmp(flush_mode_name(), "CLFLUSH"), "ClFlush must force plain CLFLUSH");
    set_flush_mode(FlushMode::Auto);
    CHECK(flush_uses_opt() == has, "Auto must use CLFLUSHOPT iff the CPU has it (cpu=%d)", (int)has);
    set_flush_mode(FlushMode::ClFlushOpt);
    CHECK(flush_uses_opt() == has, "ClFlushOpt must fall back to CLFLUSH on a CPU without it (cpu=%d)", (int)has);
    CHECK(!strcmp(flush_mode_name(), has ? "CLFLUSHOPT" : "CLFLUSH"), "mode name does not match the instruction used");
    for (FlushMode m : {FlushMode::ClFlush, FlushMode::Auto}) {
        set_flush_mode(m);
        using io = IO<32, 0, 1>; // an alias: a comma inside CHECK(...) would split the macro arguments
        alignas(64) Line mem[4];
        for (int i = 0; i < 4; i++) {
            Line e;
            for (int k = 0; k < 8; k++)
                e.w[k] = splitmix(i * 8 + k);
            io::store(&mem[i], e);
            Line act;
            CHECK(io::verify(&mem[i], e, act), "mode %d: flushed store/verify of line %d failed", (int)m, i);
            e.w[3] ^= 1;
            CHECK(!io::verify(&mem[i], e, act) && act.w[3] == (e.w[3] ^ 1),
                  "mode %d: a wrong expectation must be reported with the data actually read", (int)m);
        }
    }
    set_flush_mode(FlushMode::Auto);
}

static void test_cache_override() {
    const struct {
        CacheMode mode;
        const char* flush;
        const char* nt;
        const char* name;
    } modes[] = {{CacheMode::Bypass, "1", "0", "bypass"},
                 {CacheMode::Cached, "0", "0", "cached"},
                 {CacheMode::NonTemporal, "0", "1", "nt"}};
    std::set<std::string> seenFlush, seenNt; // algos that took part, to show the check is not vacuous
    for (const auto& m : modes) {
        Settings s;
        s.cacheMode = m.mode;
        std::vector<TestDef> sel = select_tests(s, builtin_suite());
        CHECK(sel.size() == 28, "--cache %s: expected all 28 tests selected, got %zu", m.name, sel.size());
        for (auto& t : sel) {
            const std::string algo = t.algo();
            const auto& keys = algo_keys(algo);
            if (keys.count("flush")) {
                seenFlush.insert(algo);
                CHECK(t.p.str("flush") == m.flush, "--cache %s: test '%s' (algo %s) has flush='%s', expected '%s'",
                      m.name, t.name.c_str(), algo.c_str(), t.p.str("flush").c_str(), m.flush);
            }
            if (keys.count("nt")) {
                seenNt.insert(algo);
                CHECK(t.p.str("nt") == m.nt, "--cache %s: test '%s' (algo %s) has nt='%s', expected '%s'", m.name,
                      t.name.c_str(), algo.c_str(), t.p.str("nt").c_str(), m.nt);
            }
            // an algo without the key must not gain it from the override (kDefaults may give it flush, so only nt is
            // checked for absence when the test did not set it itself)
            if (!keys.count("nt"))
                CHECK(!t.p.has("nt"), "--cache %s: test '%s' (algo %s) gained an nt key it does not read", m.name,
                      t.name.c_str(), algo.c_str());
        }
    }
    for (const char* a : {"fill", "march", "rwstorm", "evolve"}) {
        CHECK(seenFlush.count(a), "--cache check never saw algo '%s' with a flush key", a);
        CHECK(seenNt.count(a), "--cache check never saw algo '%s' with an nt key", a);
    }

    // Default leaves the suite's own values alone
    Settings d;
    std::vector<TestDef> base = builtin_suite(), sel = select_tests(d, builtin_suite());
    for (size_t i = 0; i < sel.size() && i < base.size(); i++)
        CHECK(sel[i].p.m == base[i].p.m, "--cache default changed the parameters of test '%s'", sel[i].name.c_str());
}

// algo_keys() (algo.cpp) holds, by hand, the keys the shared helpers build_vars(), stride_list() and
// Run::window_lines() read. Record the keys those helpers really look up (Params::reads) and require every algo that
// calls a helper to list each of its keys: a key added to a helper but not to the list would otherwise be reported as
// "not used" at run time.
static void test_shared_key_sets() {
    using Cfg = std::map<std::string, std::string>;
    auto record = [](const std::function<void(const Params&)>& call) {
        std::set<std::string> seen;
        Params p;
        p.reads = &seen;
        call(p);
        return seen;
    };
    // build_vars reads other keys in each data mode, so drive every branch (each config sets all the keys of its
    // branch)
    const std::vector<Cfg> dataCfgs = {{{"data", "sat"}, {"sat_width", "32 64"}, {"seed", "1"}},
                                       {{"data", "hash"}, {"seed", "1"}, {"seeds", "2"}},
                                       {{"data", "addr"}, {"seed", "1"}, {"seeds", "2"}},
                                       {{"style", "victim"}, {"victims", "1 2"}, {"altshift", "1"}, {"modulo", "2"}},
                                       {{"pattern", "0x1"}, {"altshift", "1"}, {"modulo", "2"}}};
    std::set<std::string> vars, strides, windows;
    for (const Cfg& c : dataCfgs) {
        std::set<std::string> seen = record([&](const Params& q) {
            Params p = q; // copies the recorder pointer
            p.m = c;
            build_vars(p, 0, Rng::Default, test_log);
        });
        vars.insert(seen.begin(), seen.end());
    }
    strides = record([](const Params& q) {
        Params p = q;
        p.m["stride"] = "4K";
        stride_list(p, true);
    });
    windows = record([](const Params& q) {
        Params p = q;
        p.m["window_mb"] = "64";
        Run R(test_log);
        R.layout.T = 1;
        R.window_lines(p);
    });
    // a recorder that saw nothing (or missed the obvious keys) would make every check below pass for no reason
    CHECK(vars.count("data") && vars.count("seeds") && vars.count("victims") && vars.count("modulo"),
          "build_vars key recorder missed keys it must see");
    CHECK(strides.count("stride") && windows.count("window_mb"), "stride/window key recorder saw nothing");

    // which algos call which helper (the algo_*.cpp files; see the comment at the top of each)
    struct Use {
        const char* helper;
        const std::set<std::string>* keys;
        std::vector<const char*> algos;
    };
    const Use uses[] = {
        {"build_vars", &vars, {"fill", "march", "evolve", "rwstorm", "copy", "hammer"}},
        {"stride_list", &strides, {"fill", "march", "evolve", "rwstorm"}},
        {"Run::window_lines", &windows, {"fill", "march", "evolve", "rwstorm", "copy", "hammer", "chase"}},
    };
    for (const Use& u : uses)
        for (const char* a : u.algos)
            for (const std::string& k : *u.keys)
                CHECK(algo_keys(a).count(k),
                      "algo '%s' calls %s, which reads key '%s', but algo_keys() does not list it", a, u.helper,
                      k.c_str());
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

// EvolveOpts::from(): defaults (writes random, reads seq, 16 passes, periods 1..8), parsing, clamps.
static std::string evolve_sig(const EvolveOpts& o) {
    std::string s = std::to_string((int)o.write.k) + (o.write.rev ? "r " : "f ") + std::to_string((int)o.verify.k) +
                    (o.verify.rev ? "r" : "f") + " s";
    for (u64 v : o.strides)
        s += std::to_string(v) + ",";
    return s + " w" + std::to_string(o.width) + " nt" + std::to_string(o.nt) + " fl" + std::to_string(o.flush) + " x" +
           std::to_string(o.cross) + " p" + std::to_string(o.passes) + " per" + std::to_string(o.period);
}
static void test_evolve_opts() {
    CHECK(evolve_sig(EvolveOpts::from(Params(), test_log)) == "2f 0f s1, w32 nt0 fl1 x0 p16 per8",
          "evolve defaults changed: %s", evolve_sig(EvolveOpts::from(Params(), test_log)).c_str());
    Params p;
    p.m = {{"order", "stride"}, {"verify_order", "down"}, {"stride", "4K"}, {"width", "16"}, {"nt", "1"},
           {"flush", "0"},      {"cross", "1"},           {"passes", "0"},  {"period", "0"}};
    CHECK(evolve_sig(EvolveOpts::from(p, test_log)) == "1f 0r s64, w16 nt1 fl0 x1 p1 per1",
          "evolve options misparsed: %s", evolve_sig(EvolveOpts::from(p, test_log)).c_str());
    check_opts_keys("Evolve", EvolveOpts::keys(),
                    {{"cross", "1"},
                     {"flush", "0"},
                     {"nt", "1"},
                     {"order", "rev"},
                     {"passes", "9"},
                     {"period", "3"},
                     {"verify_order", "random"},
                     {"width", "64"}},
                    [](const Params& q) { return evolve_sig(EvolveOpts::from(q, test_log)); });
}

// ProbeOpts::from(): the default stride sweep (64 B .. 16 MB), parsing, the sample clamp.
static std::string probe_sig(const ProbeOpts& o) {
    std::string s = "st";
    for (u64 v : o.stride)
        s += std::to_string(v) + ",";
    return s + " n" + std::to_string(o.samples);
}
static void test_probe_opts() {
    ProbeOpts d = ProbeOpts::from(Params());
    CHECK(d.stride.size() == 19 && d.stride.front() == 64 && d.stride[4] == 1024 && d.stride.back() == 16u << 20 &&
              d.samples == 20000,
          "probe defaults changed: %s", probe_sig(d).c_str());
    Params p;
    p.m = {{"stride", "4K, 64K"}, {"samples", "10"}};
    CHECK(probe_sig(ProbeOpts::from(p)) == "st4096,65536, n1000", "probe options misparsed: %s",
          probe_sig(ProbeOpts::from(p)).c_str());
    check_opts_keys("Probe", ProbeOpts::keys(), {{"samples", "5000"}, {"stride", "4K"}},
                    [](const Params& q) { return probe_sig(ProbeOpts::from(q)); });
}

// PagemixOpts::from(): default 30 s, parsing.
static std::string pagemix_sig(const PagemixOpts& o) {
    return "sec" + std::to_string((long long)o.seconds);
}
static void test_pagemix_opts() {
    CHECK(pagemix_sig(PagemixOpts::from(Params())) == "sec30", "pagemix default changed: %s",
          pagemix_sig(PagemixOpts::from(Params())).c_str());
    Params p;
    p.m = {{"seconds", "5"}};
    CHECK(pagemix_sig(PagemixOpts::from(p)) == "sec5", "pagemix seconds misparsed");
    check_opts_keys("Pagemix", PagemixOpts::keys(), {{"seconds", "7"}},
                    [](const Params& q) { return pagemix_sig(PagemixOpts::from(q)); });
}

// CoherencyOpts::from(): defaults (lines 2 8 64, 1000 increments, 8 s), parsing, the inc clamp.
static std::string coherency_sig(const CoherencyOpts& o) {
    std::string s = "l";
    for (u64 v : o.lines)
        s += std::to_string(v) + ",";
    return s + " inc" + std::to_string(o.inc) + " sec" + std::to_string((long long)o.seconds);
}
static void test_coherency_opts() {
    CHECK(coherency_sig(CoherencyOpts::from(Params())) == "l2,8,64, inc1000 sec8", "coherency defaults changed: %s",
          coherency_sig(CoherencyOpts::from(Params())).c_str());
    Params p;
    p.m = {{"lines", "4, 16"}, {"inc", "0"}, {"seconds", "2"}};
    CHECK(coherency_sig(CoherencyOpts::from(p)) == "l4,16, inc1 sec2", "coherency options misparsed: %s",
          coherency_sig(CoherencyOpts::from(p)).c_str());
    check_opts_keys("Coherency", CoherencyOpts::keys(), {{"inc", "50"}, {"lines", "4"}, {"seconds", "3"}},
                    [](const Params& q) { return coherency_sig(CoherencyOpts::from(q)); });
}

// Values, not only keys: a typo such as "4KB" or "ture" must be reported, never silently turned into a number or false.
static void test_values() {
    struct Good {
        const char* s;
        u64 v;
    };
    const Good good[] = {{"4K", 4096},
                         {"2M", 2ull << 20},
                         {"1g", 1ull << 30},
                         {"0x10", 16},
                         {"0XfF", 255},
                         {" 7 ", 7},
                         {"0", 0},
                         {"18446744073709551615", ~0ull},
                         {"0xFFFFFFFFFFFFFFFF", ~0ull},
                         {"17179869183G", 17179869183ull << 30}};
    for (auto& g : good) {
        u64 v = 12345;
        CHECK(pnum_checked(g.s, v) && v == g.v, "pnum_checked('%s') must give %llu", g.s, (unsigned long long)g.v);
        CHECK(pnum(g.s) == g.v, "pnum('%s') changed for a valid value", g.s);
    }
    const char* bad[] = {"",
                         " ",
                         "4KB",
                         "abc",
                         "0x",
                         "0xG1",
                         "1.5G",
                         "-1",
                         "+1",
                         "99999999999999999999",
                         "0x10000000000000000",
                         "17179869184G",
                         "18446744073709551615K",
                         "K",
                         "1 2",
                         "0x10K"};
    for (const char* b : bad) {
        u64 v = 777;
        CHECK(!pnum_checked(b, v), "pnum_checked('%s') must reject it", b);
        CHECK(pnum(b) == 0, "pnum('%s') must give 0 for a bad value", b);
    }

    Params p;
    p.test = "demo";
    p.m["n"] = "4KB";
    p.m["ok"] = "4K";
    p.m["f1"] = "ture";
    p.m["f2"] = "Yes";
    p.m["f3"] = "off";
    p.m["list"] = "4K 12x 2M";
    CHECK(p.num("ok", 1) == 4096 && p.bad.empty(), "a valid number must not be recorded as bad");
    CHECK(p.flag("f2", false) && !p.flag("f3", true) && p.bad.empty(), "yes/off are valid flags");
    CHECK(p.num("n", 9) == 0 && p.bad.size() == 1, "a bad number must read as 0 and be recorded");
    CHECK(!p.flag("f1", true) && p.bad.size() == 2, "a misspelled flag must read as false and be recorded");
    CHECK(p.nums("list").size() == 3 && p.bad.size() == 3, "one bad item of a list must be recorded");
    CHECK(p.bad[0].find("test 'demo': key 'n': '4KB'") == 0, "message must name test, key and value: %s",
          p.bad[0].c_str());

    // the whole built-in suite is clean, and bad_values() sees the usual mistakes
    for (auto& t : builtin_suite())
        for (auto& m : bad_values(t))
            CHECK(false, "built-in suite: %s", m.c_str());
    auto mk = [](const char* algo, const char* k, const char* v) {
        TestDef t;
        t.name = "bad-demo";
        t.p.m["algo"] = algo;
        t.p.m[k] = v;
        return t;
    };
    struct Case {
        const char* algo;
        const char* key;
        const char* val;
        const char* expect; // text the report must contain
    };
    const Case cases[] = {
        {"evolve", "passes", "4KB", "key 'passes': '4KB'"},
        {"fill", "nt", "ture", "key 'nt': 'ture'"},
        {"fill", "order", "bogus", "unknown address order 'bogus'"},
        {"march", "march", "u(w0) D(abc)", "bad march element 'D(abc)'"},
        {"march", "march", "u(w0) X(r1.5)", "bad march element 'X(r1.5)'"},
        {"fill", "stride", "4K 1.5G", "key 'stride': '1.5G'"},
        {"fill", "window_mb", "-1", "key 'window_mb': '-1'"},
        {"fill", "pattern", "0xZZ", "key 'pattern': '0xZZ'"},
        {"hammer", "acts", "99999999999999999999", "key 'acts'"},
        {"coherency", "seconds", "3s", "key 'seconds': '3s'"},
        {"copy", "method", "memcpy movzb", "unknown copy method 'movzb'"},
        {"fill", "width", "12", "key 'width': '12' is not a bus width"},
        {"march", "march", "^(w0) >(r0)", "bad march element '>(r0)' (direction must be ^ or u, v or *)"},
        {"fill", "altshift", "6 64", "key 'altshift': 64 is not a bit of the line index"},
    };
    for (auto& c : cases) {
        std::vector<std::string> r = bad_values(mk(c.algo, c.key, c.val));
        bool found = false;
        for (auto& m : r)
            found = found || (m.find(c.expect) != std::string::npos && m.find("test 'bad-demo'") != std::string::npos);
        CHECK(found, "%s %s=%s: bad_values must report '%s' with the test name (got %zu message(s): %s)", c.algo, c.key,
              c.val, c.expect, r.size(), r.empty() ? "" : r[0].c_str());
    }
    { // victims: lanes are 0..31, so 32 and above can never match
        TestDef t = mk("fill", "style", "victim");
        t.p.m["victims"] = "3 32";
        bool found = false;
        for (auto& m : bad_values(t))
            found = found || m.find("key 'victims': lane 32 does not exist") != std::string::npos;
        CHECK(found, "victims=32 must be reported");
    }
    { // the values just inside the limits stay silent, in any letter case
        const Case fine[] = {{"copy", "method", "MEMCPY movsb NtCopy", ""},
                             {"fill", "width", "8", ""},
                             {"fill", "width", "16", ""},
                             {"fill", "width", "32", ""},
                             {"fill", "altshift", "0 63", ""},
                             {"march", "march", "*(w0) ^(r0,w1) u(r0) v(r1,w0) V(r0) D(5) X(r2)", ""}};
        for (auto& c : fine) {
            std::vector<std::string> r = bad_values(mk(c.algo, c.key, c.val));
            CHECK(r.empty(), "%s %s=%s is valid but bad_values says: %s", c.algo, c.key, c.val,
                  r.empty() ? "" : r[0].c_str());
        }
        TestDef t = mk("fill", "style", "victim");
        t.p.m["victims"] = "0 31";
        CHECK(bad_values(t).empty(), "victims 0 and 31 are valid");
    }
}

// Exceptions: nothing that goes wrong in a worker or in run_suite's own code may end in std::terminate (a throw leaving
// a thread function, or a joinable std::thread destroyed during unwinding). Reaching the end of these tests IS the
// check.
static void test_exceptions() {
    Logger lg;
    lg.sink = [](const std::string&) {
    };
    { // a worker throws: the others finish, the exception comes out of parallel() on the calling thread
        Run R(lg);
        R.layout.T = 3;
        std::atomic<int> finished{0};
        bool caught = false;
        std::string what;
        try {
            R.parallel([&](int tid) {
                if (tid == 1)
                    throw std::runtime_error("boom 1");
                finished++;
            });
        } catch (const std::exception& e) {
            caught = true;
            what = e.what();
        }
        CHECK(caught && what == "boom 1", "parallel() must rethrow the worker's exception, got '%s'", what.c_str());
        CHECK(finished == 2, "the other workers must finish, %d did", finished.load());
        CHECK(R.stop.load(), "a failed worker must stop the run");
    }
    { // the partner is already waiting at the barrier when a worker throws: it must be released, not wait forever
        Run R(lg);
        R.layout.T = 2;
        std::atomic<int> passed{0};
        bool caught = false;
        try {
            R.parallel([&](int tid) {
                if (tid == 1) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // tid 0 is in sync() by now
                    throw std::runtime_error("boom 2");
                }
                R.sync();
                passed++;
            });
        } catch (const std::exception&) {
            caught = true;
        }
        CHECK(caught && passed == 1, "a thrower must release a worker waiting at the barrier (caught %d, passed %d)",
              (int)caught, passed.load());
    }
    { // an exception deep inside run_suite, while FPU threads run and memory is allocated: exit code 3, nothing leaked
        Settings st;
        st.megabytes = 8;
        st.threads = 2;
        st.largePages = false;
        st.stressFpu = true;
        st.filter = "Bulk copy";
        st.loops = 1;
        std::string text;
        bool thrown = false;
        Logger sink;
        sink.sink = [&](const std::string& t) {
            if (!thrown && t.find("[loop") != std::string::npos) {
                thrown = true;
                throw std::runtime_error("boom 3"); // thrown from inside run_suite, after the allocation
            }
            text += t;
        };
        Run R(sink);
        int rc = run_suite(st, R);
        CHECK(thrown, "the test hook never fired");
        CHECK(rc == 3, "an exception inside run_suite must give exit code 3, got %d", rc);
        CHECK(text.find("internal error: boom 3") != std::string::npos, "the exception must be logged");
        CHECK(R.layout.region == nullptr, "the region must be released on the exception path");
    }
}

// Barrier protocol (G1, see run.h): the helper that compares the workers' sync() counts, and parallel() itself in the
// two situations the debug check (DDR5_CHECK_SYNC) must tell apart. A run whose workers all make the same calls must
// pass; a run that is stopped may let workers leave at different points and must NOT be reported. The violation itself
// ends in std::abort, so it cannot be tested here: selftest.sh does it with the DDR5TEST_EXTRA_SYNC hook.
static void test_sync_protocol() {
    CHECK(sync_count_mismatch({}) == -1 && sync_count_mismatch({7}) == -1 && sync_count_mismatch({3, 3, 3}) == -1,
          "equal counts (or no workers) are no mismatch");
    CHECK(sync_count_mismatch({3, 3, 4}) == 2 && sync_count_mismatch({3, 2, 3}) == 1 &&
              sync_count_mismatch({0, 1}) == 1,
          "the first worker that differs from worker 0 is reported");
    Logger lg;
    lg.sink = [](const std::string&) {
    };
    { // every worker makes the same 5 calls: nothing to report, and the barrier must not hang
        Run R(lg);
        R.layout.T = 3;
        std::atomic<int> done{0};
        R.parallel([&](int) {
            for (int i = 0; i < 5; i++)
                R.sync();
            done++;
        });
        CHECK(done == 3 && !R.stop.load(), "an unstopped run with equal sync counts must just finish (%d done)",
              done.load());
    }
    { // worker 0 stops the run and leaves at once; the others still make two sync() calls. Rule 2: allowed, not
      // reported
        Run R(lg);
        R.layout.T = 3;
        std::atomic<int> passed{0};
        R.parallel([&](int tid) {
            if (tid == 0) {
                R.stop = true;
                return;
            }
            R.sync();
            R.sync();
            passed++;
        });
        CHECK(passed == 2 && R.stop.load(),
              "workers of a stopped run may make different numbers of sync() calls (%d passed)", passed.load());
    }
}

// Locking the region in slices (B1, platform.h). First the slicing logic with a fake lock function: the slices must
// tile [0, bytes) exactly, Stop must end it before the next slice, and a failure must end it too. Then plat::alloc
// itself with Stop already raised (the real mlock / VirtualLock path): it must return usable memory, not locked, saying
// why. A lock of many GB that is cut short in the middle cannot be run here, so the fake lock function stands in for
// it.
static void test_lock_slices() {
    using plat::lock_in_slices;
    using plat::LockProgress;
    struct Call {
        u64 off, n;
    };
    // (bytes, slice): exact multiple, short last slice, smaller than one slice, one byte, slice 0 = one slice, nothing
    const u64 cases[][2] = {{1200, 300}, {1000, 300}, {100, 300}, {1, 1}, {1000, 0}, {0, 300}};
    for (const auto& c : cases) {
        const u64 bytes = c[0], slice = c[1] ? c[1] : c[0];
        std::vector<u8> mem(bytes + 1);
        std::vector<Call> calls;
        const LockProgress r = lock_in_slices(mem.data(), bytes, nullptr, c[1], [&](u8* p, u64 n) {
            calls.push_back({(u64)(p - mem.data()), n});
            return true;
        });
        u64 next = 0;
        bool tiled = true;
        for (const Call& k : calls) {
            tiled = tiled && k.off == next && k.n >= 1 && k.n <= slice;
            next += k.n;
        }
        const u64 want = bytes ? (bytes + slice - 1) / slice : 0;
        CHECK(tiled && next == bytes && calls.size() == want && r.done == bytes && !r.failed && !r.stopped,
              "bytes=%llu slice=%llu: %zu calls (want %llu), covered %llu, done %llu, tiled=%d",
              (unsigned long long)bytes, (unsigned long long)c[1], calls.size(), (unsigned long long)want,
              (unsigned long long)next, (unsigned long long)r.done, (int)tiled);
    }
    std::vector<u8> mem(1000);
    { // Stop raised during the 2nd slice: that slice is finished, the 3rd is never started
        std::atomic<bool> stop{false};
        int calls = 0;
        const LockProgress r = lock_in_slices(mem.data(), 1000, &stop, 300, [&](u8*, u64) {
            if (++calls == 2)
                stop = true;
            return true;
        });
        CHECK(calls == 2 && r.done == 600 && r.stopped && !r.failed, "stop in slice 2: %d calls, done %llu, stopped=%d",
              calls, (unsigned long long)r.done, (int)r.stopped);
    }
    { // Stop already raised: nothing is locked
        std::atomic<bool> stop{true};
        int calls = 0;
        const LockProgress r = lock_in_slices(mem.data(), 1000, &stop, 300, [&](u8*, u64) {
            calls++;
            return true;
        });
        CHECK(calls == 0 && r.done == 0 && r.stopped && !r.failed, "stop before the first slice: %d calls", calls);
    }
    { // Stop raised by the LAST slice: everything is locked, so the run was not cut short
        std::atomic<bool> stop{false};
        const LockProgress r = lock_in_slices(mem.data(), 1000, &stop, 250, [&](u8* p, u64) {
            if (p == mem.data() + 750)
                stop = true;
            return true;
        });
        CHECK(r.done == 1000 && !r.stopped && !r.failed, "stop in the last slice: done %llu, stopped=%d",
              (unsigned long long)r.done, (int)r.stopped);
    }
    { // the 3rd slice fails: the first two stay locked and are reported, nothing after the failure is attempted
        int calls = 0;
        const LockProgress r = lock_in_slices(mem.data(), 1000, nullptr, 300, [&](u8*, u64) { return ++calls != 3; });
        CHECK(calls == 3 && r.done == 600 && r.failed && !r.stopped, "failure in slice 3: %d calls, done %llu", calls,
              (unsigned long long)r.done);
    }
    const u64 bytes = 16ull << 20;
    { // the real allocation with Stop already raised: usable memory back, not locked, and the note says why
        std::atomic<bool> stop{true};
        bool got = true, locked = true;
        std::string note;
        void* p = plat::alloc(bytes, false, got, locked, note, &stop);
        CHECK(p != nullptr && !got && !locked && note.find("stopped") != std::string::npos,
              "alloc with stop raised: p=%p got=%d locked=%d note='%s'", p, (int)got, (int)locked, note.c_str());
        if (p) {
            memset(p, 0xA5, bytes);
            plat::release(p, bytes);
        }
    }
    { // and without it (no limit on locked memory is promised here): it either locks or explains why it did not
        std::atomic<bool> stop{false};
        bool got = true, locked = false;
        std::string note;
        void* p = plat::alloc(bytes, false, got, locked, note, &stop);
        CHECK(p != nullptr && !got && (locked || !note.empty()) && note.find("stopped") == std::string::npos,
              "alloc without stop: p=%p got=%d locked=%d note='%s'", p, (int)got, (int)locked, note.c_str());
        if (p) {
            memset(p, 0x5A, bytes);
            plat::release(p, bytes);
        }
    }
}

// Memory sizing and allocation retry (C3, sizing.h): the floor left for the OS, the commit cap, the per-thread split,
// and the retry sequence when the OS refuses an allocation.
static void test_sizing() {
    using namespace sizing;
    const u64 GB = 1024 * kMB;
    // automatic size: 90% of the free memory while that leaves more than the floor, otherwise free memory - floor
    CHECK(auto_bytes(64 * GB) == 64 * GB / 100 * 90, "64 GB free: 90%%");
    CHECK(auto_bytes(2 * GB) == 2 * GB - kAutoFloor, "2 GB free: floor binds, got %llu MB",
          (unsigned long long)(auto_bytes(2 * GB) >> 20));
    CHECK(auto_bytes(kAutoFloor) == 0 && auto_bytes(100 * kMB) == 0 && auto_bytes(0) == 0, "nothing above the floor");
    // plan(): a request is kept when it fits, capped by the free memory or the commit limit otherwise
    Plan p = plan(4 * GB, 16 * GB, 0);
    CHECK(p.bytes == 4 * GB && p.limited == Limit::None, "request that fits is kept");
    p = plan(16 * GB, 16 * GB, 0);
    CHECK(p.bytes == 16 * GB / 100 * kCapPercent && p.limited == Limit::Free,
          "all of the free memory: 97%% cap binds on a big machine, got %llu MB", (unsigned long long)(p.bytes >> 20));
    p = plan(100 * GB, 8 * GB, 0);
    CHECK(p.bytes == 8 * GB - kCapFloor && p.limited == Limit::Free, "request above free memory is capped");
    p = plan(0, 16 * GB, 0);
    CHECK(p.bytes == auto_bytes(16 * GB) && p.limited == Limit::None, "automatic size is not 'limited'");
    p = plan(12 * GB, 16 * GB, 6 * GB);
    CHECK(p.bytes == 6 * GB - kCommitMargin && p.limited == Limit::Commit, "commit limit caps the request: %llu MB",
          (unsigned long long)(p.bytes >> 20));
    p = plan(0, 16 * GB, 3 * GB);
    CHECK(p.bytes == 3 * GB - kCommitMargin && p.limited == Limit::Commit, "commit limit caps the automatic size");
    p = plan(2 * GB, 16 * GB, 64 * GB);
    CHECK(p.bytes == 2 * GB && p.limited == Limit::None, "a roomy commit limit changes nothing");
    p = plan(4 * GB, 0, 0);
    CHECK(p.bytes == 4 * GB && p.limited == Limit::None, "free memory unknown: the request is used as it is");
    p = plan(4 * GB, 0, 2 * GB);
    CHECK(p.bytes == 2 * GB - kCommitMargin && p.limited == Limit::Commit, "free memory unknown, commit known");
    p = plan(0, 0, 0);
    CHECK(p.bytes == 0, "no request and nothing known: no size");
    // the invariant the Windows working-set request relies on: size + 256 MB fits in the free memory
    for (u64 avail : {300 * kMB, 512 * kMB, GB, 3 * GB, 8 * GB, 64 * GB}) {
        const u64 a = plan(0, avail, 0).bytes, b = plan(1000 * GB, avail, 0).bytes;
        CHECK(a + kCapFloor <= avail && b + kCapFloor <= avail, "avail %llu MB: auto %llu MB, capped %llu MB",
              (unsigned long long)(avail >> 20), (unsigned long long)(a >> 20), (unsigned long long)(b >> 20));
    }
    // per-thread split: whole 2 MB pieces, never more than asked
    CHECK(per_thread(1000 * kMB, 3) == 332 * kMB, "1000 MB over 3 threads: %llu MB",
          (unsigned long long)(per_thread(1000 * kMB, 3) >> 20));
    CHECK(per_thread(3 * kMB, 1) == 2 * kMB && per_thread(3 * kMB, 2) == 0 && per_thread(GB, 0) == GB,
          "small and zero-thread cases");
    // retry sizes: 90%, 80%, ... 20% of the first size in whole pieces, then none
    {
        const u64 first = 500 * kMB;
        u64 prev = first;
        int n = 0;
        for (int step = 1; step <= 12; step++) {
            const u64 r = retry_per(first, step);
            if (!r)
                break;
            CHECK(r < prev && r % kPiece == 0 && r >= first / 100 * kRetryMinPercent - kPiece,
                  "step %d: %llu MB after %llu MB", step, (unsigned long long)(r >> 20),
                  (unsigned long long)(prev >> 20));
            prev = r;
            n++;
        }
        CHECK(n == 8 && retry_per(first, 9) == 0 && retry_per(first, 0) == 0,
              "8 retries from 90%% down to 20%%, got %d", n);
        CHECK(retry_per(first, 1) == 450 * kMB && retry_per(first, 8) == 100 * kMB, "first and last retry size");
    }
    // alloc_with_retry with a fake allocator that refuses everything above a limit
    auto run = [&](u64 perThread, int threads, u64 limit, const std::atomic<bool>* stop, std::vector<u64>& tried,
                   std::vector<std::pair<u64, u64>>& logged) {
        return alloc_with_retry(
            perThread, threads, stop,
            [&](u64 bytes) -> void* {
                tried.push_back(bytes);
                return bytes <= limit ? (void*)0x1000 : nullptr;
            },
            [&](u64 failed, u64 next) { logged.push_back({failed, next}); });
    };
    std::vector<u64> tried;
    std::vector<std::pair<u64, u64>> logged;
    AllocOutcome o = run(500 * kMB, 2, 10000 * kMB, nullptr, tried, logged);
    CHECK(o.p && o.attempts == 1 && o.per == 500 * kMB && tried.size() == 1 && logged.empty(),
          "success at the first size: no retry, no log");
    tried.clear();
    logged.clear();
    o = run(500 * kMB, 2, 700 * kMB, nullptr, tried, logged); // 1000, 900, 800 refused; 700 = 350 MB x 2 ... see below
    // tries (total bytes, 2 threads): 1000, 900, 800 MB refused, 700 MB (= 350 MB per thread) accepted
    CHECK(o.p && o.attempts == 4 && o.per == 350 * kMB && tried.size() == 4 && tried[0] == 1000 * kMB &&
              tried[1] == 900 * kMB && tried[2] == 800 * kMB && tried[3] == 700 * kMB,
          "retry until it fits: %d attempts, per thread %llu MB", o.attempts, (unsigned long long)(o.per >> 20));
    CHECK(logged.size() == 3 && logged[0].first == 1000 * kMB && logged[0].second == 900 * kMB &&
              logged[2].first == 800 * kMB && logged[2].second == 700 * kMB,
          "one log call per retry, with the failed and the next size (%zu calls)", logged.size());
    tried.clear();
    logged.clear();
    o = run(500 * kMB, 2, 100 * kMB, nullptr, tried, logged); // even 20% (200 MB total) is refused
    CHECK(!o.p && o.attempts == 9 && tried.size() == 9 && tried.back() == 200 * kMB && logged.size() == 8,
          "gives up after 8 retries at 20%%: %d attempts, last %llu MB", o.attempts,
          (unsigned long long)(tried.empty() ? 0 : tried.back() >> 20));
    CHECK(o.per == 100 * kMB, "the outcome reports the last size tried");
    tried.clear();
    logged.clear();
    { // Stop raised during the first refusal: no retry (the user wants out, not a smaller test)
        std::atomic<bool> stop{false};
        int calls = 0;
        o = alloc_with_retry(
            500 * kMB, 2, &stop,
            [&](u64) -> void* {
                calls++;
                stop = true;
                return nullptr;
            },
            [&](u64, u64) { calls += 100; });
        CHECK(!o.p && o.attempts == 1 && calls == 1, "stop ends the retrying: %d attempts, calls %d", o.attempts,
              calls);
    }
    { // small sizes: steps that round to the same size are skipped, and the sequence stays strictly decreasing
        tried.clear();
        logged.clear();
        o = run(4 * kMB, 1, 0, nullptr, tried, logged);
        bool dec = true;
        for (size_t i = 1; i < tried.size(); i++)
            dec = dec && tried[i] < tried[i - 1] && tried[i] % kPiece == 0 && tried[i] > 0;
        CHECK(!o.p && dec && tried.front() == 4 * kMB && tried.back() == 2 * kMB && tried.size() == 2,
              "4 MB over 1 thread: %zu sizes, last %llu MB", tried.size(),
              (unsigned long long)(tried.empty() ? 0 : tried.back() >> 20));
    }
    { // nothing to allocate: the allocator is not called
        tried.clear();
        logged.clear();
        o = run(0, 2, 10000 * kMB, nullptr, tried, logged);
        CHECK(!o.p && tried.empty() && logged.empty(), "per = 0: no call");
    }
}

// FPU stress kernel (D6). Left alone, x = x * m + k grows by ~1.2e-7 per step and a register reaches +inf after ~8e8
// steps
// (~40,100 outer loops), after which it never changes again. The kernel re-seeds the registers every kFpuReseedLoops
// outer loops, so the whole run is periodic: if two and a half periods stay finite, bounded and changing on EVERY outer
// loop, and the state is back at the start after each full period, an unbounded run does too (a fast proof; running
// past 40,100 loops would take seconds).
static void test_fpu_kernel() {
    FpuKernel K;
    float start[8][8], prev[8][8];
    memcpy(start, K.v, sizeof start);
    bool finite = true, changing = true, periodic = true;
    float maxv = 0;
    for (u64 n = 1; n <= 2 * kFpuReseedLoops + kFpuReseedLoops / 2; n++) {
        memcpy(prev, K.v, sizeof prev);
        K.run(1);
        for (int i = 0; i < 8; i++) {
            if (K.v[i][0] == prev[i][0])
                changing = false; // register i did not change in a whole outer loop
            for (int l = 0; l < 8; l++) {
                if (!std::isfinite(K.v[i][l]))
                    finite = false;
                maxv = std::max(maxv, std::fabs(K.v[i][l]));
            }
        }
        if (n % kFpuReseedLoops == 0 && memcmp(K.v, start, sizeof start) != 0)
            periodic = false;
    }
    CHECK(finite, "an FPU register left the finite range");
    CHECK(changing, "an FPU register stopped changing");
    CHECK(maxv < 100.0f, "FPU values must stay small (max %g)", (double)maxv);
    CHECK(periodic, "the kernel must be back at its start state after kFpuReseedLoops outer loops");
    CHECK(std::isfinite(K.sink()) && K.sink() > 0.0f, "FpuKernel::sink() must be a finite positive number");
    FpuKernel A, B; // run(a) + run(b) is the same as run(a + b): the state really is carried in v
    A.run(7);
    B.run(3);
    B.run(4);
    CHECK(memcmp(A.v, B.v, sizeof A.v) == 0 && A.loops == B.loops, "run() must carry its state between calls");
}

// Run lifecycle seen from the front-end side: status() may be polled from another thread while run_suite works (the GUI
// does exactly this; run it under TSan, see S12), and a Stop that arrives before the run starts must not be lost.
static void test_run_status() {
    Settings st;
    st.megabytes = 8;
    st.threads = 2;
    st.largePages = false;
    st.filter = "Bulk copy";
    st.loops = 1; // 0 would mean "until stopped"
    st.testSeconds = 1;
    st.seedSet = true;
    st.seed = 1;
    std::mutex mu;
    std::string text;
    Logger lg;
    lg.sink = [&](const std::string& t) {
        std::lock_guard<std::mutex> l(mu);
        text += t;
    };
    {
        Run R(lg);
        std::atomic<bool> done{false};
        int rc = -1;
        std::thread worker([&] {
            rc = run_suite(st, R);
            done = true;
        });
        bool sawRegion = false, sawStart = false;
        u64 lastV = 0;
        while (!done) {
            Run::Status s = R.status();
            sawRegion = sawRegion || s.regionBytes > 0;
            sawStart = sawStart || s.tStart > 0;
            CHECK(s.vtotal >= lastV, "coverage counter went backwards");
            lastV = s.vtotal;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        worker.join();
        Run::Status f = R.status();
        CHECK(rc == 0, "run_suite on a clean run must return 0, got %d", rc);
        CHECK(f.regionBytes == (8ull << 20) && f.tStart > 0 && f.errors == 0,
              "final status wrong: region %llu start %.2f errors %llu", (unsigned long long)f.regionBytes, f.tStart,
              (unsigned long long)f.errors);
        CHECK(sawRegion || sawStart || f.regionBytes, "status never showed the run");
    }
    {
        text.clear();
        Run R(lg);
        R.stop = true; // Stop pressed while the run is still starting up
        int rc = run_suite(st, R);
        CHECK(rc == 0, "run_suite with stop already set must return 0, got %d", rc);
        CHECK(text.find("[loop") == std::string::npos, "a Stop requested before the start was lost: a test ran");
    }
}

// Run::report and the Logger formatting helpers. The error block is built in a string that grows to fit (a fixed buffer
// would cut a long test name and lose the qword lines after it, or overrun), and once the log holds maxLog blocks
// report() still counts every error but writes nothing more.
static void test_report() {
    // appendf / emitf: output longer than any fixed buffer arrives whole
    {
        std::string s = "head:";
        appendf(s, "%s|%d", std::string(5000, 'q').c_str(), 42);
        CHECK(s == "head:" + std::string(5000, 'q') + "|42", "appendf lost or cut text (%zu bytes)", s.size());
        std::string small;
        appendf(small, "%d-%s", 7, "ab");
        CHECK(small == "7-ab", "appendf short output wrong: '%s'", small.c_str());
        std::string got;
        Logger lg;
        lg.sink = [&](const std::string& t) {
            got = t;
        };
        lg.emitf("[%s]", std::string(3000, 'z').c_str());
        CHECK(got == "[" + std::string(3000, 'z') + "]", "emitf cut a long message (%zu bytes)", got.size());
    }
    alignas(64) static u8 mem[64 * 8];
    Line e, a; // worst case: every qword differs, in every bit
    for (int j = 0; j < 8; j++) {
        e.w[j] = 0x0123456789ABCDEFull * (u64)(j + 1);
        a.w[j] = ~e.w[j];
    }
    // one error with a very long test name: the whole header and all 8 qword lines must be there, well formed
    {
        std::vector<std::string> blocks;
        Logger lg;
        lg.sink = [&](const std::string& t) {
            blocks.push_back(t);
        };
        Run R(lg);
        R.layout.region = mem;
        const std::string name(3000, 'n');
        R.set_label(name);
        R.report(2, mem + 64, e, a, "verify", false);
        CHECK(blocks.size() == 1, "one error must give one block, got %zu", blocks.size());
        if (blocks.size() == 1) {
            const std::string& b = blocks[0];
            std::vector<std::string> lines;
            std::stringstream ss(b);
            for (std::string l; std::getline(ss, l);)
                lines.push_back(l);
            CHECK(!b.empty() && b.back() == '\n', "the block must end with a newline");
            CHECK(lines.size() == 9, "header + 8 qword lines expected, got %zu lines", lines.size());
            CHECK(!lines.empty() && lines[0].rfind("ERROR t=", 0) == 0 &&
                      lines[0].find("test=\"" + name + "\" verify thr=2 ") != std::string::npos &&
                      lines[0].find("bits=512 ") != std::string::npos &&
                      lines[0].find("x8-codewords[1b/2b/3-4b/5+b]=0/0/0/4 (logical check") != std::string::npos,
                  "header cut or malformed: '%.160s'", lines.empty() ? "" : lines[0].c_str());
            for (int j = 0; j < 8 && (size_t)j + 1 < lines.size(); j++) {
                char want[128];
                snprintf(want, sizeof want, "      qword %d: exp=%016llX act=%016llX xor=%016llX", j,
                         (unsigned long long)e.w[j], (unsigned long long)a.w[j], (unsigned long long)~0ull);
                CHECK(lines[(size_t)j + 1] == want, "qword line %d wrong: '%s'", j, lines[(size_t)j + 1].c_str());
            }
        }
        CHECK(R.error_count() == 1, "error_count %llu", (unsigned long long)R.error_count());
    }
    // 4 threads report 100 errors each: every error is counted, exactly maxLog blocks are written, none interleaved
    {
        const u64 maxLog = Cfg().maxLog;
        std::vector<std::string> blocks; // the sink runs under the Run's lock, so it needs none of its own
        Logger lg;
        lg.sink = [&](const std::string& t) {
            blocks.push_back(t);
        };
        Run R(lg);
        R.layout.region = mem;
        R.set_label("report storm");
        R.layout.T = 4;
        R.parallel([&](int tid) {
            for (int i = 0; i < 100; i++)
                R.report(tid, mem + 64 * (size_t)((tid + i) % 8), e, a, "verify", false);
        });
        CHECK(R.error_count() == 400, "every error must be counted, got %llu", (unsigned long long)R.error_count());
        CHECK(blocks.size() == maxLog, "exactly maxLog (%llu) blocks must be written, got %zu",
              (unsigned long long)maxLog, blocks.size());
        for (const std::string& b : blocks)
            if (b.rfind("ERROR t=", 0) != 0 || std::count(b.begin(), b.end(), '\n') != 9) {
                CHECK(false, "a block is malformed or interleaved: '%.120s'", b.c_str());
                break;
            }
    }
}

// try_start (GUI start_run, F2): a throwing step (no memory for the Run, the OS refusing a thread) is caught and
// reported, nothing escapes, and whatever the step built into locals before it threw is simply dropped.
static void test_start_guard() {
    std::string why = "stale";
    int ran = 0;
    CHECK(try_start([&] { ran++; }, why) && ran == 1 && why.empty(),
          "a step that works must return true, run exactly once and leave `why` empty (got '%s')", why.c_str());
    CHECK(
        !try_start(
            [] { throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again), "thread"); },
            why) &&
            why.find("thread") != std::string::npos,
        "a refused thread (std::system_error) must return false with its text in `why` (got '%s')", why.c_str());
    CHECK(!try_start([] { throw std::bad_alloc(); }, why) && !why.empty(),
          "bad_alloc must return false with a non-empty reason");
    CHECK(!try_start([] { throw 42; }, why) && why == "unknown error",
          "a non-std exception must return false with 'unknown error' (got '%s')", why.c_str());
    struct Silent : std::exception {
        const char* what() const noexcept override { return ""; }
    };
    CHECK(!try_start([] { throw Silent(); }, why) && why == "unknown error",
          "an exception with an empty text must still give a reason (got '%s')", why.c_str());
    CHECK(try_start([] {}, why) && why.empty(), "`why` must be cleared by a later successful start");
    { // the pattern of start_run: build into locals, commit only on success
        std::unique_ptr<int> run, committed;
        std::thread worker;
        const bool ok = try_start(
            [&] {
                run = std::make_unique<int>(7);
                worker = std::thread([] {});
                throw std::runtime_error("second step failed");
            },
            why);
        if (ok)
            committed = std::move(run);
        CHECK(!ok && !committed && why == "second step failed", "a failed start must not commit anything (got '%s')",
              why.c_str());
        if (worker.joinable())
            worker.join(); // a thread built before the throw must be joined by the caller, as start_run's locals are
    }
    { // success: what the step built is usable afterwards
        std::unique_ptr<int> run;
        std::thread worker;
        std::atomic<int> seen{0};
        const bool ok = try_start(
            [&] {
                run = std::make_unique<int>(7);
                const int* r = run.get();
                worker = std::thread([&seen, r] { seen = *r; });
            },
            why);
        CHECK(ok && worker.joinable(), "a successful start must leave a joinable worker in the caller's local");
        if (worker.joinable())
            worker.join();
        CHECK(seen == 7, "the worker must have run on the Run built by the step (saw %d)", seen.load());
    }
}

// StopGate (GUI Stop / close rules, B2): the UI never waits for the worker, a second click only forces after the delay,
// close waits for the run's done message.
static void test_stop_gate() {
    using Act = StopGate::Act;
    { // idle: close goes straight through, Stop does nothing (the GUI starts a run instead)
        StopGate g(3.0);
        CHECK(g.on_close(0) == Act::CloseNow, "close with no run must destroy the window");
        CHECK(g.on_stop_click(0) == Act::None, "Stop with no run must do nothing");
        CHECK(!g.tick(100), "no force offer without a run");
    }
    { // Stop: the first click asks, the second one within the delay is ignored, after the delay it forces
        StopGate g(3.0);
        g.on_run_start();
        CHECK(g.running() && !g.stopping(), "state after start");
        CHECK(g.on_stop_click(10) == Act::RequestStop && g.stopping(), "first Stop click must request the stop");
        CHECK(g.on_stop_click(10.5) == Act::None, "second click inside the delay must be ignored (double click)");
        CHECK(!g.tick(12.9) && !g.force_offered(), "no force offer before the delay");
        CHECK(g.on_stop_click(12.9) == Act::None, "still no force before the delay");
        CHECK(g.tick(13.0) && g.force_offered(), "force offered once the delay has passed");
        CHECK(!g.tick(14.0), "the offer is reported exactly once");
        CHECK(g.on_stop_click(14.0) == Act::ForceExit, "click on Force quit must end the process");
        CHECK(g.on_run_done() == Act::None && !g.running() && !g.stopping(),
              "done without a close request: nothing to do");
    }
    { // close while running: ask for the stop, keep the window, destroy it only when the worker is done
        StopGate g(3.0);
        g.on_run_start();
        CHECK(g.on_close(5) == Act::RequestStop && g.closing() && g.stopping(),
              "close during a run must request the stop, not close");
        CHECK(g.on_close(6) == Act::None, "second close inside the delay is ignored");
        CHECK(g.on_run_done() == Act::CloseNow, "window must close when the run has ended");
    }
    { // second close after the delay forces
        StopGate g(3.0);
        g.on_run_start();
        CHECK(g.on_close(0) == Act::RequestStop, "first close");
        CHECK(g.tick(3.5), "force offered");
        CHECK(g.on_close(4) == Act::ForceExit, "second close after the delay must end the process");
    }
    { // Stop first, then close: the close joins the pending stop (no second request) and closes at the end
        StopGate g(3.0);
        g.on_run_start();
        CHECK(g.on_stop_click(1) == Act::RequestStop, "stop");
        CHECK(g.on_close(2) == Act::None && g.closing(), "close after Stop: no second request, remembered");
        CHECK(g.on_run_done() == Act::CloseNow, "closes when done");
    }
    { // the run ends on its own (or stop-on-error) before any click: a later close is immediate; a new run starts clean
        StopGate g(3.0);
        g.on_run_start();
        CHECK(g.on_run_done() == Act::None, "run ended by itself");
        CHECK(g.on_close(0) == Act::CloseNow, "close after the run ended");
        g.on_run_start();
        CHECK(!g.stopping() && !g.closing() && !g.force_offered(), "a new run must start with a clean state");
    }
    { // the delay counts from the stop request, not from the start of the run
        StopGate g(3.0);
        g.on_run_start();
        CHECK(!g.tick(1000), "no offer while not stopping, however long the run has been going");
        CHECK(g.on_stop_click(1000) == Act::RequestStop, "stop late in the run");
        CHECK(!g.tick(1001) && g.tick(1003), "offer comes 3 s after the stop request");
    }
}

// plat::keep_awake (C1): POSIX has nothing to do; on Windows the request is really on the calling thread and clearing
// removes it.
// The allowed-CPU set (cpu_set.h) that decides which CPU thread N is pinned to, and the real ncpu() / pin() of this
// platform.
static void test_cpu_set() {
    using namespace cpuset;
    CHECK(low_bits(0) == 0 && low_bits(1) == 1 && low_bits(63) == (~0ull >> 1) && low_bits(64) == ~0ull &&
              low_bits(70) == ~0ull,
          "low_bits must cover 0..64 and not shift by 64 or more");
    // expand: group order, then bit order, gaps skipped
    {
        const std::vector<CpuRef> v = expand({GroupMask{0, 0b1010}, GroupMask{1, 0b101}});
        const std::vector<CpuRef> want = {CpuRef{0, 1}, CpuRef{0, 3}, CpuRef{1, 0}, CpuRef{1, 2}};
        CHECK(v == want, "expand must list (group, bit) in group order and bit order, skipping unset bits");
        CHECK(expand({}).empty() && expand({GroupMask{3, 0}}).empty(), "no groups / an empty mask give no CPUs");
        CHECK(expand({GroupMask{2, ~0ull}}).size() == 64 && expand({GroupMask{2, ~0ull}}).back() == (CpuRef{2, 63}),
              "a full group holds 64 CPUs, the last one is bit 63");
    }
    // restrict_to: groups missing from the limit are dropped, the rest are intersected, empty results vanish
    {
        const std::vector<GroupMask> a = {GroupMask{0, 0xFF}, GroupMask{1, 0xF0}, GroupMask{2, 0x3}};
        const std::vector<GroupMask> r = restrict_to(a, {GroupMask{0, 0x0F}, GroupMask{1, 0x0F}});
        CHECK(
            r.size() == 1 && r[0].group == 0 && r[0].mask == 0x0F,
            "group 2 is not in the limit, group 1 intersects to nothing: only group 0 (0x0F) is left (got %zu groups)",
            r.size());
        const std::vector<GroupMask> same =
            restrict_to(a, {GroupMask{0, ~0ull}, GroupMask{1, ~0ull}, GroupMask{2, ~0ull}});
        CHECK(same.size() == 3 && same[0].mask == 0xFF && same[1].mask == 0xF0 && same[2].mask == 0x3,
              "a limit that allows everything changes nothing");
        CHECK(restrict_to(a, {}).empty(), "an empty limit leaves nothing (the caller must not believe that)");
        const std::vector<GroupMask> twice =
            restrict_to({GroupMask{0, 0xFF}}, {GroupMask{0, 0x0F}, GroupMask{0, 0x30}});
        CHECK(twice.size() == 1 && twice[0].mask == 0x3F,
              "two limit entries for one group add up before the intersection");
    }
    // nth: the idx-th CPU, wrapping; no answer for an empty set or a negative index
    {
        const std::vector<CpuRef> v = expand({GroupMask{0, 0b110}, GroupMask{1, 0b1}}); // (0,1) (0,2) (1,0)
        CHECK(*nth(v, 0) == (CpuRef{0, 1}) && *nth(v, 1) == (CpuRef{0, 2}) && *nth(v, 2) == (CpuRef{1, 0}),
              "thread 0, 1, 2 get the 1st, 2nd, 3rd allowed CPU, not CPU numbers 0, 1, 2");
        CHECK(*nth(v, 3) == (CpuRef{0, 1}) && *nth(v, 7) == (CpuRef{0, 2}), "more threads than CPUs wrap round");
        CHECK(nth(v, -1) == nullptr && nth({}, 0) == nullptr, "negative index and empty set give no CPU");
    }
    // The real platform: ncpu() is at least 1, and every thread number up to twice that (wrapping) can be pinned. Done
    // in a thread of its own so the main thread keeps its affinity.
    {
        const int n = plat::ncpu();
        CHECK(n >= 1, "ncpu() must be at least 1 (got %d)", n);
        bool all = true, neg = true;
        std::thread th([&] {
            for (int i = 0; i < 2 * n && all; i++)
                all = plat::pin(i);
            neg = plat::pin(-1);
        });
        th.join();
        CHECK(all, "pin(i) must succeed for i < 2 * ncpu(): the cpu-th allowed CPU, wrapping round");
        CHECK(!neg, "pin(-1) must report not pinned");
    }
}
static void test_keep_awake() {
#ifdef _WIN32
    CHECK(plat::keep_awake(true) == plat::KeepAwake::On, "keep_awake(true) must be accepted by the OS");
    // SetThreadExecutionState returns the state that was in force before the call; the probe re-applies the same state
    EXECUTION_STATE st = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    CHECK((st & ES_SYSTEM_REQUIRED) != 0, "ES_SYSTEM_REQUIRED must be set after keep_awake(true) (state 0x%lx)",
          (unsigned long)st);
    plat::keep_awake(false);
    st = SetThreadExecutionState(ES_CONTINUOUS); // probe: looks at the cleared state and leaves it cleared
    CHECK((st & ES_SYSTEM_REQUIRED) == 0, "ES_SYSTEM_REQUIRED must be gone after keep_awake(false) (state 0x%lx)",
          (unsigned long)st);
#else
    CHECK(plat::keep_awake(true) == plat::KeepAwake::NotSupported, "POSIX has nothing to keep awake");
    plat::keep_awake(false);
#endif
}

// CoverageMap (E2a): one bit per whole 4 KiB page, a page is marked only when the range covers all of it.
static void test_coverage_map() {
    const u64 P = kPageBytes;
    { // sizing: the tail that is not a whole page counts neither as a page nor in the total
        CoverageMap m;
        m.reset(10 * P + 100);
        CHECK(m.pages_total() == 10 && m.pages_covered() == 0 && m.percent() == 0.0, "reset: total=%llu covered=%llu",
              (unsigned long long)m.pages_total(), (unsigned long long)m.pages_covered());
        m.reset(P - 1);
        CHECK(m.pages_total() == 0 && m.percent() == 0.0 && m.first_unmarked() == 0, "region below one page");
        m.mark(0, P); // must not write anywhere
        CHECK(m.pages_covered() == 0, "mark on an empty map must do nothing");
        m.reset(0);
        m.mark(0, 1000 * P);
        CHECK(m.pages_total() == 0 && m.pages_covered() == 0 && !m.marked(0), "reset(0)");
    }
    { // exactly one page, and its neighbours stay clear
        CoverageMap m;
        m.reset(10 * P);
        m.mark(3 * P, P);
        CHECK(m.pages_covered() == 1 && m.marked(3) && !m.marked(2) && !m.marked(4), "one exact page");
        CHECK(m.percent() == 10.0, "1 of 10 pages is 10 percent, got %f", m.percent());
    }
    { // partial pages at either end are not marked
        CoverageMap m;
        m.reset(10 * P);
        m.mark(P + 1, P); // [4097, 8193): neither page 1 nor page 2 is complete
        CHECK(m.pages_covered() == 0, "a range inside two pages marks none, got %llu",
              (unsigned long long)m.pages_covered());
        m.mark(100, 3 * P); // [100, 12388): pages 1 and 2 are complete, page 0 and 3 are not
        CHECK(m.pages_covered() == 2 && m.marked(1) && m.marked(2) && !m.marked(0) && !m.marked(3),
              "unaligned start and end: covered=%llu", (unsigned long long)m.pages_covered());
        m.mark(5 * P, P - 1); // one byte short of a page
        CHECK(!m.marked(5), "a page missing its last byte must stay unmarked");
        m.mark(5 * P + 1, P); // starts one byte late
        CHECK(!m.marked(5) && !m.marked(6), "a page missing its first byte must stay unmarked");
    }
    { // empty, out-of-range and wrapping ranges
        CoverageMap m;
        m.reset(10 * P);
        m.mark(2 * P, 0);
        m.mark(10 * P, P);
        m.mark((u64)1 << 40, P);
        m.mark(~(u64)0 - 5, 100); // offset + bytes wraps around
        CHECK(m.pages_covered() == 0, "empty / past-the-end ranges must mark nothing, got %llu",
              (unsigned long long)m.pages_covered());
        m.mark(8 * P, 100 * P); // reaches past the end: cut there
        CHECK(m.pages_covered() == 2 && m.marked(8) && m.marked(9), "range past the end is cut at the region end");
        m.mark(0, ~(u64)0); // the whole address space
        CHECK(m.pages_covered() == 10 && m.percent() == 100.0 && m.first_unmarked() == 10, "mark everything");
        m.reset(10 * P);
        CHECK(m.pages_covered() == 0, "reset must clear every bit");
    }
    { // ranges that cross the 64-bit word edge, and the last page of the region
        CoverageMap m;
        m.reset(130 * P); // 3 words, the last holds 2 pages
        m.mark(60 * P, 10 * P);
        CHECK(m.pages_covered() == 10, "range across a word edge: covered=%llu", (unsigned long long)m.pages_covered());
        CHECK(!m.marked(59) && m.marked(60) && m.marked(63) && m.marked(64) && m.marked(69) && !m.marked(70),
              "word-edge range has wrong bits");
        m.mark(128 * P, 2 * P);
        CHECK(m.marked(128) && m.marked(129) && m.pages_covered() == 12, "last two pages");
        m.mark(0, 130 * P);
        CHECK(m.pages_covered() == 130 && m.percent() == 100.0, "every page of 130 marked once: %llu",
              (unsigned long long)m.pages_covered());
        m.mark(0, 130 * P); // again: still 130, never counted twice
        m.mark(60 * P, 10 * P);
        CHECK(m.pages_covered() == 130, "re-marking must be idempotent, got %llu",
              (unsigned long long)m.pages_covered());
    }
    { // first_unmarked
        CoverageMap m;
        m.reset(200 * P);
        CHECK(m.first_unmarked() == 0, "empty map: first unmarked is page 0");
        m.mark(0, 77 * P);
        m.mark(78 * P, 122 * P);
        CHECK(m.pages_covered() == 199 && m.first_unmarked() == 77, "hole at page 77, got %llu",
              (unsigned long long)m.first_unmarked());
        CHECK(m.first_unmarked(78) == 200 && m.first_unmarked(200) == 200 && m.first_unmarked(500) == 200,
              "nothing unmarked after the hole");
        m.mark(77 * P, P);
        CHECK(m.first_unmarked() == 200 && m.percent() == 100.0, "no hole left");
    }
    { // 4 threads marking interleaved single pages and overlapping ranges: the final count is exact
        CoverageMap m;
        const u64 n = 4096;
        m.reset(n * P);
        std::vector<std::thread> th;
        for (int t = 0; t < 4; t++)
            th.emplace_back([&m, t, n] {
                for (u64 p = (u64)t; p < n; p += 4) {
                    m.mark(p * P, P);
                    m.mark(p * P, 3 * P); // overlaps the next pages, which other threads set too
                }
            });
        for (auto& x : th)
            x.join();
        CHECK(m.pages_covered() == n && m.percent() == 100.0 && m.first_unmarked() == n,
              "threads: covered=%llu of %llu", (unsigned long long)m.pages_covered(), (unsigned long long)n);
    }
}

// for_pages_except: the maximal runs of pages that avoid the skipped ones.
static void test_pages_except() {
    auto runs = [](std::vector<u64> skip, u64 pages) {
        std::vector<std::pair<u64, u64>> r;
        for_pages_except(std::move(skip), pages, [&](u64 pg, u64 n) { r.push_back({pg, n}); });
        return r;
    };
    using V = std::vector<std::pair<u64, u64>>;
    CHECK(runs({}, 10) == V({{0, 10}}), "no skipped page: one run");
    CHECK(runs({}, 0).empty(), "no pages: no run");
    CHECK(runs({3}, 10) == V({{0, 3}, {4, 6}}), "one skipped page in the middle");
    CHECK(runs({0}, 4) == V({{1, 3}}) && runs({3}, 4) == V({{0, 3}}), "skipped page at either end");
    CHECK(runs({7, 2, 2, 5}, 10) == V({{0, 2}, {3, 2}, {6, 1}, {8, 2}}), "unsorted, duplicated skips");
    CHECK(runs({0, 1, 2, 3}, 4).empty(), "every page skipped: no run");
    CHECK(runs({99, 5}, 8) == V({{0, 5}, {6, 2}}), "a page number past the end is ignored");
}

// E2b: the algos mark the pages they really verified in Run::cover. A clean run of each algo alone covers every page of
// the region; a run stopped in the middle of its first verify covers fewer (a stopped job marks nothing).
static void test_coverage_hooks() {
    struct Out {
        u64 covered, total;
        int rc;
    };
    auto go = [](const char* filter, u64 mb, bool stopInVerify) {
        Settings st;
        st.megabytes = mb;
        st.threads = 2;
        st.largePages = false;
        st.filter = filter;
        st.loops = 1;
        st.testSeconds = 1;
        st.seedSet = true;
        st.seed = 7;
        Logger lg;
        lg.sink = [](const std::string&) {
        };
        Run R(lg);
        std::atomic<bool> done{false};
        std::thread watcher;
        if (stopInVerify) // vtotal moves for the first time after a thread has verified 64K lines: that is mid-verify
            watcher = std::thread([&] {
                while (!done && R.status().vtotal == 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                R.stop = true;
            });
        int rc = run_suite(st, R);
        done = true;
        if (watcher.joinable())
            watcher.join();
        return Out{R.cover.pages_covered(), R.cover.pages_total(), rc};
    };
    const struct {
        const char* name;
        const char* filter;
    } algos[] = {{"fill", "solid and checker"}, {"fill cross", "cross-core verify"}, {"march", "address-in-address"},
                 {"evolve", "evolving data"},   {"rwstorm", "storm, hash data"},     {"copy", "bulk copy"}};
    for (const auto& a : algos) {
        Out c = go(a.filter, 8, false);
        CHECK(c.rc == 0 && c.total == (8ull << 20) / kPageBytes && c.covered == c.total,
              "%s: a clean run must cover every page, got %llu of %llu (rc %d)", a.name, (unsigned long long)c.covered,
              (unsigned long long)c.total, c.rc);
        Out s = go(a.filter, 256, true);
        CHECK(s.total == (256ull << 20) / kPageBytes && s.covered < s.total,
              "%s: a run stopped in its first verify must not cover every page, got %llu of %llu", a.name,
              (unsigned long long)s.covered, (unsigned long long)s.total);
    }
    // Algos that are called directly on a hand-made region, so a test can set keys the suite does not (chase with 12
    // chains leaves w.n % 12 lines of the order unvisited; hammer is not in the built-in suite).
    // stopMode: 0 = run to the end, 1 = stop already set at the start, 2 = stop in the middle of the first verify
    auto direct = [](void (*algo)(Run&, const Params&), std::map<std::string, std::string> keys, int stopMode,
                     u64 mb = 8) {
        Logger lg;
        lg.sink = [](const std::string&) {
        };
        Run R(lg);
        const int T = 2;
        const u64 total = mb << 20, per = total / T;
        bool got = false, locked = false;
        std::string note;
        u8* mem = (u8*)plat::alloc(total, false, got, locked, note);
        Out o{0, 0, -1};
        if (!mem) {
            CHECK(false, "could not allocate the test region");
            return o;
        }
        std::memset(mem, 0, total);
        R.layout.T = T;
        R.layout.region = mem;
        for (int t = 0; t < T; t++)
            R.layout.chunks.push_back({mem + (u64)t * per, per / kLineBytes, (u64)t * (per / kLineBytes)});
        R.cover.reset(total);
        R.layout.regionBytes.store(total, std::memory_order_release);
        R.testName = "direct";
        Params P;
        P.m = std::move(keys);
        if (stopMode == 1)
            R.stop = true; // before the start: nothing may be marked
        std::atomic<bool> fin{false};
        std::thread watcher;
        if (stopMode == 2)
            watcher = std::thread([&] {
                while (!fin && R.status().vtotal == 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                R.stop = true;
            });
        algo(R, P);
        fin = true;
        if (watcher.joinable())
            watcher.join();
        o = Out{R.cover.pages_covered(), R.cover.pages_total(), 0};
        plat::release(mem, total);
        return o;
    };
    {
        // 65536 lines per chunk are not a multiple of 12: 4 lines per chunk (random places in it) are never loaded, so
        // with ONE chain layout their pages stay unmarked: 8 pages at most, 2 at least (some may share a page)
        Out c = direct(algo_chase, {{"mlp", "12"}, {"seeds", "1"}}, 0);
        CHECK(c.total == 2048 && c.covered < c.total && c.covered >= c.total - 8,
              "chase mlp=12: pages holding an unvisited line must stay unmarked, got %llu of %llu",
              (unsigned long long)c.covered, (unsigned long long)c.total);
        Out k = direct(algo_chase, {{"mlp", "8"}, {"seeds", "1"}}, 0);
        CHECK(k.covered == k.total, "chase mlp=8 visits every line, got %llu of %llu", (unsigned long long)k.covered,
              (unsigned long long)k.total);
        Out z = direct(algo_chase, {{"mlp", "12"}}, 1);
        CHECK(z.covered == 0, "chase with stop set marked %llu pages", (unsigned long long)z.covered);
        Out m = direct(algo_chase, {{"mlp", "12"}, {"seeds", "1"}}, 2, 256);
        CHECK(m.total == 65536 && m.covered < m.total / 2, "chase stopped mid-chain marked %llu of %llu pages",
              (unsigned long long)m.covered, (unsigned long long)m.total);
    }
    {
        Out c = direct(algo_hammer, {{"rounds", "1"}, {"acts", "1000"}}, 0);
        CHECK(c.total == 2048 && c.covered == c.total,
              "hammer: the victim check must cover every page, got %llu of %llu", (unsigned long long)c.covered,
              (unsigned long long)c.total);
        Out z = direct(algo_hammer, {{"rounds", "1"}, {"acts", "1000"}}, 1);
        CHECK(z.covered == 0, "hammer with stop set marked %llu pages", (unsigned long long)z.covered);
        Out m = direct(algo_hammer, {{"rounds", "1"}, {"acts", "1000"}}, 2, 256);
        CHECK(m.total == 65536 && m.covered < m.total / 2, "hammer stopped mid-verify marked %llu of %llu pages",
              (unsigned long long)m.covered, (unsigned long long)m.total);
    }
    { // pagemix with no copy / invert / refill phase (seconds=0, at most 63 operations): the final verify marks the
      // rest
        Out c = direct(algo_pagemix, {{"seconds", "0"}}, 0);
        CHECK(c.total == 2048 && c.covered == c.total, "pagemix final verify must cover every page, got %llu of %llu",
              (unsigned long long)c.covered, (unsigned long long)c.total);
        Out m = direct(algo_pagemix, {{"seconds", "0"}}, 2, 256);
        CHECK(m.total == 65536 && m.covered < m.total / 2, "pagemix stopped mid-final-verify marked %llu of %llu pages",
              (unsigned long long)m.covered, (unsigned long long)m.total);
    }
    { // coherency and probe compare no data pages: they must mark nothing
        Out c = direct(algo_coherency, {{"seconds", "1"}}, 0);
        CHECK(c.covered == 0, "coherency marked %llu pages", (unsigned long long)c.covered);
    }
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
    test_cache_override();
    test_flush_mode();
    test_avx2_check();
    test_shared_key_sets();
    test_values();
    test_run_status();
    test_exceptions();
    test_sync_protocol();
    test_lock_slices();
    test_sizing();
    test_start_guard();
    test_stop_gate();
    test_keep_awake();
    test_cpu_set();
    test_coverage_map();
    test_pages_except();
    test_coverage_hooks();
    test_fpu_kernel();
    test_report();
    test_fill_opts();
    test_march_opts();
    test_hammer_opts();
    test_copy_opts();
    test_chase_opts();
    test_rwstorm_opts();
    test_evolve_opts();
    test_probe_opts();
    test_pagemix_opts();
    test_coherency_opts();
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    puts("unit tests: all checks passed");
    return 0;
}
