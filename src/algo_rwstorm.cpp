// algo_rwstorm.cpp - concurrent read + write traffic on the same memory.
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

// ------------------------------------------------------------------ algo: rwstorm (concurrent read + write traffic on
// the same memory) Even threads keep rewriting their chunk with IDENTICAL data while their odd partner thread verifies
// that chunk at the same time. Stores and loads are in flight together (bus turnaround, write queue drain,
// read-after-write hazards in the controller, power-delivery load) yet every read must still match. Finally every
// thread verifies its own chunk.
void algo_rwstorm(Run& R, const Params& P) {
    auto vars = build_vars(P);
    OrderSpec wo = ospec(P, "order", "random"), vo = ospec(P, "verify_order", "seq");
    auto strides = stride_list(P, wo.k == Order::STRIDE || vo.k == Order::STRIDE);
    int width = (int)P.num("width", 32);
    bool nt = P.flag("nt", false), fl = P.flag("flush", true);
    u64 passes = std::max<u64>(1, P.num("passes", 4)), wl = R.window_lines(P);
    struct Job {
        std::string label;
        const Gen* g;
        u64 stride;
    };
    std::vector<Job> jobs;
    for (auto& v : vars)
        for (u64 s : strides)
            jobs.push_back({v.label, &v.g, s});
    u64 base = R.winctr[R.testName];
    R.winctr[R.testName] += jobs.size();
    R.parallel([&](int tid) {
        for (size_t ji = 0; ji < jobs.size(); ji++) {
            if (R.stop)
                break;
            const Job& j = jobs[ji];
            if (tid == 0)
                R.set_label("rwstorm " + j.label);
            bool writer = (tid % 2) == 0;
            int partner = writer ? tid : tid - 1;
            View w = R.view(tid, wl, base + ji), pv = R.view(partner, wl, base + ji);
            Order ow, ov, of;
            ow.init(wo.k, w.n, j.stride, 3 + ji);
            ov.init(vo.k, pv.n, j.stride, 9 + ji);
            of.init(Order::SEQ, w.n, 1, 0);
            dispatch(width, nt, fl, [&](auto Wc, auto NTc, auto FLc) {
                using io = IO<decltype(Wc)::value, decltype(NTc)::value, decltype(FLc)::value>;
                auto wr = [&](u64 i) {
                    u8* a = w.base + i * 64;
                    Line l;
                    j.g->make(w.g + i, a, false, l);
                    io::store(a, l);
                };
                auto rd = [&](const View& v) {
                    return [&, v](u64 i) {
                        const u8* a = v.base + i * 64;
                        Line e, act;
                        j.g->make(v.g + i, a, false, e);
                        R.vtick();
                        if (!io::verify(a, e, act))
                            R.report(tid, a, e, act, "rwstorm-read");
                    };
                };
                walk(R, of, false, wr);
                _mm_sfence();
                R.sync(); // initial fill of own chunk
                for (u64 p = 0; p < passes && !R.stop; p++) {
                    if (writer)
                        walk(R, ow, wo.rev, wr); // rewrite same data
                    else
                        walk(R, ov, vo.rev, rd(pv)); // concurrently verify partner's chunk
                }
                _mm_sfence();
                R.sync();
                walk(R, of, false, rd(w)); // final check of own chunk
            });
            R.sync();
        }
    });
}
