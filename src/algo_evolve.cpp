// algo_evolve.cpp - data that changes from pass to pass (lost-write detector).
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

// ------------------------------------------------------------------ algo: evolve (data that changes from pass to pass)
// Every other data test writes a pattern once, or rewrites IDENTICAL data (rwstorm), so a write that never lands leaves exactly the bytes
// the test expects: a lost write is invisible to it. Here every line has its own update period p(line) in 1..maxp passes and after pass P
// must hold generation P / p(line); its data is a pure function of (line, generation, seed), so nothing is stored on the side. A pass
// rewrites only the lines whose period divides P, then verifies EVERY line:
//   - a rewritten line that still shows its previous generation = a lost / misdirected write (looks perfectly valid in a static test)
//   - a line that was NOT rewritten but changed = a stray write, disturb or leak from the traffic around it
// Memory therefore holds a mix of generations at all times, and old data must survive while its neighbours are being rewritten.
void algo_evolve(Run& R, const Params& P) {
    auto vars = build_vars(P);   // data = hash: one variant per seed (seeds = N)
    OrderSpec wo = ospec(P, "order", "random"), vo = ospec(P, "verify_order", "seq");
    auto strides = stride_list(P, wo.k == Order::STRIDE || vo.k == Order::STRIDE);
    int width = (int)P.num("width", 32); bool nt = P.flag("nt", false), fl = P.flag("flush", true), cross = P.flag("cross", false);
    u64 passes = std::max<u64>(1, P.num("passes", 16)), maxp = std::max<u64>(1, P.num("period", 8)), wl = R.window_lines(P);
    struct Job { std::string label; u64 seed, stride; };
    std::vector<Job> jobs;
    for (auto& v : vars) for (u64 s : strides) jobs.push_back({"evolve " + v.label + (strides.size() > 1 ? " stride=" + std::to_string(s * 64) + "B" : ""), v.g.seed, s});
    u64 base = R.winctr[R.testName]; R.winctr[R.testName] += jobs.size();
    R.parallel([&](int tid) {
        for (size_t ji = 0; ji < jobs.size(); ji++) {
            if (R.stop) break;
            const Job& j = jobs[ji]; if (tid == 0) R.set_label(j.label);
            View w = R.view(tid, wl, base + ji), vw = R.view(cross ? (tid + 1) % R.T : tid, wl, base + ji);
            Order ow, ov; ow.init(wo.k, w.n, j.stride, 21 + ji); ov.init(vo.k, vw.n, j.stride, 55 + ji);
            dispatch(width, nt, fl, [&](auto Wc, auto NTc, auto FLc) {
                using io = IO<decltype(Wc)::value, decltype(NTc)::value, decltype(FLc)::value>;
                walk(R, ow, wo.rev, [&](u64 i) { Line l; evo_make(w.g + i, 0, j.seed, l); io::store(w.base + i * 64, l); });   // generation 0 everywhere
                _mm_sfence(); R.sync();
                for (u64 p = 1; p <= passes && !R.stop; p++) {
                    walk(R, ow, wo.rev, [&](u64 i) {
                        u64 g = w.g + i, per = evo_period(g, j.seed, maxp); if (p % per) return;   // not this line's turn: it must keep its old data
                        Line l; evo_make(g, p / per, j.seed, l); io::store(w.base + i * 64, l);
                    });
                    _mm_sfence(); R.sync();
#ifdef DDR5TEST_INJECT  // self-test only, last pass: (1) undo one rewrite = a lost write, (2) flip one bit in a line that was not rewritten
                    if (p == passes) {
                        if (tid == 0 && ji == 0) {
                            bool lost = false, flip = false;
                            for (u64 i = 0; i < w.n && !(lost && flip); i++) {
                                u64 g = w.g + i, per = evo_period(g, j.seed, maxp); u8* a = w.base + i * 64;
                                if (!lost && p % per == 0) { Line old; evo_make(g, p / per - 1, j.seed, old); io::store(a, old); lost = true; }
                                else if (!flip && p % per != 0) { a[8] ^= 0x20; _mm_clflush(a); flip = true; }
                            }
                            _mm_sfence(); _mm_mfence();
                        }
                        R.sync();
                    }
#endif
                    walk(R, ov, vo.rev, [&](u64 i) {
                        const u8* a = vw.base + i * 64; u64 g = vw.g + i; Line e, act; evo_make(g, p / evo_period(g, j.seed, maxp), j.seed, e); R.vtick();
                        if (!io::verify(a, e, act)) R.report(tid, a, e, act, "evolve-verify");
                    });
                    R.sync();
                }
            });
            R.sync();
        }
    });
}
