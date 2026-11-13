// suite.cpp - run_suite, failure summary, FPU stress.
#include "suite.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <thread>
#include <vector>
#include "algo.h"
#include "log.h"
#include "params.h"
#include "platform.h"
#include "seed.h"

namespace ddr5 {

static void emit_skew(const char* prefix, const Stats& s, u64 regionBytes, const Logger& log) {
    u64 n = s.lines.load();
    if (n < 16)
        return;
    std::string out;
    for (int b = 6; b < 48 && (1ull << b) < regionBytes; b++) {
        double f = (double)s.abit[b].load() / (double)n;
        if (f >= 0.85 || f <= 0.15) {
            char buf[64];
            snprintf(buf, sizeof buf, " bit%d=%d(%.0f%%)", b, f >= 0.85 ? 1 : 0, 100.0 * (f >= 0.85 ? f : 1 - f));
            out += buf;
        }
    }
    if (out.empty())
        log.emitf("%saddress bits: errors are spread evenly (no single address bit explains them)\n", prefix);
    else
        log.emitf(
            "%saddress-bit skew (>=85%% of errors share a value):%s\n%s-> errors cluster on one side of these bits: "
            "suspect one rank/bank/row/subchannel region\n",
            prefix, out.c_str(), prefix);
}

// Keeps the FPU loop from being optimised away. Every FPU thread adds to it, so it is atomic (relaxed: only the
// side effect matters, a lost update is harmless); a plain or volatile global would be a data race between threads.
static std::atomic<float> g_fpu_sink{0};

void FpuKernel::reseed() {
    for (int i = 0; i < 8; i++)
        for (int l = 0; l < 8; l++)
            v[i][l] = 1.0f + 0.01f * (float)i;
    loops = 0;
}
void FpuKernel::run(unsigned outer) {
    __m256 x[8];
    for (int i = 0; i < 8; i++)
        x[i] = _mm256_loadu_ps(v[i]);
    const __m256 m = _mm256_set1_ps(1.0000001f), k = _mm256_set1_ps(0.0000001f);
    for (unsigned o = 0; o < outer; o++) {
        for (int it = 0; it < kFpuInner; it++)
            for (int i = 0; i < 8; i++)
                x[i] = _mm256_add_ps(_mm256_mul_ps(x[i], m), k);
        if (++loops >= kFpuReseedLoops) { // renormalise before the values can overflow (see suite.h)
            loops = 0;
            for (int i = 0; i < 8; i++)
                x[i] = _mm256_set1_ps(1.0f + 0.01f * (float)i);
        }
    }
    for (int i = 0; i < 8; i++)
        _mm256_storeu_ps(v[i], x[i]);
}
float FpuKernel::sink() const {
    float s = 0;
    for (int i = 0; i < 8; i++)
        s += v[i][0];
    return s;
}

// Priority and pinning (decided 2026-11-13, D6): the FPU threads are NOT pinned and keep the platform's lowest
// normal priority (Windows: below normal; POSIX: lower_priority() does nothing). Pinning them to the cores of the
// memory workers would make them compete with the very threads that do the test; unpinned and at low priority they take
// the time the workers leave free (barriers, waits, SMT siblings) and the scheduler moves them away from a busy worker.
static void fpu_stress(std::atomic<bool>& stop) {
    plat::lower_priority();
    FpuKernel K;
    while (!stop.load(std::memory_order_relaxed)) {
        K.run(1); // one outer loop = kFpuInner steps of all eight registers, then the stop flag is looked at again
        g_fpu_sink.store(g_fpu_sink.load(std::memory_order_relaxed) + K.sink(), std::memory_order_relaxed);
    }
}

// Runs its function when the scope ends, normally or because an exception is leaving it. run_suite releases what it
// acquires through these (each guard is created right after its acquisition succeeded), which makes the success path
// and the exception path release the same things in the same order (reverse of acquisition).
template <class F>
struct ScopeExit {
    explicit ScopeExit(F fn) : f(std::move(fn)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() { f(); }
    F f;
};

// The FPU stress threads (--stress-fpu): they run next to the memory tests for the length of the run.
struct FpuThreads {
    std::atomic<bool> halt{false};
    std::vector<std::thread> th;
    void start(int n) {
        for (int i = 0; i < n; i++)
            th.emplace_back(fpu_stress, std::ref(halt));
    }
    void stop() { // also called before the summary, so the elapsed time ends when the tests do; safe to call twice
        halt = true;
        for (auto& t : th)
            if (t.joinable())
                t.join();
    }
};

// The tests of the built-in suite that this run was asked for (enabled, in the quick set, matching -f, with a known
// algo), with the run's overrides applied: --test-seconds caps `seconds`, --cache sets flush / nt in every algo that
// lists that key in algo_keys() (so a new algo with those keys gets the override without a change here).
std::vector<TestDef> select_tests(const Settings& s, std::vector<TestDef> tests) {
    std::vector<TestDef> sel;
    const std::string flt = lower(s.filter);
    for (auto& t : tests) {
        if (t.p.has("enabled") && !t.p.flag("enabled", true))
            continue;
        if (s.suite == SuiteSet::Quick && !t.p.flag("quick", false))
            continue;
        if (!flt.empty() && lower(t.name).find(flt) == std::string::npos)
            continue;
        const std::string algo = t.algo();
        if (!algos().count(algo))
            continue;
        if (s.testSeconds && t.p.has("seconds"))
            t.p.m["seconds"] = std::to_string(std::min<u64>(t.p.num("seconds", s.testSeconds), s.testSeconds));
        if (s.cacheMode != CacheMode::Default) {
            const auto& keys = algo_keys(algo);
            if (keys.count("flush"))
                t.p.m["flush"] = s.cacheMode == CacheMode::Bypass ? "1" : "0";
            if (keys.count("nt"))
                t.p.m["nt"] = s.cacheMode == CacheMode::NonTemporal ? "1" : "0";
        }
        t.p.test = t.name; // so a message from the algo's option parsing says which test it is about
        sel.push_back(t);
    }
    return sel;
}

// run_suite, one step per member. It is a friend of Run: it writes the setup state the algos only read. One driver
// serves one run_suite call. The steps that acquire something (error log, region, FPU threads) do not release it: run()
// puts a ScopeExit guard right after each of them, so that is the only place that has to be right.
struct SuiteDriver {
    const Settings& s;
    Run& R;
    Cfg cfg;
    u64 runSeed = 0;
    std::vector<TestDef> sel;
    u64 avail = 0; // free physical memory (0 = the OS would not say)
    u64 per = 0;   // bytes tested by each thread
    u64 total = 0; // bytes tested in all
    bool gotLP = false, locked = false;
    std::string allocNote;
    int failedTests = 0;
    std::vector<std::string> failedNames;

    SuiteDriver(const Settings& st, Run& r) : s(st), R(r) {}

    int run() {
        try {
            std::vector<TestDef> tests = builtin_suite();
            if (!suite_is_valid(tests))
                return 2;
            read_settings();
            sel = select_tests(s, tests);
            if (sel.empty()) {
                R.log.emitf("no tests selected\n");
                return 2;
            }
            if (!open_error_log())
                return 2;
            ScopeExit logGuard([&] { close_error_log(); });
            configure_run();
            if (!size_region())
                return 2;
            announce();
            if (!allocate())
                return 2;
            ScopeExit regionGuard([&] { release_region(); });
            prepare_region();
            FpuThreads fpu;
            ScopeExit fpuGuard(
                [&] { fpu.stop(); }); // a joinable std::thread destroyed during unwinding would terminate
            if (s.stressFpu)
                fpu.start(R.layout.T);
            const double t0 = plat::now();
            R.progress.tStart.store(t0, std::memory_order_release);
            run_tests();
            fpu.stop();
            print_final_summary(plat::now() - t0);
            return R.total.lines ? 1 : 0;   // the guards stop the FPU threads, release the region and close the log
        } catch (const std::exception& e) { // the guards above have already run
            R.log.emitf("internal error: %s\n", e.what());
            return 3;
        } catch (...) {
            R.log.emitf("internal error: unknown exception\n");
            return 3;
        }
    }

    // Keys no algo reads would be ignored silently: say so (a clean suite prints nothing). A value that is not a
    // number / flag / order would silently become a default: refuse to start instead.
    bool suite_is_valid(const std::vector<TestDef>& tests) const {
        for (const TestDef& t : tests)
            for (const std::string& k : unknown_keys(t)) {
                if (t.own.count(k))
                    R.log.emitf("test '%s': key '%s' is not used by algo '%s' (ignored)\n", t.name.c_str(), k.c_str(),
                                t.algo().c_str());
                else
                    R.log.emitf("test '%s': key '%s' (inherited default) is not used by any algo (ignored)\n",
                                t.name.c_str(), k.c_str());
            }
        bool badValue = false;
        for (const TestDef& t : tests)
            for (const std::string& m : bad_values(t)) {
                R.log.emitf("%s\n", m.c_str());
                badValue = true;
            }
        if (badValue)
            R.log.emitf("the built-in suite has a bad value (see above); not starting\n");
        return !badValue;
    }

    void read_settings() {
        cfg.threads = s.threads;
        cfg.large = s.largePages;
        cfg.stopOnErr = s.stopOnError;
        cfg.logFile = s.logFile;
        cfg.loops = s.loops;
        R.rng = s.rngMode;
        set_flush_mode(s.flushMode); // before any worker exists: the hot path reads the choice without synchronisation
        runSeed = s.seedSet ? s.seed : fresh_seed();
    }

    // CLI: errors also go to the log file. GUI: the front-end's log sink already writes everything (incl. errors) to
    // its log. Opened before the allocation, so a bad --log fails at once instead of after sizing and zeroing GBs.
    // false = give up (exit 2); a log file that could not be opened is fatal only when --log was given.
    bool open_error_log() {
        if (R.log.sink)
            return true;
        R.errors.logf = fopen(cfg.logFile.c_str(), "a");
        if (R.errors.logf)
            return true;
        R.log.emitf("cannot open log file '%s': %s\n", cfg.logFile.c_str(), strerror(errno));
        if (s.logFileSet)
            return false;
        R.log.emitf("continuing without it: errors are reported on the console only\n");
        return true;
    }
    void close_error_log() {
        if (R.errors.logf)
            fclose(R.errors.logf);
        R.errors.logf = nullptr;
    }

    void configure_run() {
        R.cfg = cfg;
        R.layout.T = cfg.threads > 0 ? cfg.threads : std::max(1, plat::ncpu());
        R.progress.stopCoverage = s.coverageStop;
        R.progress.stopSeconds = s.minutes * 60.0;
    }

    // How much memory to test: the requested size (default 90% of the free memory) capped at 97% of it, split into
    // whole 2 MB pieces per thread. Says why and returns false when there is no usable answer.
    bool size_region() {
        const u64 MB2 = 2ull << 20;
        avail = plat::avail_phys();
        u64 want = s.megabytes ? s.megabytes << 20 : avail / 100 * 90;
        if (!avail) { // the OS would not say: unknown is not "no memory free"
            if (!s.megabytes) {
                R.log.emitf("cannot determine the free memory; give the size to test with -mb\n");
                return false;
            }
            R.log.emitf("cannot determine the free memory; using the requested size without checking it\n");
        } else if (want > avail / 100 * 97) {
            want = avail / 100 * 97;
            R.log.emitf("requested memory exceeds free memory; reduced to %llu MB to avoid paging\n",
                        (unsigned long long)(want >> 20));
        }
        per = want / R.layout.T / MB2 * MB2;
        if (!per) {
            R.log.emitf("not enough memory (need at least 2 MB per thread)\n");
            return false;
        }
        total = per * R.layout.T;
        return true;
    }

    void announce() {
        std::string freeTxt = " (free memory unknown)";
        if (avail) {
            freeTxt.clear();
            appendf(freeTxt, " of %.2f GiB free", avail / 1073741824.0);
        }
        R.log.emitf("%d threads, testing %.2f GiB%s, %zu tests\n", R.layout.T, total / 1073741824.0, freeTxt.c_str(),
                    sel.size());
        R.log.emitf(
            "flush instruction: %s%s\n", flush_mode_name(),
            s.flushMode == FlushMode::ClFlushOpt && !flush_uses_opt() ? " (CLFLUSHOPT not supported by this CPU)" : "");
        R.log.emitf("run seed: 0x%016llX (%s)  to reproduce: --seed 0x%016llX with the same -mb and -t\n",
                    (unsigned long long)runSeed, s.seedSet ? "given" : "random", (unsigned long long)runSeed);
    }

    bool allocate() {
        R.layout.region = (u8*)plat::alloc(total, cfg.large, gotLP, locked, allocNote);
        if (!R.layout.region)
            R.log.emitf("allocation of %llu MB failed\n", (unsigned long long)(total >> 20));
        return R.layout.region != nullptr;
    }
    void release_region() {
        if (R.layout.region)
            plat::release(R.layout.region, total);
        R.layout.region = nullptr;
    }

    // Tell the front-end and the log about the region, split it into one chunk per thread and zero it (first touch).
    void prepare_region() {
        R.layout.regionBytes.store(total, std::memory_order_release);
        R.log.emitf("memory %s, large pages: %s\n",
                    locked ? "locked"
                           : "NOT locked (the OS may page parts of it out; close other programs or lower Megabytes)",
                    gotLP       ? "yes (2MB contiguous physical runs)"
                    : cfg.large ? "no (needs 'Lock pages in memory' right; using normal pages)"
                                : "off");
        if (!allocNote.empty())
            R.log.emitf("note: %s\n", allocNote.c_str());
        R.layout.chunks.clear();
        for (int t = 0; t < R.layout.T; t++)
            R.layout.chunks.push_back({R.layout.region + (u64)t * per, per / kLineBytes, (u64)t * (per / kLineBytes)});
        // First touch of every page. Done in slices so that Stop is honoured within a fraction of a second even when a
        // thread owns many GB; a stop here leaves the rest untouched and run_tests() then starts nothing.
        R.parallel([&](int tid) {
            const u64 slice = 64ull << 20;
            for (u64 off = 0; off < per && !R.stop.load(std::memory_order_relaxed); off += slice)
                std::memset(R.layout.chunks[tid].base + off, 0, std::min(slice, per - off));
        });
        std::string hdr;
        appendf(hdr, "---- ddr5test run, %.2f GiB, %d threads, seed 0x%016llX ----\n", total / 1073741824.0, R.layout.T,
                (unsigned long long)runSeed);
        R.write_error_log(hdr);
    }

    void run_tests() {
        for (u64 loop = 1; (!cfg.loops || loop <= cfg.loops) && !R.stop; loop++) {
            R.salt = salt_for(runSeed, loop); // each loop gets fresh seeds/permutations, all derived from the run seed
            for (size_t ti = 0; ti < sel.size() && !R.stop; ti++) {
                const u64 bad = run_one_test(loop, ti);
                if (!bad)
                    continue;
                failedTests++;
                if (std::find(failedNames.begin(), failedNames.end(), sel[ti].name) == failedNames.end())
                    failedNames.push_back(sel[ti].name);
                print_failure_detail(bad);
                R.total.lines += bad;
                R.total.bits += R.st.bits.load();
                for (int b = 0; b < 48; b++)
                    R.total.abit[b] += R.st.abit[b].load();
            }
        }
    }

    // One test of one loop: resets the per-test state, runs the algo and prints its pass / FAIL / STOP line.
    // Returns the number of bad lines it found.
    u64 run_one_test(u64 loop, size_t ti) {
        TestDef& t = sel[ti];
        R.testName = t.name;
        R.st.clear();
        R.progress.done = 0;
        R.progress.vbytes = 0;
        R.errors.firstErrTest = -1;
        R.progress.tTestStart = plat::now();
        R.set_label(t.name);
        R.log.emitf("[loop %llu  %zu/%zu] %s\n", (unsigned long long)loop, ti + 1, sel.size(), t.name.c_str());
        double ts = plat::now();
        algos().at(t.algo())(R, t.p);
        double el = plat::now() - ts;
        u64 bad = R.st.lines.load();
        R.log.emitf("    %s  %.1fs  %.1f GB/s avg   coverage this test %.1fx\n",
                    bad ? "FAIL" : (R.stop ? "STOP" : "pass"), el, R.progress.done.load() / 1e9 / std::max(el, 1e-9),
                    (double)R.progress.vbytes.load() / (double)total);
        return bad;
    }

    // What the failed test found: counts, where in the address space, which DQ lanes / beats, ECC codeword shape.
    void print_failure_detail(u64 bad) {
        R.log.emitf("    %llu bad lines, %llu bits; first error %.1f s into this test (%.1f s into the run)\n",
                    (unsigned long long)bad, (unsigned long long)R.st.bits.load(), R.errors.firstErrTest.load(),
                    R.errors.firstErrRun.load());
        emit_skew("    ", R.st, total, R.log);
        std::string l1 = "    DQ lane errors  : ";
        for (int l = 0; l < 32; l++)
            if (R.st.lane[l])
                l1 += "[" + std::to_string(l) + "]=" + std::to_string(R.st.lane[l].load()) + " ";
        std::string l2 = "    BL16 beat errors: ";
        for (int b = 0; b < 16; b++)
            if (R.st.beat[b])
                l2 += "[" + std::to_string(b) + "]=" + std::to_string(R.st.beat[b].load()) + " ";
        R.log.emit(l1 + "\n" + l2 + "\n");
        R.log.emitf("    x8 on-die-ECC codeword (8 lanes x 16 beats, assumed layout) bit-count 1/2/3-4/5+: "
                    "%llu/%llu/%llu/%llu   re-read: transient=%llu stable=%llu unstable=%llu\n",
                    (unsigned long long)R.st.blk[0].load(), (unsigned long long)R.st.blk[1].load(),
                    (unsigned long long)R.st.blk[2].load(), (unsigned long long)R.st.blk[3].load(),
                    (unsigned long long)R.st.trans.load(), (unsigned long long)R.st.stable.load(),
                    (unsigned long long)R.st.unstable.load());
    }

    void print_final_summary(double el) {
        R.log.emitf("\nfinished in %.0fs: %s (%llu bad lines, %d failing test runs)%s%s\n", el,
                    R.total.lines ? "ERRORS FOUND" : "no errors", (unsigned long long)R.total.lines.load(), failedTests,
                    R.progress.stopReason.empty() ? "" : "  - ", R.progress.stopReason.c_str());
        R.log.emitf("total coverage: %.1fx of the %.2f GiB tested region verified\n",
                    (double)R.progress.vtotal.load() / (double)total, total / 1073741824.0);
        if (R.errors.logFailed)
            R.log.emitf("WARNING: writing to the log file '%s' failed (see above): its error details are incomplete\n",
                        cfg.logFile.c_str());
        if (!failedNames.empty()) {
            R.log.emitf("tests with errors:\n");
            for (auto& n : failedNames)
                R.log.emitf("    %s\n", n.c_str());
        }
        if (R.total.lines) {
            R.log.emitf("first error: %.1f s into the run   (run seed 0x%016llX)\nwhole run:\n",
                        R.errors.firstErrRun.load(), (unsigned long long)runSeed);
            emit_skew("    ", R.total, total, R.log);
        }
    }
};

int run_suite(const Settings& s, Run& R) {
    return SuiteDriver(s, R).run();
}

} // namespace ddr5
