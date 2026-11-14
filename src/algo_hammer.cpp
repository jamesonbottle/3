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

namespace ddr5 {

// ------------------------------------------------------------------ options (the only place hammer reads its keys)
// `window_mb` (Run::window_lines) and the data keys (build_vars) belong to shared helpers.
const std::set<std::string>& HammerOpts::keys() {
    static const std::set<std::string> k = {"acts", "aggressors", "rounds", "rowstride"};
    return k;
}
HammerOpts HammerOpts::from(const Params& P) {
    HammerOpts o;
    o.rowstride = P.nums("rowstride", "256K 1M");
    o.aggressors = P.nums("aggressors", "2 9");
    o.acts = P.num("acts", 400000);
    o.rounds = P.num("rounds", 24);
    return o;
}

// ------------------------------------------------------------------ algo: hammer (n-sided row hammer / activation-rate
// stress)
void algo_hammer(Run& R, const Params& P) {
    auto vars = build_vars(P, R.salt, R.rng, R.log);
    const HammerOpts opt = HammerOpts::from(P);
    u64 wl = R.window_lines(P);
    struct Job {
        std::string label;
        const Gen* g;
        u64 sL;
        u64 A;
    };
    std::vector<Job> jobs;
    for (auto& v : vars)
        for (u64 s : opt.rowstride)
            for (u64 a : opt.aggressors)
                jobs.push_back({v.label + " rowstride=" + std::to_string(s) + "B " + std::to_string(a) + "-sided", &v.g,
                                std::max<u64>(1, s / kLineBytes), a});
    u64 base = R.winctr[R.testName];
    R.winctr[R.testName] += jobs.size();
    R.parallel([&](int tid) {
        u64 rng = splitmix(0xABCD + tid * 7919 + R.salt);
        for (size_t ji = 0; ji < jobs.size(); ji++) {
            if (R.stop)
                break;
            const Job& j = jobs[ji];
            if (tid == 0)
                R.set_label("hammer " + j.label);
            View w = R.view(tid, wl, base + ji);
            using io = IO<32, 0, 1>;
            for (u64 i = 0; i < w.n; i++) {
                u8* a = w.base + i * kLineBytes;
                Line l;
                j.g->make(w.g + i, a, false, l);
                io::store(a, l);
                if (((i + 1) & 0xFFFF) == 0) {
                    R.tick();
                    if (R.stop.load(std::memory_order_relaxed))
                        break; // the rounds and the victim check below are skipped too: the data is only partly written
                }
            }
            R.sync();
            u64 span = 2 * (j.A - 1) * j.sL + 1;
            if (span < w.n) {
                for (u64 r = 0; r < opt.rounds && !R.stop; r++) {
                    rng = splitmix(rng);
                    u64 b0 = rng % (w.n - span);
                    std::vector<u8*> ag;
                    for (u64 k = 0; k < j.A; k++)
                        ag.push_back(w.base + (b0 + 2 * k * j.sL) * kLineBytes);
                    for (u8* a : ag) {
                        Line l;
                        j.g->make(0, a, true, l);
                        io::store(a, l);
                    }
                    for (u64 it = 0; it < opt.acts; it++) {
                        for (u8* a : ag) {
                            (void)*(volatile u64*)a;
                            _mm_clflush(a);
                        }
                        _mm_mfence();
                        if ((it & 0x3FFF) == 0x3FFF && R.stop.load(std::memory_order_relaxed))
                            break; // the aggressors are restored below; the victim check is skipped after a stop
                    }
                    for (size_t k = 0; k < ag.size(); k++) {
                        u64 i = (ag[k] - w.base) / kLineBytes;
                        Line l;
                        j.g->make(w.g + i, ag[k], false, l);
                        io::store(ag[k], l);
                    }
                    R.add_done(opt.acts * j.A * kLineBytes);
                }
            }
            R.sync();
            for (u64 i = 0; i < w.n && !R.stop; i++) {
                const u8* a = w.base + i * kLineBytes;
                Line e, act;
                j.g->make(w.g + i, a, false, e);
                R.vtick();
                if (!io::verify(a, e, act))
                    R.report(tid, a, e, act, "hammer-victim");
                if (((i + 1) & 0xFFFF) == 0)
                    R.tick();
            }
            R.sync();
        }
    });
}

} // namespace ddr5
