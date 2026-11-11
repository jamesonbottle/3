// algo_probe.cpp - row-hit vs row-conflict latency by address distance (informational, not pass/fail).
#include "algo.h"
#include "gen.h"
#include "lineio.h"
#include "log.h"
#include "order.h"
#include "platform.h"
#include "run.h"
#include "seed.h"
#include <algorithm>
#include <string>
#include <vector>

namespace ddr5 {

// ------------------------------------------------------------------ algo: probe (row-hit vs row-conflict latency by
// address distance) Informational, not pass/fail. Reads line A, then times a read of line B = A + stride (both flushed
// from cache first). B in the same bank but a different row => row conflict (PRE + ACT + CAS: ~tRP + tRCD + CL),
// noticeably slower. B in another bank, or the same row => faster. The slow-fraction per stride shows which address
// bits feed the bank function, and (slow - fast) approximates tRP + tRCD as seen from the core (includes noise, so
// treat as +-several ns).
void algo_probe(Run& R, const Params& P) {
    auto strides = P.nums("stride", "64 128 256 512 1K 2K 4K 8K 16K 32K 64K 128K 256K 512K 1M 2M 4M 8M 16M");
    u64 samples = std::max<u64>(1000, P.num("samples", 20000));
    R.parallel([&](int tid) {
        if (tid != 0)
            return; // single thread, rest idle, so measurements are clean
        const Chunk& c = R.layout.chunks[0];
        R.set_label("probe: calibrating TSC");
        double t0 = plat::now();
        u64 c0 = __rdtsc();
        R.sleep_ms(80);
        double t1 = plat::now();
        u64 c1 = __rdtsc();
        double ghz = (double)(c1 - c0) / (t1 - t0) / 1e9;
        std::vector<double> oh; // measurement overhead
        for (int i = 0; i < 2001; i++) {
            _mm_lfence();
            u64 a = __rdtsc();
            _mm_lfence();
            u64 b = __rdtsc();
            _mm_lfence();
            oh.push_back((double)(b - a));
        }
        std::sort(oh.begin(), oh.end());
        double ovh = oh[oh.size() / 2];
        std::vector<std::vector<double>> res(strides.size());
        std::vector<double> all;
        u64 rng = splitmix(0x9876);
        for (size_t si = 0; si < strides.size() && !R.stop; si++) {
            u64 sl = std::max<u64>(1, strides[si] / 64);
            if (sl + 2 >= c.lines)
                continue;
            R.set_label("probe stride " + std::to_string(strides[si]) + "B");
            for (u64 k = 0; k < samples && !R.stop; k++) {
                rng = splitmix(rng);
                u64 bl = rng % (c.lines - sl - 1);
                const u8* A = c.base + bl * 64;
                const u8* B = A + sl * 64;
                (void)*(volatile const u64*)A;
                (void)*(volatile const u64*)B; // prime TLB
                _mm_clflush(A);
                _mm_clflush(B);
                _mm_mfence();
                CBAR();
                (void)*(volatile const u64*)A;
                _mm_lfence();
                u64 a = __rdtsc();
                _mm_lfence();
                (void)*(volatile const u64*)B;
                _mm_lfence();
                u64 b = __rdtsc();
                _mm_lfence();
                CBAR();
                double ns = std::max(0.0, ((double)(b - a) - ovh) / ghz);
                res[si].push_back(ns);
                all.push_back(ns);
            }
        }
        if (all.empty())
            return;
        std::sort(all.begin(), all.end());
        auto pct = [](std::vector<double>& v, double p) {
            return v[std::min(v.size() - 1, (size_t)(p * v.size()))];
        };
        double lo = all[all.size() / 10], hi = pct(all, 0.97), thr = (lo + hi) / 2;
        R.log.print_clear();
        R.log.emitf(
            "    TSC %.3f GHz, overhead %.0f cycles removed. fast cluster ~%.0f ns, slow cluster ~%.0f ns, threshold "
            "%.0f ns\n",
            ghz, ovh, lo, hi, thr);
        R.log.emitf("    stride        median   p10    p90   slow%%   (slow = likely same bank, different row)\n");
        double minSlow = 100, maxSlow = 0;
        for (size_t si = 0; si < strides.size(); si++) {
            if (res[si].empty()) {
                continue;
            }
            auto& v = res[si];
            std::sort(v.begin(), v.end());
            size_t slow = 0;
            for (double x : v)
                if (x > thr)
                    slow++;
            double sp = 100.0 * slow / v.size();
            minSlow = std::min(minSlow, sp);
            maxSlow = std::max(maxSlow, sp);
            R.log.emitf("    %-12llu %6.0f %6.0f %6.0f  %5.1f%%\n", (unsigned long long)strides[si], pct(v, 0.5),
                        pct(v, 0.1), pct(v, 0.9), sp);
        }
        if (maxSlow - minSlow >= 15.0) {
            R.log.emitf(
                "    slow - fast = %.0f ns  ~ tRP + tRCD (core-visible, noisy; compare with BIOS/ZenTimings/HWiNFO "
                "values)\n",
                hi - lo);
            R.log.emitf(
                "    Strides with a high slow%% hit the same bank in another row; the bits that flip between them "
                "feed the bank/row hash.\n");
        } else {
            R.log.emitf(
                "    No stride separates into a distinct fast/slow group (slow%% spread %.1f..%.1f): bank structure "
                "NOT resolvable here,\n"
                "    so no tRP+tRCD estimate is given. Typical causes: virtual machine, small-page TLB noise (use "
                "large pages), SMT/background load.\n",
                minSlow, maxSlow);
        }
    });
}

} // namespace ddr5
