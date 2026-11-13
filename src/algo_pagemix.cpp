// algo_pagemix.cpp - stressapptest-style page shuffle.
#include "algo.h"
#include "gen.h"
#include "inject.h"
#include "lineio.h"
#include "log.h"
#include "order.h"
#include "platform.h"
#include "run.h"
#include "seed.h"
#include <atomic>
#include <cstdlib>
#include <memory>
#include <vector>

namespace ddr5 {

// ------------------------------------------------------------------ algo: pagemix (stressapptest-style page shuffle)
// All memory is split into 4 KB pages and every page gets a random pattern from the stressapptest-style table. Threads
// then pick random page pairs and (a) copy source -> destination while verifying the source line by line, (b) invert a
// page in place 4 times (up, down, down, up - net identity, with flushes), (c) refill a page with a new pattern, or (d)
// re-check a page. The destination takes the source's pattern, so data keeps moving between cores, banks and channels
// while every line stays predictable. Hot (cache-resident) and flushed (DRAM round-trip) variants are mixed. Finally
// every page is verified.
struct PagePat {
    u16 pat;
    u8 rep, inv;
};
template <int FL>
static void pm_fill(u8* p, const Gen& g, bool inv) {
    using io = IO<32, 0, FL>;
    for (int l = 0; l < kLinesPerPage; l++) {
        Line e;
        g.make(l, p + l * kLineBytes, inv, e);
        io::store(p + l * kLineBytes, e);
    }
}
template <int FL>
static void pm_check(Run& R, int tid, u8* p, const Gen& g, bool inv, const char* ph) {
    using io = IO<32, 0, FL>;
    for (int l = 0; l < kLinesPerPage; l++) {
        Line e, a;
        g.make(l, p + l * kLineBytes, inv, e);
        R.vtick();
        if (!io::verify(p + l * kLineBytes, e, a)) {
            R.report(tid, p + l * kLineBytes, e, a, ph);
            io::store(p + l * kLineBytes, e);
        }
    }
}
template <int FL>
static void pm_copy(Run& R, int tid, u8* s, u8* d, const Gen& g, bool inv) {
    using io = IO<32, 0, FL>;
    for (int l = 0; l < kLinesPerPage; l++) {
        Line e, a;
        g.make(l, s + l * kLineBytes, inv, e);
        R.vtick();
        if (!io::verify(s + l * kLineBytes, e, a)) {
            R.report(tid, s + l * kLineBytes, e, a, "pagemix-src");
            io::store(s + l * kLineBytes, e);
        }
        io::store(d + l * kLineBytes,
                  e); // what was read equals e when the check passes; after a failure the destination gets good data
    }
}
template <int FL>
static void pm_invert(u8* p) {
    static const int dir[4] = {0, 1, 1, 0}; // up, down, down, up
    const __m256i ones = _mm256_set1_epi32(-1);
    for (int pass = 0; pass < 4; pass++)
        for (int k = 0; k < kLinesPerPage; k++) {
            u8* a = p + (dir[pass] ? kLinesPerPage - 1 - k : k) * kLineBytes;
            CBAR();
            __m256i x = _mm256_xor_si256(_mm256_load_si256((const __m256i*)a), ones),
                    y = _mm256_xor_si256(_mm256_load_si256((const __m256i*)a + 1), ones);
            _mm256_store_si256((__m256i*)a, x);
            _mm256_store_si256((__m256i*)a + 1, y);
            if constexpr (FL)
                _mm_clflush(a);
            CBAR();
        }
}
const std::set<std::string>& PagemixOpts::keys() {
    static const std::set<std::string> k = {"seconds"};
    return k;
}
PagemixOpts PagemixOpts::from(const Params& P) {
    PagemixOpts o;
    o.seconds = (double)P.num("seconds", 30);
    return o;
}
void algo_pagemix(Run& R, const Params& P) {
    const PagemixOpts opt = PagemixOpts::from(P);
    const auto& tab = sat_table();
    u64 np = R.layout.regionBytes / kPageBytes;
    if (np < (u64)R.layout.T * 8) {
        R.log.emitf("    pagemix: region too small, skipped\n");
        return;
    }
    std::unique_ptr<std::atomic<u8>[]> busy(new std::atomic<u8>[np]);
    for (u64 i = 0; i < np; i++)
        busy[i].store(0, std::memory_order_relaxed);
    std::vector<PagePat> pp(np);
    std::vector<u8> filled(np,
                           0); // a page the fill phase never reached has no pattern yet: the final verify must skip it
    u64 wsum = 0;
    for (auto& t : tab)
        for (int w = 0; w < 4; w++)
            wsum += t.weight[w];
    auto pick = [&](u64 r) {
        PagePat q{};
        u64 x = r % (wsum * 2);
        q.inv = (u8)(x & 1);
        x >>= 1;
        for (size_t i = 0; i < tab.size(); i++)
            for (int w = 0; w < 4; w++) {
                if (x < (u64)tab[i].weight[w]) {
                    q.pat = (u16)i;
                    q.rep = (u8)w;
                    return q;
                }
                x -= tab[i].weight[w];
            }
        return q;
    };
    auto gen = [&](const PagePat& q) {
        Gen g;
        g.kind = 3;
        g.sp = &tab[q.pat];
        g.rep = q.rep;
        return g;
    };
    R.parallel([&](int tid) {
        u64 rng = splitmix(0x5A7 + tid * 7919 + R.salt);
        auto nx = [&] {
            rng = splitmix(rng);
            return rng;
        };
        if (tid == 0)
            R.set_label("page shuffle: fill");
        for (u64 pg = tid; pg < np && !R.stop; pg += R.layout.T) {
            pp[pg] = pick(nx());
            u8* a = R.layout.region + pg * kPageBytes;
            Gen g = gen(pp[pg]);
            pm_fill<1>(a, g, pp[pg].inv);
            filled[pg] = 1;
            if (((pg / R.layout.T + 1) & 0x3FF) == 0)
                R.tick();
            DDR5_INJECT(pagemix_fill_stop, R, tid, pg);
        }
        _mm_sfence();
        R.sync();
        if (tid == 0)
            R.set_label("page shuffle: copy / invert / refill / check");
        double end = plat::now() + opt.seconds;
        u64 n = 0;
        while (!R.stop) {
            if ((++n & 63) == 0 && plat::now() >= end)
                break;
            u64 r = nx();
            u64 s = (r >> 8) % np, d = nx() % np;
            int op = (int)(r % 10);
            bool fl = (r >> 5) & 1;
            if (op <= 5 && d == s)
                continue;
            if (busy[s].exchange(1, std::memory_order_acquire))
                continue;
            bool two = op <= 5;
            if (two && busy[d].exchange(1, std::memory_order_acquire)) {
                busy[s].store(0, std::memory_order_release);
                continue;
            }
            u8* sa = R.layout.region + s * kPageBytes;
            u8* da = R.layout.region + d * kPageBytes;
            PagePat sq = pp[s]; // sq / sg: the source page's pattern and its generator
            Gen sg = gen(sq);
            if (op <= 5) {
                with_flush(fl, [&](auto FLc) { pm_copy<decltype(FLc)::value>(R, tid, sa, da, sg, sq.inv); });
                pp[d] = sq;
                R.add_done(8192);
            } else if (op == 6) {
                PagePat nq = pick(nx()); // nq / ng: the new pattern for the refill and its generator
                Gen ng = gen(nq);
                with_flush(fl, [&](auto FLc) { pm_fill<decltype(FLc)::value>(sa, ng, nq.inv); });
                pp[s] = nq;
                R.add_done(kPageBytes);
            } else if (op <= 8) {
                with_flush(fl, [&](auto FLc) { pm_invert<decltype(FLc)::value>(sa); });
                R.add_done(kPageBytes * 4);
            } else {
                with_flush(fl,
                           [&](auto FLc) { pm_check<decltype(FLc)::value>(R, tid, sa, sg, sq.inv, "pagemix-check"); });
                R.add_done(kPageBytes);
            }
            if (two)
                busy[d].store(0, std::memory_order_release);
            busy[s].store(0, std::memory_order_release);
        }
        _mm_sfence();
        R.sync();
        DDR5_INJECT(pagemix_flip, R, tid);
        if (tid == 0)
            R.set_label("page shuffle: final verify of all pages");
        // Hard stop: Stop (or the first error in stop-on-error mode) ends this verify at the next page. Pages not yet
        // compared are simply not checked, which can miss a fault but never invents one.
        for (u64 pg = tid; pg < np && !R.stop.load(std::memory_order_relaxed); pg += R.layout.T) {
            if (!filled[pg])
                continue;
            Gen g = gen(pp[pg]);
            pm_check<1>(R, tid, R.layout.region + pg * kPageBytes, g, pp[pg].inv, "pagemix-final");
            if (((pg / R.layout.T + 1) & 0x3FF) == 0)
                R.tick();
            DDR5_INJECT(pagemix_final_stop, R, tid, pg);
        }
        R.sync();
    });
}

} // namespace ddr5
