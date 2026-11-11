#pragma once
// run.h - Chunk, View, Stats, Run, walk().
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
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
    u64 regionBytes = 0;
    int T = 1;                 // worker threads
    std::vector<Chunk> chunks; // one per thread
};

// How far the run has got: the byte counters the workers bump, the clocks, and the user's stop criteria.
struct Progress {
    std::atomic<u64> done{0}; // bytes moved in the current test (drives the GB/s figure)
    // coverage (HCI/Karhu-style: how many times the tested region has been verified): this test / the whole run
    std::atomic<u64> vbytes{0}, vtotal{0};
    double tStart = 0, tTestStart = 0; // plat::now() at the start of the run / of the current test
    u64 stopCoverage = 0;              // stop once vtotal reaches this many percent of the region (0 = off)
    double stopSeconds = 0;            // stop after this many seconds (0 = off)
    std::string stopReason;
};

// Error reporting: the log file, the "first error" times, the counters. `mu` serializes Run::report and also guards
// Run::label (the status line reads it from the GUI thread).
struct ErrorSink {
    std::mutex mu;
    FILE* logf = nullptr;
    u64 logged = 0;
    double firstErrRun = -1, firstErrTest = -1; // seconds into the run / the current test; -1 = no error yet
    std::atomic<u64> errTotal{0};
};

struct Run {
    const Logger& log; // where output goes; owned by the front-end, must outlive the Run
    explicit Run(const Logger& l) : log(l) {}
    Cfg cfg;
    u64 salt = 0; // from the run seed and the loop number (run_suite sets it per loop): every random choice is a
                  // function of it
    int rng = 0;  // hash mixer chosen in Settings (0 splitmix64, 1 murmur3 fmix64, 2 xorshift64*); see mix64()
    Layout layout;
    Progress progress;
    ErrorSink errors;
    std::atomic<bool> stop{false};
    Stats st, total;
    std::barrier<>* bar = nullptr;
    std::string label, testName;
    std::map<std::string, u64> winctr;
    std::atomic<bool> running{false};

    // call once per verified 64B line; flushes to the shared counters every 64K lines per thread, and once more when
    // the thread ends (parallel() calls vflush), so the lines counted since the last 64K boundary are not lost
    void vtick() {
        if (++t_vcount >= 0x10000)
            vflush();
    }
    void vflush() {
        u64 c = t_vcount;
        t_vcount = 0;
        if (c) {
            progress.vbytes.fetch_add(c * 64, std::memory_order_relaxed);
            progress.vtotal.fetch_add(c * 64, std::memory_order_relaxed);
        }
    }
    void sync() { bar->arrive_and_wait(); }
    void set_label(const std::string& s) {
        std::lock_guard<std::mutex> l(errors.mu);
        label = s;
    }
    void tick() { progress.done.fetch_add(0x10000ull * 64, std::memory_order_relaxed); }
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
};

template <class F>
void Run::parallel(F fn) {
    std::barrier<> b(layout.T);
    bar = &b;
    std::atomic<int> running{layout.T};
    std::vector<std::thread> th;
    for (int t = 0; t < layout.T; t++)
        th.emplace_back([&, t] {
            plat::pin(t);
            fn(t);
            vflush();
            b.arrive_and_drop();
            running--;
        });
    double t0 = plat::now(), tl = t0, hb = t0;
    u64 dl = progress.done.load();
    while (running.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        double t = plat::now();
        if (!stop.load() && layout.regionBytes) { // user-set stop criteria: coverage target or time limit
            if (progress.stopCoverage && progress.vtotal.load() * 100 >= progress.stopCoverage * layout.regionBytes) {
                progress.stopReason = "coverage target reached";
                stop = true;
            } else if (progress.stopSeconds > 0 && progress.tStart > 0 && t - progress.tStart >= progress.stopSeconds) {
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
            snprintf(buf, sizeof buf, "  [%5.0fs] %-50.50s %5.1f GB/s  cov %4.0f%%  errors: %llu", t - t0, lab.c_str(),
                     rate, layout.regionBytes ? 100.0 * progress.vtotal.load() / layout.regionBytes : 0.0,
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
    for (auto& x : th)
        x.join();
    bar = nullptr;
    log.print_clear();
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
