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

// ------------------------------------------------------------------ options (the only place fill reads its keys)
// `stride` (stride_list), `window_mb` (Run::window_lines) and the data keys (build_vars) belong to shared helpers.
const std::set<std::string>& FillOpts::keys() {
    static const std::set<std::string> k = {"both", "cross", "disturb", "disturb_passes", "dwell_ms", "flush",
                                            "nt",   "order", "reads",   "verify_order",   "width"};
    return k;
}
FillOpts FillOpts::from(const Params& P, const Logger& log) {
    FillOpts o;
    o.write = ospec(P, "order", "seq", log);
    o.verify = ospec(P, "verify_order", P.str("order", "seq").c_str(), log);
    o.strided = o.write.k == Order::STRIDE || o.verify.k == Order::STRIDE;
    o.strides = stride_list(P, o.strided);
    o.both = P.flag("both", false);
    o.cross = P.flag("cross", false);
    o.width = (int)P.num("width", 32);
    o.nt = P.flag("nt", false);
    o.flush = P.flag("flush", true);
    o.reads = std::max<u64>(1, P.num("reads", 1));
    o.dwell_ms = P.nums("dwell_ms", "0");
    if (o.dwell_ms.empty())
        o.dwell_ms.push_back(0);
    for (auto& w : P.words("disturb")) {
        int m = parse_dmode(lower(w));
        if (m >= 0)
            o.disturb.push_back(m);
    }
    if (o.disturb.empty())
        o.disturb.push_back(-1);
    o.disturb_passes = std::max<u64>(1, P.num("disturb_passes", 2));
    return o;
}

// ------------------------------------------------------------------ algo: fill (write -> [disturb] -> [dwell] ->
// verify xN)
void algo_fill(Run& R, const Params& P) {
    auto vars = build_vars(P, R.salt, R.rng, R.log);
    const FillOpts o = FillOpts::from(P, R.log);
    const OrderSpec &wo = o.write, &vo = o.verify; // wo / vo: write order and verify order
    u64 wl = R.window_lines(P);
    std::vector<int> dmodes = o.disturb;
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
        for (u64 s : o.strides)
            for (int inv = 0; inv <= (o.both ? 1 : 0); inv++)
                for (u64 dw : o.dwell_ms)
                    for (int dm : dmodes)
                        jobs.push_back({v.label + (inv ? " [inverted]" : "") +
                                            (o.strided ? " stride=" + std::to_string(s * kLineBytes) + "B" : "") +
                                            (o.dwell_ms.size() > 1 ? " dwell=" + std::to_string(dw) + "ms" : "") +
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
            View w = R.view(tid, wl, base + ji), vw = R.view(o.cross ? (tid + 1) % R.layout.T : tid, wl, base + ji);
            Order ow, ov; // ow / ov: the write and the verify walk over this job's windows
            ow.init(wo.k, w.n, j.stride, 1 + ji, R.salt);
            ov.init(vo.k, vw.n, j.stride, 77 + ji, R.salt);
            dispatch(o.width, o.nt, o.flush, [&](auto Wc, auto NTc, auto FLc) {
                using io = IO<decltype(Wc)::value, decltype(NTc)::value, decltype(FLc)::value>;
                walk(R, ow, wo.rev, [&](u64 i) {
                    u8* a = w.base + i * kLineBytes;
                    Line l;
                    j.g->make(w.g + i, a, j.inv, l);
                    io::store(a, l);
                });
                _mm_sfence();
                R.sync();
                DDR5_INJECT(after_fill_throw, tid);
                DDR5_INJECT(after_fill, R, tid, ji, w);
                if (j.dmode >= 0) {
                    disturb_traffic(R, R.dview(tid, wl, base + ji), j.dmode, o.disturb_passes,
                                    0xD157 + tid + ji + R.salt);
                    _mm_sfence();
                    R.sync();
                }
                if (j.dwell) {
                    if (tid == 0)
                        R.sleep_ms(j.dwell);
                    R.sync();
                }
                for (u64 r = 0; r < o.reads && !R.stop; r++)
                    walk(R, ov, vo.rev, [&](u64 i) {
                        const u8* a = vw.base + i * kLineBytes;
                        Line e, act;
                        j.g->make(vw.g + i, a, j.inv, e);
                        R.vtick();
                        if (!io::verify(a, e, act))
                            R.report(tid, a, e, act, "verify");
                    });
            });
            R.sync();
        }
        DDR5_INJECT(extra_sync, R, tid);
    });
}

} // namespace ddr5
