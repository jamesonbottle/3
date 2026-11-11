#pragma once
// run.h - Chunk, View, Stats, Layout, Progress, ErrorSink, Run, walk().
#include <algorithm>
#include <atomic>
#include <exception>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include "log.h"
#include "order.h"
#include "params.h"
#include "platform.h"
#include "types.h"

namespace ddr5 {

// ------------------------------------------------------------------ run context
struct Chunk {
    u8* base;
    u64 lines;
    u64 g;
};
struct View {
    u8* base;
    u64 g;
    u64 n;
};
struct Stats {
    std::atomic<u64> lines{0}, bits{0}, lane[32], beat[16], blk[4], abit[48], trans{0}, stable{0}, unstable{0};
    Stats() { clear(); }
    void clear() {
        lines = 0;
        bits = 0;
        trans = 0;
        stable = 0;
        unstable = 0;
        for (auto& a : lane)
            a = 0;
        for (auto& a : beat)
            a = 0;
        for (auto& a : blk)
            a = 0;
        for (auto& a : abit)
            a = 0;
    }
};
inline thread_local u64 t_vcount =
    0; // lines this thread has verified since its last flush into Progress::vbytes / vtotal

// Where the tested memory is and how it is split between the worker threads. run_suite fills it in before the first
// test starts; the algos only read it.
struct Layout {
    u8* region = nullptr;
    std::atomic<u64> regionBytes{0}; // set once by run_suite (release); the GUI reads it through Run::status()
    int T = 1;                       // worker threads
    std::vector<Chunk> chunks;       // one per thread
};

// How far the run has got: the byte counters the workers bump, the clocks, and the user's stop criteria.
struct Progress {
    std::atomic<u64> done{0}; // bytes moved in the current test (drives the GB/s figure)
    // coverage (HCI/Karhu-style: how many times the tested region has been verified): this test / the whole run
    std::atomic<u64> vbytes{0}, vtotal{0};
    std::atomic<double> tStart{0}; // plat::now() at the start of the run; set once by run_suite, read by Run::status()
    double tTestStart = 0;         // plat::now() at the start of the current test
    u64 stopCoverage = 0;          // stop once vtotal reaches this many percent of the region (0 = off)
    double stopSeconds = 0;        // stop after this many seconds (0 = off)
    std::string stopReason;
};

// Error reporting: the log file, the "first error" times, the counters. `mu` serializes Run::report and also guards
// Run::label (the status line reads it from the GUI thread).
struct ErrorSink {
    mutable std::mutex mu; // mutable: Run::status() is const but has to lock it
    FILE* logf = nullptr;
    u64 logged = 0;
    double firstErrRun = -1, firstErrTest = -1; // seconds into the run / the current test; -1 = no error yet
    std::atomic<u64> errTotal{0};
};

struct Run;
struct Settings;
int run_suite(const Settings& s, Run& R); // suite.h: the one function besides Run's own methods that drives a Run

// The state of one run of the suite, in three parts:
//  - the algo-facing part (public, first): what algo_*.cpp may use. The data members are read-only for the algos
//    (run_suite sets them before the tests start); they reach the shared counters only through the methods.
//  - the front-end part (public, `Status` and below): what a GUI/CLI needs to follow a run from another thread.
//  - everything else is private; run_suite is the only outside code that writes it (friend).
//
// Ownership and threads. One Run serves exactly ONE run_suite call: the totals and error counters are never reset, so
// a front-end makes a fresh Run per Start (the GUI) or has a single one (the CLI).
//  - The front-end owns the Run and the Logger it refers to, and keeps both alive until run_suite returns. The GUI
//    runs run_suite on a worker thread and follows it from the UI thread through status() / error_count().
//  - run_suite owns the setup: it fills in cfg, rng, salt, testName, layout and progress, and opens and closes
//    errors.logf (CLI only; the GUI's Logger sink writes the log itself). The fields status() reads from another
//    thread (layout.regionBytes, progress.tStart) are atomics that run_suite stores with release order.
//  - `stop` may be set by anyone, at any time, even before run_suite starts (a Stop that arrives early must not be
//    lost): a front-end (Stop button, Ctrl-C), report() on the first error in stop-on-error mode, parallel() when a
//    stop criterion (coverage or time) is reached. Nobody clears it: a Run serves exactly one run_suite call.
//  - report() is called by the worker threads; under errors.mu it updates the error counters, the "first error" times
//    and errors.logf. parallel() sets `bar` for its duration and joins every worker before it returns.
//  - A Run is neither copyable nor movable: the workers, the barrier and the front-end all hold its address.
struct Run {
    // ---- algo-facing: data (read-only for the algos)
    const Logger& log; // where output goes; owned by the front-end, must outlive the Run
    explicit Run(const Logger& l) : log(l) {}
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;
    u64 salt = 0; // from the run seed and the loop number (run_suite sets it per loop): every random choice is a
                  // function of it
    int rng = 0;  // hash mixer chosen in Settings (0 splitmix64, 1 murmur3 fmix64, 2 xorshift64*); see mix64()
    Layout layout;
    std::string testName;              // the running test; also the key into winctr
    std::map<std::string, u64> winctr; // per test name: how many windows earlier loops have used (rotates the windows)
    std::atomic<bool> stop{false};     // set by a front-end (Stop button, Ctrl-C), by report() on the first error in
                                       // stop-on-error mode, and by parallel() when a stop criterion is reached
    // ---- algo-facing: methods
    // call once per verified 64B line; flushes to the shared counters every 64K lines per thread, and once more when
    // the thread ends (parallel() calls vflush), so the lines counted since the last 64K boundary are not lost
    void vtick() {
        if (++t_vcount >= 0x10000)
            vflush();
    }
    void sync() { bar->arrive_and_wait(); }
    void set_label(const std::string& s) {
        std::lock_guard<std::mutex> l(errors.mu);
        label = s;
    }
    void tick() { progress.done.fetch_add(0x10000ull * 64, std::memory_order_relaxed); }
    void add_done(u64 bytes) { progress.done.fetch_add(bytes, std::memory_order_relaxed); } // bytes moved, for GB/s
    // Window `ctr` of the thread's chunk. ceil(lines / wl) windows rotate; the last one is shifted back so it ends
    // exactly at the chunk end (it overlaps the previous window instead of leaving a never-tested tail).
    View view(int tid, u64 wl, u64 ctr) const {
        const Chunk& c = layout.chunks[tid];
        if (!wl || wl >= c.lines)
            return {c.base, c.g, c.lines};
        u64 nw = (c.lines + wl - 1) / wl, s = (ctr % nw) * wl;
        if (s + wl > c.lines)
            s = c.lines - wl;
        return {c.base + s * 64, c.g + s, wl};
    }
    // A window of the same size that never overlaps view(tid, wl, ctr): where the disturb traffic goes. Needs >= 2
    // whole windows per chunk.
    View dview(int tid, u64 wl, u64 ctr) const {
        const Chunk& c = layout.chunks[tid];
        View v = view(tid, wl, ctr);
        u64 sv = (u64)(v.base - c.base) / 64, sd = sv + wl;
        if (sd + wl > c.lines)
            sd = sv >= wl ? sv - wl : sv + wl;
        return {c.base + sd * 64, c.g + sd, wl};
    }
    u64 window_lines(const Params& P) const {
        u64 mb = P.num("window_mb", 0);
        if (!mb)
            return 0;
        u64 wl = (mb << 20) / 64 / layout.T;
        wl = wl / 4096 * 4096;
        return std::max<u64>(wl, 4096);
    }
    void sleep_ms(u64 ms) {
        double end = plat::now() + ms / 1000.0;
        while (plat::now() < end && !stop.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    template <class F>
    void parallel(F fn);
    void report(int tid, const u8* addr, const Line& e, const Line& a, const char* phase, bool reread = true);

    // ---- front-end: follow a run from another thread
    struct Status {
        double tStart;     // plat::now() when the run started; 0 while the memory is still being allocated
        u64 regionBytes;   // size of the tested region; 0 before it is allocated
        u64 vtotal;        // bytes verified so far in the whole run (coverage = vtotal / regionBytes)
        u64 errors;        // bad lines so far
        std::string label; // the running test
    };
    Status status() const {
        Status s{progress.tStart.load(std::memory_order_acquire),
                 layout.regionBytes.load(std::memory_order_acquire),
                 progress.vtotal.load(),
                 errors.errTotal.load(),
                 {}};
        std::lock_guard<std::mutex> l(errors.mu);
        s.label = label;
        return s;
    }
    u64 error_count() const { return errors.errTotal.load(); }

  private:
    friend int run_suite(const Settings& s, Run& R);
    void vflush() {
        u64 c = t_vcount;
        t_vcount = 0;
        if (c) {
            progress.vbytes.fetch_add(c * 64, std::memory_order_relaxed);
            progress.vtotal.fetch_add(c * 64, std::memory_order_relaxed);
        }
    }

    Cfg cfg;
    Progress progress;
    ErrorSink errors;
    Stats st, total;                  // this test's / the whole run's error statistics
    std::barrier<>* bar = nullptr;    // the barrier of the parallel() in progress (sync() waits on it)
    std::atomic<bool> aborted{false}; // a worker threw: report() ignores what the other workers see from then on
    std::string label;                // the running test's name for the status line; guarded by errors.mu
};
static_assert(!std::is_copy_constructible_v<Run> && !std::is_copy_assignable_v<Run> &&
                  !std::is_move_constructible_v<Run> && !std::is_move_assignable_v<Run>,
              "Run is shared by address between the workers, the barrier and the front-end");

template <class F>
void Run::parallel(F fn) {
    std::barrier<> b(layout.T);
    bar = &b;
    std::atomic<int> running{layout.T};
    // Nothing that goes wrong in here may reach std::terminate (a joinable std::thread that is destroyed, or an
    // exception that leaves a thread function). An exception from a worker, from creating the workers or from the
    // monitor loop below is kept (the first one), the run is stopped and marked aborted, every started worker still
    // leaves the barrier (so nobody waits for it forever) and is joined, and the exception is rethrown on the calling
    // thread once all workers are gone.
    std::exception_ptr failure;
    std::mutex failMu;
    auto fail = [&] { // call only from a catch block
        {
            std::lock_guard<std::mutex> l(failMu);
            if (!failure)
                failure = std::current_exception();
        }
        aborted = true;
        stop = true;
    };
    std::vector<std::thread> th;
    int started = 0;
    try {
        th.reserve((size_t)layout.T);
        for (int t = 0; t < layout.T; t++) {
            th.emplace_back([&, t] {
                try {
                    plat::pin(t);
                    fn(t);
                } catch (...) {
                    fail();
                }
                vflush();
                b.arrive_and_drop();
                running--;
            });
            started++;
        }
    } catch (...) { // e.g. the OS thread limit: the workers that never started must not be waited for
        fail();
        for (int k = started; k < layout.T; k++) {
            b.arrive_and_drop();
            running--;
        }
    }
    double t0 = plat::now(), tl = t0, hb = t0;
    u64 dl = progress.done.load();
    try {
        while (running.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            double t = plat::now();
            if (!stop.load() && layout.regionBytes) { // user-set stop criteria: coverage target or time limit
                if (progress.stopCoverage &&
                    progress.vtotal.load() * 100 >= progress.stopCoverage * layout.regionBytes) {
                    progress.stopReason = "coverage target reached";
                    stop = true;
                } else if (progress.stopSeconds > 0 && progress.tStart > 0 &&
                           t - progress.tStart >= progress.stopSeconds) {
                    progress.stopReason = "time limit reached";
                    stop = true;
                }
            }
            if (t - tl >= 0.5) {
                u64 d = progress.done.load();
                double rate = (d - dl) / (t - tl) / 1e9;
                tl = t;
                dl = d;
                std::string lab;
                {
                    std::lock_guard<std::mutex> l(errors.mu);
                    lab = label;
                }
                char buf[220];
                snprintf(buf, sizeof buf, "  [%5.0fs] %-50.50s %5.1f GB/s  cov %4.0f%%  errors: %llu (this test %llu)",
                         t - t0, lab.c_str(), rate,
                         layout.regionBytes ? 100.0 * progress.vtotal.load() / layout.regionBytes : 0.0,
                         (unsigned long long)errors.errTotal.load(), // run total: st is reset for every test
                         (unsigned long long)st.lines.load());
                if (log.console && log.tty) {
                    printf("\r%-118.118s", buf);
                    fflush(stdout);
                } else if (log.console && t - hb >= 30.0) {
                    hb = t;
                    printf("%s\n", buf);
                    fflush(stdout);
                }
            }
        }
    } catch (...) {
        fail();
    }
    for (auto& x : th)
        x.join();
    bar = nullptr;
    log.print_clear();
    if (failure)
        std::rethrow_exception(failure);
}

// walk q = 0..n-1 through an order; f(lineIndex)
template <class F>
inline void walk(Run& R, const Order& o, bool down, F&& f) {
    for (u64 q = 0; q < o.n; q++) {
        f(o.pos(q, down));
        if (((q + 1) & 0xFFFF) == 0) {
            R.tick();
            if (R.stop.load(std::memory_order_relaxed))
                return;
        }
    }
}

} // namespace ddr5
