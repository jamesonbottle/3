// suite.cpp - built-in suite loading, run_suite, failure summary, FPU stress.
#include "suite.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <sstream>
#include <thread>
#include <vector>
#include "algo.h"
#include "log.h"
#include "params.h"
#include "platform.h"
#include "seed.h"
#include "suite_data.h"   // generated from suite.cfg by tools/gen_suite.py: the whole test list is compiled in   // included here ONLY: it defines static arrays, a second include would duplicate the 16 KB of text

std::string suite_text() { std::string t; for (int i = 0; i < SUITE_CFG_NPARTS; i++) t += SUITE_CFG_PARTS[i]; return t; }

static void emit_skew(const char* prefix, const Stats& s, u64 regionBytes) {
    u64 n = s.lines.load(); if (n < 16) return;
    std::string out;
    for (int b = 6; b < 48 && (1ull << b) < regionBytes; b++) {
        double f = (double)s.abit[b].load() / (double)n;
        if (f >= 0.85 || f <= 0.15) { char buf[64]; snprintf(buf, sizeof buf, " bit%d=%d(%.0f%%)", b, f >= 0.85 ? 1 : 0, 100.0 * (f >= 0.85 ? f : 1 - f)); out += buf; }
    }
    if (out.empty()) emitf("%saddress bits: errors are spread evenly (no single address bit explains them)\n", prefix);
    else emitf("%saddress-bit skew (>=85%% of errors share a value):%s\n%s-> errors cluster on one side of these bits: suspect one rank/bank/row/subchannel region\n", prefix, out.c_str(), prefix);
}

static volatile float g_fpu_sink = 0;
static void fpu_stress(std::atomic<bool>& stop) {
    plat::lower_priority();
    __m256 x[8]; for (int i = 0; i < 8; i++) x[i] = _mm256_set1_ps(1.0f + 0.01f * (float)i);
    const __m256 m = _mm256_set1_ps(1.0000001f), k = _mm256_set1_ps(0.0000001f);
    while (!stop.load(std::memory_order_relaxed)) {
        for (int it = 0; it < 20000; it++) for (int i = 0; i < 8; i++) x[i] = _mm256_add_ps(_mm256_mul_ps(x[i], m), k);
        float t[8]; _mm256_storeu_ps(t, x[0]); g_fpu_sink = g_fpu_sink + t[0];
    }
}


int run_suite(const Settings& s, Run& R) {
    try {
        Cfg cfg; std::vector<TestDef> tests;
        { std::istringstream is(suite_text()); parse_cfg_stream(is, "<built-in suite>", cfg, tests); }
        cfg.threads = s.threads; cfg.large = s.largePages; cfg.stopOnErr = s.stopOnError; cfg.logFile = s.logFile; cfg.loops = s.loops;
        g_rng = s.rngMode == 2 ? 1 : s.rngMode == 3 ? 2 : 0;
        const u64 runSeed = s.seedSet ? s.seed : fresh_seed();
        std::vector<TestDef> sel; std::string flt = lower(s.filter);
        for (auto& t : tests) {
            if (t.p.has("enabled") && !t.p.flag("enabled", true)) continue;
            if (s.suite == 1 && !t.p.flag("quick", false)) continue;
            if (!flt.empty() && lower(t.name).find(flt) == std::string::npos) continue;
            std::string algo = lower(t.p.str("algo", "fill")); if (!ALGOS.count(algo)) continue;
            if (s.testSeconds && t.p.has("seconds")) t.p.m["seconds"] = std::to_string(std::min<u64>(t.p.num("seconds", s.testSeconds), s.testSeconds));
            if (s.cacheMode && (algo == "fill" || algo == "march" || algo == "rwstorm")) {
                t.p.m["flush"] = s.cacheMode == 1 ? "1" : "0"; t.p.m["nt"] = s.cacheMode == 3 ? "1" : "0";
            }
            sel.push_back(t);
        }
        if (sel.empty()) { emitf("no tests selected\n"); return 2; }

        R.cfg = cfg; R.T = cfg.threads > 0 ? cfg.threads : std::max(1, plat::ncpu());
        R.stopCoverage = s.coverageStop; R.stopSeconds = s.minutes * 60.0; R.stop = false;
        const u64 MB2 = 2ull << 20; u64 avail = plat::avail_phys(), want;
        if (s.megabytes) want = s.megabytes << 20; else want = avail / 100 * 90;
        if (want > avail / 100 * 97) { want = avail / 100 * 97; emitf("requested memory exceeds free memory; reduced to %llu MB to avoid paging\n", (unsigned long long)(want >> 20)); }
        u64 per = want / R.T / MB2 * MB2; if (!per) { emitf("not enough memory (need at least 2 MB per thread)\n"); return 2; }
        u64 total = per * R.T; bool gotLP = false, locked = false;
        emitf("%d threads, testing %.2f GiB of %.2f GiB free, %zu tests\n", R.T, total / 1073741824.0, avail / 1073741824.0, sel.size());
        emitf("run seed: 0x%016llX (%s)  to reproduce: --seed 0x%016llX with the same -mb and -t\n", (unsigned long long)runSeed, s.seedSet ? "given" : "random", (unsigned long long)runSeed);
        R.region = (u8*)plat::alloc(total, cfg.large, gotLP, locked);
        if (!R.region) { emitf("allocation of %llu MB failed\n", (unsigned long long)(total >> 20)); return 2; }
        R.regionBytes = total;
        emitf("memory %s, large pages: %s\n", locked ? "locked" : "NOT locked (the OS may page parts of it out; close other programs or lower Megabytes)",
              gotLP ? "yes (2MB contiguous physical runs)" : cfg.large ? "no (needs 'Lock pages in memory' right; using normal pages)" : "off");
        R.chunks.clear(); for (int t = 0; t < R.T; t++) R.chunks.push_back({R.region + (u64)t * per, per / 64, (u64)t * (per / 64)});
        R.running = true;
        R.parallel([&](int tid) { std::memset(R.chunks[tid].base, 0, per); });
        // CLI: errors also go to the log file. GUI: the front-end's emit sink already writes everything (incl. errors) to its log.
        R.logf = g_emit ? nullptr : fopen(cfg.logFile.c_str(), "a");
        if (R.logf) fprintf(R.logf, "---- ddr5test run, %.2f GiB, %d threads, seed 0x%016llX ----\n", total / 1073741824.0, R.T, (unsigned long long)runSeed);

        std::atomic<bool> fpuStop{false}; std::vector<std::thread> fpu;
        if (s.stressFpu) for (int i = 0; i < R.T; i++) fpu.emplace_back(fpu_stress, std::ref(fpuStop));

        double t0 = plat::now(); R.tStart = t0; int failedTests = 0; std::vector<std::string> failedNames;
        for (u64 loop = 1; (!cfg.loops || loop <= cfg.loops) && !R.stop; loop++) {
            g_salt = salt_for(runSeed, loop);  // each loop gets fresh seeds/permutations, all derived from the run seed
            for (size_t ti = 0; ti < sel.size() && !R.stop; ti++) {
                TestDef& t = sel[ti]; R.testName = t.name; R.st.clear(); R.done = 0; R.vbytes = 0; R.firstErrTest = -1; R.tTestStart = plat::now();
                R.set_label(t.name);
                emitf("[loop %llu  %zu/%zu] %s\n", (unsigned long long)loop, ti + 1, sel.size(), t.name.c_str());
                double ts = plat::now(); ALGOS.at(lower(t.p.str("algo", "fill")))(R, t.p); double el = plat::now() - ts;
                u64 bad = R.st.lines.load();
                emitf("    %s  %.1fs  %.1f GB/s avg   coverage this test %.1fx\n", bad ? "FAIL" : (R.stop ? "STOP" : "pass"), el, R.done.load() / 1e9 / std::max(el, 1e-9), (double)R.vbytes.load() / (double)total);
                if (bad) {
                    failedTests++; if (std::find(failedNames.begin(), failedNames.end(), t.name) == failedNames.end()) failedNames.push_back(t.name);
                    emitf("    %llu bad lines, %llu bits; first error %.1f s into this test (%.1f s into the run)\n", (unsigned long long)bad, (unsigned long long)R.st.bits.load(), R.firstErrTest, R.firstErrRun);
                    emit_skew("    ", R.st, total);
                    std::string l1 = "    DQ lane errors  : "; for (int l = 0; l < 32; l++) if (R.st.lane[l]) l1 += "[" + std::to_string(l) + "]=" + std::to_string(R.st.lane[l].load()) + " ";
                    std::string l2 = "    BL16 beat errors: "; for (int b = 0; b < 16; b++) if (R.st.beat[b]) l2 += "[" + std::to_string(b) + "]=" + std::to_string(R.st.beat[b].load()) + " ";
                    emit(l1 + "\n" + l2 + "\n");
                    emitf("    x8 on-die-ECC codeword (8 lanes x 16 beats, assumed layout) bit-count 1/2/3-4/5+: %llu/%llu/%llu/%llu   re-read: transient=%llu stable=%llu unstable=%llu\n",
                          (unsigned long long)R.st.blk[0].load(), (unsigned long long)R.st.blk[1].load(), (unsigned long long)R.st.blk[2].load(), (unsigned long long)R.st.blk[3].load(),
                          (unsigned long long)R.st.trans.load(), (unsigned long long)R.st.stable.load(), (unsigned long long)R.st.unstable.load());
                    R.total.lines += bad; R.total.bits += R.st.bits.load();
                    for (int b = 0; b < 48; b++) R.total.abit[b] += R.st.abit[b].load();
                }
            }
        }
        fpuStop = true; for (auto& th : fpu) th.join();
        double el = plat::now() - t0;
        emitf("\nfinished in %.0fs: %s (%llu bad lines, %d failing test runs)%s%s\n", el, R.total.lines ? "ERRORS FOUND" : "no errors", (unsigned long long)R.total.lines.load(), failedTests,
              R.stopReason.empty() ? "" : "  - ", R.stopReason.c_str());
        emitf("total coverage: %.1fx of the %.2f GiB tested region verified\n", (double)R.vtotal.load() / (double)total, total / 1073741824.0);
        if (!failedNames.empty()) { emitf("tests with errors:\n"); for (auto& n : failedNames) emitf("    %s\n", n.c_str()); }
        if (R.total.lines) { emitf("first error: %.1f s into the run   (run seed 0x%016llX)\nwhole run:\n", R.firstErrRun, (unsigned long long)runSeed); emit_skew("    ", R.total, total); }
        if (R.logf) { fclose(R.logf); R.logf = nullptr; }
        plat::release(R.region, total); R.region = nullptr;
        R.running = false;
        return R.total.lines ? 1 : 0;
    } catch (const std::exception& e) {
        emitf("internal error: %s\n", e.what()); R.running = false; return 3;
    }
}
