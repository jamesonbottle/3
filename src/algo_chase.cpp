// algo_chase.cpp - random pointer chase, K independent chains.
#include "algo.h"
#include "gen.h"
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

// ------------------------------------------------------------------ algo: chase (random pointer chase, K independent
// chains = max bank-level parallelism)
void algo_chase(Run& R, const Params& P) {
    auto mlps = P.nums("mlp", "12");
    if (mlps.empty())
        mlps.push_back(12);
    u64 seeds = std::max<u64>(1, P.num("seeds", 2)), wl = R.window_lines(P);
    u64 base = R.winctr[R.testName];
    R.winctr[R.testName] += seeds * mlps.size();
    R.parallel([&](int tid) {
        for (u64 sd = 0; sd < seeds; sd++)
            for (size_t mi = 0; mi < mlps.size(); mi++) {
                if (R.stop)
                    break;
                u64 K = std::max<u64>(1, mlps[mi]);
                if (tid == 0)
                    R.set_label("chase seed " + std::to_string(sd) + " mlp=" + std::to_string(K));
                View w = R.view(tid, wl, base + sd * mlps.size() + mi);
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
                    io::store(w.base + idx * 64, l);
                    if (((i + 1) & 0xFFFF) == 0) {
                        R.tick();
                        if (R.stop)
                            break;
                    }
                }
                _mm_sfence();
                R.sync();
                u64 steps = w.n / K;
                std::vector<u64> p(K), bad(K, 0);
                for (u64 k = 0; k < K; k++)
                    p[k] = o.at(k * steps);
                for (u64 st = 0; st < steps && !R.stop; st++) {
                    for (u64 k = 0; k < K; k++) {
                        if (bad[k])
                            continue;
                        const u8* a = w.base + p[k] * 64;
                        Line act, e;
                        CBAR();
                        IO<32, 0, 0>::load(a, act);
                        _mm_clflush(a);
                        CBAR();
                        make(p[k], act.w[0], e);
                        R.vtick();
                        if (memcmp(&act, &e, 64) != 0 || act.w[0] >= w.n) {
                            R.report(tid, a, e, act, "chase");
                            bad[k] = 1;
                            continue;
                        }
                        p[k] = act.w[0];
                    }
                    if (((st + 1) & 0xFFFF) == 0) {
                        R.progress.done.fetch_add(0x10000ull * 64 * K, std::memory_order_relaxed);
                    }
                }
                R.sync();
            }
    });
}

} // namespace ddr5
