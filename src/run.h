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
inline thread_local u64 t_vcount = 0; // lines this thread has verified since its last flush into Run::vbytes / vtotal
struct Run {
    Cfg cfg;
    int T = 1;
    std::vector<Chunk> chunks;
    std::atomic<bool> stop{false};
    std::atomic<u64> done{0};
    Stats st, total;
    std::barrier<>* bar = nullptr;
    std::mutex mu;
    std::string label, testName;
    FILE* logf = nullptr;
    u64 logged = 0;
    std::map<std::string, u64> winctr;
    u8* region = nullptr;
    u64 regionBytes = 0;
    // coverage (HCI/Karhu-style: how many times the tested region has been verified), timing, stop criteria
    std::atomic<u64> vbytes{0}, vtotal{0};
    double tStart = 0, tTestStart = 0, firstErrRun = -1, firstErrTest = -1;
    u64 stopCoverage = 0;
    double stopSeconds = 0;
    std::string stopReason;
    std::atomic<u64> errTotal{0};
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
            vbytes.fetch_add(c * 64, std::memory_order_relaxed);
            vtotal.fetch_add(c * 64, std::memory_order_relaxed);
        }
    }
    void sync() { bar->arrive_and_wait(); }
    void set_label(const std::string& s) {
        std::lock_guard<std::mutex> l(mu);
        label = s;
    }
    void tick() { done.fetch_add(0x10000ull * 64, std::memory_order_relaxed); }
    // Window `ctr` of the thread's chunk. ceil(lines / wl) windows rotate; the last one is shifted back so it ends
    // exactly at the chunk end (it overlaps the previous window instead of leaving a never-tested tail).
    View view(int tid, u64 wl, u64 ctr) const {
        const Chunk& c = chunks[tid];
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
        const Chunk& c = chunks[tid];
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
        u64 wl = (mb << 20) / 64 / T;
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
    std::barrier<> b(T);
    bar = &b;
    std::atomic<int> running{T};
    std::vector<std::thread> th;
    for (int t = 0; t < T; t++)
        th.emplace_back([&, t] {
            plat::pin(t);
            fn(t);
            vflush();
            b.arrive_and_drop();
            running--;
        });
    double t0 = plat::now(), tl = t0, hb = t0;
    u64 dl = done.load();
    while (running.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        double t = plat::now();
        if (!stop.load() && regionBytes) { // user-set stop criteria: coverage target or time limit
            if (stopCoverage && vtotal.load() * 100 >= stopCoverage * regionBytes) {
                stopReason = "coverage target reached";
                stop = true;
            } else if (stopSeconds > 0 && tStart > 0 && t - tStart >= stopSeconds) {
                stopReason = "time limit reached";
                stop = true;
            }
        }
        if (t - tl >= 0.5) {
            u64 d = done.load();
            double rate = (d - dl) / (t - tl) / 1e9;
            tl = t;
            dl = d;
            std::string lab;
            {
                std::lock_guard<std::mutex> l(mu);
                lab = label;
            }
            char buf[220];
            snprintf(buf, sizeof buf, "  [%5.0fs] %-50.50s %5.1f GB/s  cov %4.0f%%  errors: %llu", t - t0, lab.c_str(),
                     rate, regionBytes ? 100.0 * vtotal.load() / regionBytes : 0.0,
                     (unsigned long long)st.lines.load());
            if (g_console && g_tty) {
                printf("\r%-118.118s", buf);
                fflush(stdout);
            } else if (g_console && t - hb >= 30.0) {
                hb = t;
                printf("%s\n", buf);
                fflush(stdout);
            }
        }
    }
    for (auto& x : th)
        x.join();
    bar = nullptr;
    print_clear();
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
