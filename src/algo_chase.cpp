// algo_chase.cpp - random pointer chase, K independent chains.
#include "algo.h"
#include "gen.h"
#include "inject.h"
#include "lineio.h"
#include "log.h"
#include "order.h"
#include "platform.h"
#include "run.h"
#include "seed.h"
#include <cstring>
#include <string>
#include <vector>

namespace ddr5 {

// ------------------------------------------------------------------ options (the only place chase reads its keys)
// `window_mb` (Run::window_lines) belongs to a shared helper; chase has no data variants.
const std::set<std::string>& ChaseOpts::keys() {
    static const std::set<std::string> k = {"mlp", "seeds"};
    return k;
}
ChaseOpts ChaseOpts::from(const Params& P) {
    ChaseOpts o;
    o.mlp = P.nums("mlp", "12");
    if (o.mlp.empty())
        o.mlp.push_back(12);
    o.seeds = std::max<u64>(1, P.num("seeds", 2));
    return o;
}

// ------------------------------------------------------------------ algo: chase (random pointer chase, K independent
// chains = max bank-level parallelism)
void algo_chase(Run& R, const Params& P) {
    const ChaseOpts opt = ChaseOpts::from(P);
    u64 wl = R.window_lines(P);
    u64 base = R.winctr[R.testName];
    R.winctr[R.testName] += opt.seeds * opt.mlp.size();
    R.parallel([&](int tid) {
        for (u64 sd = 0; sd < opt.seeds; sd++)
            for (size_t mi = 0; mi < opt.mlp.size(); mi++) {
                if (R.stop)
                    break;
                u64 K = std::max<u64>(1, opt.mlp[mi]);
                if (tid == 0)
                    R.set_label("chase seed " + std::to_string(sd) + " mlp=" + std::to_string(K));
                View w = R.view(tid, wl, base + sd * opt.mlp.size() + mi);
                Order o;
                u64 seed = splitmix(sd * 31 + tid + R.salt + mi * 977), salt = splitmix(seed ^ 0x5A5A);
                o.init(Order::RANDOM, w.n, 1, seed, R.salt);
                using io = IO<32, 0, 1>;
                auto make = [&](u64 idx, u64 next, Line& l) {
                    l.w[0] = next;
                    l.w[1] = ~next ^ salt;
                    for (int k = 2; k < 8; k++)
                        l.w[k] = splitmix((idx << 3 | k) ^ seed);
                };
                for (u64 i = 0; i < w.n; i++) {
                    u64 idx = o.at(i), nx = o.at(i + 1 == w.n ? 0 : i + 1);
                    Line l;
                    make(idx, nx, l);
                    io::store(w.base + idx * kLineBytes, l);
                    if (((i + 1) & 0xFFFF) == 0) {
                        R.tick();
                        if (R.stop)
                            break;
                    }
                }
                _mm_sfence();
                R.sync();
                u64 steps = w.n / K;
                DDR5_INJECT(chase_after_fill, tid, sd, K, w, o, steps);
                // p[k]: line chain k is on; pos[k]: its place in the order (where the chain should be, whatever the
                // memory says)
                std::vector<u64> p(K), pos(K);
                for (u64 k = 0; k < K; k++) {
                    pos[k] = k * steps;
                    p[k] = o.at(pos[k]);
                }
                // What the K loads of one step returned. A step is done in three parts: ALL K loads first (the K chains
                // are independent, so their cache misses overlap: that is the memory-level parallelism this test is
                // named for), then the K flushes, then the K checks. The old loop did load, flush and check for one
                // chain before it touched the next, which kept the loads of different chains from overlapping.
                std::vector<Line> act(K);
                for (u64 st = 0; st < steps && !R.stop; st++) {
                    CBAR();
                    for (u64 k = 0; k < K; k++)
                        IO<32, 0, 0>::load(w.base + p[k] * kLineBytes, act[k]);
                    for (u64 k = 0; k < K; k++)
                        flush_line(w.base + p[k] * kLineBytes);
                    CBAR();
                    for (u64 k = 0; k < K; k++) {
                        const u8* a = w.base + p[k] * kLineBytes;
                        Line e;
                        make(p[k], act[k].w[0], e);
                        R.vtick();
                        if (memcmp(&act[k], &e, kLineBytes) != 0 || act[k].w[0] >= w.n) {
                            R.report(tid, a, e, act[k], "chase");
                            // The stored pointer cannot be trusted. Carry on from the order's own next line, so the
                            // rest of the chain is still checked and every bad line is counted.
                            p[k] = o.at(pos[k] + 1 == w.n ? 0 : pos[k] + 1);
                        } else
                            p[k] = act[k].w[0]; // the next address comes from the data just loaded: a dependent load
                        pos[k]++;
                    }
                    if (((st + 1) & 0xFFFF) == 0) {
                        R.add_done(kTickLines * kLineBytes * K);
                    }
                }
                R.sync();
            }
    });
}

} // namespace ddr5
