// Deterministic unit tests for the engine's pure logic (no memory is stressed). Build and run: tools/selftest.sh
//   g++ -std=c++20 -O2 -mavx2 -pthread -Isrc tools/unit_tests.cpp -o unit_tests && ./unit_tests
#include <cmath>
#include <set>
#include "engine.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Every address order must visit each of the n lines exactly once, ascending and descending.
static void test_orders() {
    const u64 ns[] = {1, 2, 3, 4, 5, 7, 64, 100, 1000, 4096, 5000, 32768, 32768 + 17, 65536 + 4095};
    const struct { Order::K k; const char* name; } kinds[] = {{Order::SEQ, "seq"}, {Order::STRIDE, "stride"}, {Order::RANDOM, "random"},
                                                              {Order::BITREV, "bitrev"}, {Order::GRAY, "gray"}, {Order::BUTTERFLY, "butterfly"}};
    for (auto& kd : kinds) for (u64 n : ns) for (u64 stride : {1ull, 4ull, 64ull, 1000ull}) for (int down = 0; down < 2; down++) {
        if (kd.k != Order::STRIDE && stride != 1) continue;
        Order o; o.init(kd.k, n, stride, 12345);
        std::vector<u8> seen(n, 0); u64 dup = 0, oob = 0;
        for (u64 q = 0; q < n; q++) { u64 p = o.pos(q, down != 0); if (p >= n) { oob++; continue; } if (seen[p]++) dup++; }
        CHECK(!dup && !oob, "order=%s n=%llu stride=%llu down=%d dup=%llu out-of-range=%llu", kd.name, (unsigned long long)n, (unsigned long long)stride, down,
              (unsigned long long)dup, (unsigned long long)oob);
    }
}

// Window rotation: the windows of one chunk together cover every line (no never-tested tail); the disturb window stays in bounds and never overlaps.
static void test_windows() {
    u8* fake = (u8*)0x100000;
    for (u64 lines : {4096ull * 2, 4096ull * 3, 4096ull * 5 + 123, 32768ull * 7 + 4096, 1000000ull, 8388608ull + 4096 * 3 + 1}) {
        for (u64 wl : {4096ull, 8192ull, 65536ull, 524288ull, 3000000ull}) {
            if (wl >= lines) continue;
            Run R; R.T = 1; R.chunks.push_back({fake, lines, 0});
            u64 nw = (lines + wl - 1) / wl; std::vector<u8> cov(lines, 0);
            for (u64 ctr = 0; ctr < nw; ctr++) {
                View v = R.view(0, wl, ctr); u64 s = (u64)(v.base - fake) / 64;
                CHECK(v.n == wl && s + wl <= lines && v.g == s, "view lines=%llu wl=%llu ctr=%llu start=%llu", (unsigned long long)lines, (unsigned long long)wl, (unsigned long long)ctr, (unsigned long long)s);
                for (u64 i = s; i < s + wl && i < lines; i++) cov[i] = 1;
            }
            u64 missed = 0; for (u64 i = 0; i < lines; i++) missed += !cov[i];
            CHECK(missed == 0, "lines=%llu wl=%llu: %llu lines never covered by any window", (unsigned long long)lines, (unsigned long long)wl, (unsigned long long)missed);
            if (lines / wl >= 2) for (u64 ctr = 0; ctr < nw * 2; ctr++) {   // disturb is only used when >= 2 whole windows fit (disturb_possible)
                View v = R.view(0, wl, ctr), d = R.dview(0, wl, ctr); u64 sv = (u64)(v.base - fake) / 64, sd = (u64)(d.base - fake) / 64;
                bool inb = sd + wl <= lines, disjoint = sd + wl <= sv || sv + wl <= sd;
                CHECK(inb && disjoint && d.g == sd, "dview lines=%llu wl=%llu ctr=%llu victim=%llu disturb=%llu", (unsigned long long)lines, (unsigned long long)wl,
                      (unsigned long long)ctr, (unsigned long long)sv, (unsigned long long)sd);
            }
        }
    }
}

// The 'sat' pattern table: every selectable (pattern, width) pair yields a non-empty, finite pattern and inverse data is the exact complement.
static void test_generators() {
    for (auto& sp : sat_table()) CHECK(sp.len >= 1 && sp.len <= 64, "pattern %s len=%d", sp.name, sp.len);
    Gen g; g.kind = 1; g.seed = 99; Line a{}, b{}, c{};
    g.make(7, nullptr, false, a); g.make(7, nullptr, false, b); g.make(7, nullptr, true, c);
    bool same = !memcmp(&a, &b, 64), comp = true; for (int j = 0; j < 8; j++) comp &= (c.w[j] == ~a.w[j]);
    CHECK(same, "hash data must be a pure function of the line index"); CHECK(comp, "inverted data must be the exact complement");
    Gen h; h.kind = 1; h.seed = 99; Line d{}; h.make(8, nullptr, false, d);
    CHECK(memcmp(&a, &d, 64) != 0, "neighbouring lines must get different hash data");
}

// Every selectable hash mixer must avalanche: flipping one input bit flips each output bit with probability 1/2. xorshift64* used to fail this
// badly (a bare multiply as its finalizer: 308 of the 4096 input-bit -> output-bit pairs never flipped, 1450 were biased), so seeds and
// high address bits barely influenced the low bits of 'hash' data. 8192 samples put the noise at ~0.006, so 0.06 cannot fail by chance.
static void test_mixers() {
    const char* names[] = {"splitmix64", "murmur3 fmix64", "xorshift64*"};
    const int N = 8192; static u32 cnt[64][64];
    for (int mode = 0; mode < 3; mode++) {
        g_rng = mode; memset(cnt, 0, sizeof cnt); u64 rs = 0x1234;
        for (int n = 0; n < N; n++) {
            rs = splitmix(rs); u64 h0 = mix64(rs);
            for (int i = 0; i < 64; i++) { u64 d = h0 ^ mix64(rs ^ (1ull << i)); for (int j = 0; j < 64; j++) cnt[i][j] += (d >> j) & 1; }
        }
        int bad = 0, dead = 0; double worst = 0;
        for (int i = 0; i < 64; i++) for (int j = 0; j < 64; j++) {
            double dev = std::fabs((double)cnt[i][j] / N - 0.5); worst = std::max(worst, dev); bad += dev > 0.06; dead += cnt[i][j] == 0;
        }
        CHECK(!bad && !dead, "%s: %d of 4096 input->output bit pairs biased by more than 0.06 (worst %.3f), %d never flip", names[mode], bad, worst, dead);
    }
    g_rng = 0;
}

// One run seed must determine every random choice (hash seeds, random permutations), and different seeds / loops / runs must give different ones.
static void test_seed() {
    CHECK(salt_for(5, 1) == salt_for(5, 1), "salt must be a pure function of (seed, loop)");
    CHECK(salt_for(5, 1) != salt_for(5, 2) && salt_for(5, 1) != salt_for(6, 1) && salt_for(0, 1) != salt_for(0, 2), "salt must change with the seed and with the loop");
    CHECK(fresh_seed() != fresh_seed(), "two runs without --seed must not get the same seed");
    Params p; p.m["data"] = "hash"; p.m["seeds"] = "3";
    struct Sig { std::vector<u64> hash, perm; };
    auto sig = [&](u64 seed, u64 loop) {
        g_salt = salt_for(seed, loop); Sig s; for (auto& v : build_vars(p)) s.hash.push_back(v.g.seed);
        Order o; o.init(Order::RANDOM, 5000, 1, 1); for (u64 i = 0; i < 64; i++) s.perm.push_back(o.at(i));
        return s;
    };
    Sig a = sig(42, 1), b = sig(42, 1), c = sig(43, 1), d = sig(42, 2);
    CHECK(a.hash == b.hash && a.perm == b.perm, "same seed and loop must reproduce the hash seeds and the random permutation");
    CHECK(a.hash != c.hash && a.perm != c.perm, "a different seed must change the hash seeds and the random permutation");
    CHECK(a.hash != d.hash && a.perm != d.perm, "a different loop must change the hash seeds and the random permutation");
    CHECK(std::set<u64>(a.hash.begin(), a.hash.end()).size() == a.hash.size(), "the seeds of one run must differ from each other");
    g_salt = 0;
}

// Evolving data: generation rules and the property that makes a lost write visible (the old generation never equals the new one).
static void test_evolve() {
    const u64 seed = 0xABCDEF, maxp = 8, passes = 16, lines = 20000; u64 hist[9] = {};
    for (u64 g = 0; g < lines; g++) {
        u64 per = evo_period(g, seed, maxp); CHECK(per >= 1 && per <= maxp, "line %llu: period %llu out of range", (unsigned long long)g, (unsigned long long)per);
        hist[per <= maxp ? per : 0]++;
        u64 prev = 0, rewrites = 0;
        for (u64 p = 1; p <= passes; p++) {
            u64 gen = p / per; bool updated = p % per == 0;
            CHECK(gen >= prev && gen - prev <= 1 && updated == (gen != prev), "line %llu pass %llu: generation %llu -> %llu, rewritten=%d", (unsigned long long)g, (unsigned long long)p,
                  (unsigned long long)prev, (unsigned long long)gen, (int)updated);
            rewrites += updated; prev = gen;
        }
        CHECK(rewrites == passes / per && rewrites >= 1, "line %llu (period %llu) rewritten %llu times in %llu passes", (unsigned long long)g, (unsigned long long)per, (unsigned long long)rewrites, (unsigned long long)passes);
    }
    for (u64 per = 1; per <= maxp; per++) CHECK(hist[per] > lines / maxp * 3 / 4 && hist[per] < lines / maxp * 5 / 4, "period %llu chosen by %llu of %llu lines (expected ~%llu)",
                                                  (unsigned long long)per, (unsigned long long)hist[per], (unsigned long long)lines, (unsigned long long)(lines / maxp));
    for (int mode = 0; mode < 3; mode++) {   // the data follows the selected RNG, so every mixer must keep (line, generation) pairs apart
        g_rng = mode; std::set<u64> seen; Line a{}, b{}, c{}; evo_make(7, 3, seed, a); evo_make(7, 3, seed, b); evo_make(7, 2, seed, c);
        CHECK(!memcmp(&a, &b, 64), "rng %d: evolving data must be a pure function of (line, generation, seed)", mode);
        CHECK(memcmp(&a, &c, 64) != 0, "rng %d: the previous generation must differ from the new one (otherwise a lost write goes unseen)", mode);
        size_t dup = 0; for (u64 g = 0; g < 500; g++) for (u64 gen = 0; gen < 16; gen++) { Line l{}; evo_make(g, gen, seed, l); dup += !seen.insert(l.w[0]).second; }
        CHECK(!dup, "rng %d: %zu (line, generation) pairs produced the same first qword", mode, dup);
    }
    g_rng = 0;
}

// Every style / pattern name used by suite.cfg expands to at least one variant (a typo would silently drop a test).
static void test_suite_parses() {
    Cfg cfg; std::vector<TestDef> tests; std::istringstream is(suite_text()); parse_cfg_stream(is, "<built-in>", cfg, tests);
    CHECK(tests.size() == 28, "expected 28 tests, got %zu", tests.size());
    size_t quick = 0; for (auto& t : tests) quick += t.p.flag("quick", false);
    CHECK(quick == 12, "expected 12 quick tests, got %zu", quick);
    for (auto& t : tests) {
        std::string algo = lower(t.p.str("algo", "fill"));
        CHECK(ALGOS.count(algo) == 1, "test '%s': unknown algo '%s'", t.name.c_str(), algo.c_str());
        if (algo == "fill" || algo == "march" || algo == "rwstorm" || algo == "evolve" || algo == "copy" || algo == "chase")
            CHECK(!build_vars(t.p).empty(), "test '%s': no data variants (typo in data/style/pattern?)", t.name.c_str());
        if (algo == "march") CHECK(!parse_march_variants(t.p.str("march")).empty(), "test '%s': march parses to nothing", t.name.c_str());
    }
}

int main() {
    g_emit = [](const std::string& s) { printf("  (engine) %s", s.c_str()); };   // keep parser diagnostics visible, and keep them off the real log
    test_orders(); test_windows(); test_generators(); test_mixers(); test_seed(); test_evolve(); test_suite_parses();
    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    puts("unit tests: all checks passed");
    return 0;
}
