// Deterministic unit tests for the engine's pure logic (no memory is stressed). Build and run: tools/selftest.sh
//   g++ -std=c++20 -O2 -mavx2 -pthread -Isrc tools/unit_tests.cpp -o unit_tests && ./unit_tests
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

// Every style / pattern name used by suite.cfg expands to at least one variant (a typo would silently drop a test).
static void test_suite_parses() {
    Cfg cfg; std::vector<TestDef> tests; std::istringstream is(suite_text()); parse_cfg_stream(is, "<built-in>", cfg, tests);
    CHECK(tests.size() == 27, "expected 27 tests, got %zu", tests.size());
    size_t quick = 0; for (auto& t : tests) quick += t.p.flag("quick", false);
    CHECK(quick == 11, "expected 11 quick tests, got %zu", quick);
    for (auto& t : tests) {
        std::string algo = lower(t.p.str("algo", "fill"));
        CHECK(ALGOS.count(algo) == 1, "test '%s': unknown algo '%s'", t.name.c_str(), algo.c_str());
        if (algo == "fill" || algo == "march" || algo == "rwstorm" || algo == "copy" || algo == "chase")
            CHECK(!build_vars(t.p).empty(), "test '%s': no data variants (typo in data/style/pattern?)", t.name.c_str());
        if (algo == "march") CHECK(!parse_march_variants(t.p.str("march")).empty(), "test '%s': march parses to nothing", t.name.c_str());
    }
}

int main() {
    g_emit = [](const std::string& s) { printf("  (engine) %s", s.c_str()); };   // keep parser diagnostics visible, and keep them off the real log
    test_orders(); test_windows(); test_generators(); test_suite_parses();
    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    puts("unit tests: all checks passed");
    return 0;
}
