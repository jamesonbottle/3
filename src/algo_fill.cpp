// algo_fill.cpp - write -> [disturb] -> [dwell] -> verify xN.
#include "algo.h"
#include "gen.h"
#include "inject.h"
#include "lineio.h"
#include "log.h"
#include "order.h"
#include "platform.h"
#include "run.h"
#include "seed.h"
#include <string>
#include <vector>

namespace ddr5 {

// ------------------------------------------------------------------ algo: fill (write -> [disturb] -> [dwell] ->
// verify xN)
void algo_fill(Run& R, const Params& P) {
    auto vars = build_vars(P, R.salt, R.rng, R.log);
    OrderSpec wo = ospec(P, "order", "seq", R.log), vo = ospec(P, "verify_order", P.str("order", "seq").c_str(), R.log);
    bool strided = wo.k == Order::STRIDE || vo.k == Order::STRIDE;
    auto strides = stride_list(P, strided);
    bool both = P.flag("both", false), cross = P.flag("cross", false);
    int width = (int)P.num("width", 32);
    bool nt = P.flag("nt", false), fl = P.flag("flush", true);
    u64 reads = std::max<u64>(1, P.num("reads", 1)), wl = R.window_lines(P);
    auto dwells = P.nums("dwell_ms", "0");
    if (dwells.empty())
        dwells.push_back(0);
    std::vector<int> dmodes;
    for (auto& w : P.words("disturb")) {
        int m = parse_dmode(lower(w));
        if (m >= 0)
            dmodes.push_back(m);
    }
    if (dmodes.empty())
        dmodes.push_back(-1);
    u64 dpasses = std::max<u64>(1, P.num("disturb_passes", 2));
    if (dmodes[0] >= 0 && !disturb_possible(R, wl, R.testName.c_str()))
        dmodes.assign(1, -1);
    struct Job {
        std::string label;
        const Gen* g;
        bool inv;
        u64 stride;
        u64 dwell;
        int dmode;
    };
    std::vector<Job> jobs;
    const char* dn[] = {"r", "w", "rw"};
    for (auto& v : vars)
        for (u64 s : strides)
            for (int inv = 0; inv <= (both ? 1 : 0); inv++)
                for (u64 dw : dwells)
                    for (int dm : dmodes)
                        jobs.push_back({v.label + (inv ? " [inverted]" : "") +
                                            (strided ? " stride=" + std::to_string(s * 64) + "B" : "") +
                                            (dwells.size() > 1 ? " dwell=" + std::to_string(dw) + "ms" : "") +
                                            (dmodes.size() > 1 ? std::string(" disturb=") + dn[dm] : ""),
                                        &v.g, (bool)inv, s, dw, dm});
    u64 base = R.winctr[R.testName];
    R.winctr[R.testName] += jobs.size();
    R.parallel([&](int tid) {
        for (size_t ji = 0; ji < jobs.size(); ji++) {
            if (R.stop)
                break;
            const Job& j = jobs[ji];
            if (tid == 0)
                R.set_label(j.label);
            View w = R.view(tid, wl, base + ji), vw = R.view(cross ? (tid + 1) % R.layout.T : tid, wl, base + ji);
            Order ow, ov;
            ow.init(wo.k, w.n, j.stride, 1 + ji, R.salt);
            ov.init(vo.k, vw.n, j.stride, 77 + ji, R.salt);
            dispatch(width, nt, fl, [&](auto Wc, auto NTc, auto FLc) {
                using io = IO<decltype(Wc)::value, decltype(NTc)::value, decltype(FLc)::value>;
                walk(R, ow, wo.rev, [&](u64 i) {
                    u8* a = w.base + i * 64;
                    Line l;
                    j.g->make(w.g + i, a, j.inv, l);
                    io::store(a, l);
                });
                _mm_sfence();
                R.sync();
                DDR5_INJECT(after_fill, R, tid, ji, w);
                if (j.dmode >= 0) {
                    disturb_traffic(R, R.dview(tid, wl, base + ji), j.dmode, dpasses, 0xD157 + tid + ji + R.salt);
                    _mm_sfence();
                    R.sync();
                }
                if (j.dwell) {
                    if (tid == 0)
                        R.sleep_ms(j.dwell);
                    R.sync();
                }
                for (u64 r = 0; r < reads && !R.stop; r++)
                    walk(R, ov, vo.rev, [&](u64 i) {
                        const u8* a = vw.base + i * 64;
                        Line e, act;
                        j.g->make(vw.g + i, a, j.inv, e);
                        R.vtick();
                        if (!io::verify(a, e, act))
                            R.report(tid, a, e, act, "verify");
                    });
            });
            R.sync();
        }
    });
}

} // namespace ddr5
