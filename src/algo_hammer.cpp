// algo_hammer.cpp - n-sided row hammer / activation-rate stress.
#include "algo.h"
#include "gen.h"
#include "lineio.h"
#include "log.h"
#include "order.h"
#include "platform.h"
#include "run.h"
#include "seed.h"
#include <string>
#include <vector>

// ------------------------------------------------------------------ algo: hammer (n-sided row hammer / activation-rate stress)
void algo_hammer(Run& R, const Params& P) {
    auto vars = build_vars(P); auto strides = P.nums("rowstride", "256K 1M");
    auto sides = P.nums("aggressors", "2 9"); u64 acts = P.num("acts", 400000), rounds = P.num("rounds", 24), wl = R.window_lines(P);
    struct Job { std::string label; const Gen* g; u64 sL; u64 A; };
    std::vector<Job> jobs;
    for (auto& v : vars) for (u64 s : strides) for (u64 a : sides)
        jobs.push_back({v.label + " rowstride=" + std::to_string(s) + "B " + std::to_string(a) + "-sided", &v.g, std::max<u64>(1, s / 64), a});
    u64 base = R.winctr[R.testName]; R.winctr[R.testName] += jobs.size();
    R.parallel([&](int tid) {
        u64 rng = splitmix(0xABCD + tid * 7919 + g_salt);
        for (size_t ji = 0; ji < jobs.size(); ji++) {
            if (R.stop) break;
            const Job& j = jobs[ji]; if (tid == 0) R.set_label("hammer " + j.label);
            View w = R.view(tid, wl, base + ji); using io = IO<32, 0, 1>;
            for (u64 i = 0; i < w.n; i++) { u8* a = w.base + i * 64; Line l; j.g->make(w.g + i, a, false, l); io::store(a, l); if (((i + 1) & 0xFFFF) == 0) R.tick(); }
            R.sync();
            u64 span = 2 * (j.A - 1) * j.sL + 1;
            if (span < w.n) {
                for (u64 r = 0; r < rounds && !R.stop; r++) {
                    rng = splitmix(rng); u64 b0 = rng % (w.n - span);
                    std::vector<u8*> ag; for (u64 k = 0; k < j.A; k++) ag.push_back(w.base + (b0 + 2 * k * j.sL) * 64);
                    for (u8* a : ag) { Line l; j.g->make(0, a, true, l); io::store(a, l); }
                    for (u64 it = 0; it < acts; it++) {
                        for (u8* a : ag) { (void)*(volatile u64*)a; _mm_clflush(a); }
                        _mm_mfence();
                    }
                    for (size_t k = 0; k < ag.size(); k++) { u64 i = (ag[k] - w.base) / 64; Line l; j.g->make(w.g + i, ag[k], false, l); io::store(ag[k], l); }
                    R.done.fetch_add(acts * j.A * 64, std::memory_order_relaxed);
                }
            }
            R.sync();
            for (u64 i = 0; i < w.n && !R.stop; i++) {
                const u8* a = w.base + i * 64; Line e, act; j.g->make(w.g + i, a, false, e); R.vtick();
                if (!io::verify(a, e, act)) R.report(tid, a, e, act, "hammer-victim");
                if (((i + 1) & 0xFFFF) == 0) R.tick();
            }
            R.sync();
        }
    });
}
