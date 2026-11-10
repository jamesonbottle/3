// algo_coherency.cpp - stressapptest-style cache-coherency check (tests cores/fabric, not DRAM cells).
#include "algo.h"
#include "gen.h"
#include "lineio.h"
#include "log.h"
#include "order.h"
#include "platform.h"
#include "run.h"
#include "seed.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

namespace ddr5 {

// ------------------------------------------------------------------ algo: coherency (stressapptest-style
// cache-coherency check) A few cache lines are shared by all threads. Every thread owns one byte in each line and
// increments it at random lines (false sharing: the line ping-pongs between cores while each core only touches its own
// byte). After a round every thread sums its bytes over all lines and compares the low 8 bits with the number of
// increments it made. A lost or stale write = coherency fault. This exercises the cores' caches and the fabric between
// them (CCX/CCD, ring), not DRAM cells.
void algo_coherency(Run& R, const Params& P) {
    auto lines = P.nums("lines", "2 8 64");
    u64 inc = std::max<u64>(1, P.num("inc", 1000));
    double secs = (double)P.num("seconds", 8);
    int nt = std::min(R.T, 64);
    std::atomic<bool> fin{false};
    R.parallel([&](int tid) {
        for (u64 L : lines) {
            L = std::min<u64>(std::max<u64>(L, 1), 64);
            u8* mem = R.region;
            if (tid == 0) {
                R.set_label("cache coherency, " + std::to_string(L) + " shared lines, " + std::to_string(nt) +
                            " threads");
                std::memset(mem, 0, L * 64);
                fin = false;
            }
            double end = plat::now() + secs;
            u32 x = (u32)(splitmix(tid + 1 + g_salt + L * 977) | 1);
            R.sync();
            for (;;) {
                if (tid < nt) {
                    for (u64 i = 0; i < inc; i++) {
                        x = (x >> 1) ^ ((0u - (x & 1)) & 0xD0000001u);
                        u64 line = x % L;
                        u64 off = (line & (u64)tid & 1) ? (u64)(nt & ~1) - tid
                                                        : (u64)tid; // owner byte position differs between lines
                        volatile u8* c = mem + line * 64 + off;
                        *c = (u8)(*c + 1);
                    }
                }
#ifdef DDR5TEST_INJECT // self-test only: one lost/extra increment
                if (tid == 0) {
                    static bool once = false;
                    if (!once) {
                        once = true;
                        mem[0]++;
                    }
                }
#endif
                R.sync();
                if (tid < nt) {
                    u64 sum = 0;
                    for (u64 line = 0; line < L; line++) {
                        u64 off = (line & (u64)tid & 1) ? (u64)(nt & ~1) - tid : (u64)tid;
                        volatile u8* c = mem + line * 64 + off;
                        sum += *c;
                        *c = 0;
                    }
                    if ((sum & 0xFF) != (inc & 0xFF)) {
                        Line e{}, a{};
                        e.w[0] = inc & 0xFF;
                        a.w[0] = sum & 0xFF;
                        R.report(tid, mem + tid, e, a, "coherency", false);
                    }
                    R.done.fetch_add(inc * 8, std::memory_order_relaxed);
                }
                R.sync();
                if (tid == 0)
                    fin = R.stop.load() || plat::now() >= end;
                R.sync();
                if (fin.load())
                    break;
            }
            R.sync();
        }
    });
}

} // namespace ddr5
