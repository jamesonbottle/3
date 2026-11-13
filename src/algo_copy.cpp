// algo_copy.cpp - bulk copy engines (memcpy, rep movsb, non-temporal copy).
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

// ------------------------------------------------------------------ options (the only place copy reads its keys)
// `window_mb` (Run::window_lines) and the data keys (build_vars) belong to shared helpers.
const std::set<std::string>& CopyOpts::keys() {
    static const std::set<std::string> k = {"block", "method"};
    return k;
}
CopyOpts CopyOpts::from(const Params& P) {
    CopyOpts o;
    o.methods = P.words("method", "memcpy movsb ntcopy");
    o.block = std::max<u64>(kLineBytes, P.num("block", 64 * 1024));
    return o;
}

// ------------------------------------------------------------------ algo: copy (real-world bulk copy engines)
void algo_copy(Run& R, const Params& P) {
    auto vars = build_vars(P, R.salt, R.rng, R.log);
    const CopyOpts opt = CopyOpts::from(P);
    u64 wl = R.window_lines(P);
    struct Job {
        std::string label;
        const Gen* g;
        std::string m;
    };
    std::vector<Job> jobs;
    for (auto& v : vars)
        for (auto& m : opt.methods)
            jobs.push_back({v.label + " via " + m, &v.g, lower(m)});
    u64 base = R.winctr[R.testName];
    R.winctr[R.testName] += jobs.size();
    R.parallel([&](int tid) {
        for (size_t ji = 0; ji < jobs.size(); ji++) {
            if (R.stop)
                break;
            const Job& j = jobs[ji];
            if (tid == 0)
                R.set_label("copy " + j.label);
            View w = R.view(tid, wl, base + ji);
            u64 bl = opt.block / kLineBytes, nA = w.n / 2 / bl * bl;
            using io = IO<32, 0, 1>;
            u8* A = w.base;
            u8* B = w.base + nA * kLineBytes;
            for (u64 i = 0; i < nA; i++) {
                u8* a = A + i * kLineBytes;
                Line l;
                j.g->make(w.g + i, a, false, l);
                io::store(a, l);
                if (((i + 1) & 0xFFFF) == 0)
                    R.tick();
            }
            R.sync();
            for (u64 o = 0; o < nA && !R.stop; o += bl) {
                u8* s = A + o * kLineBytes;
                u8* d = B + o * kLineBytes;
                size_t nb = bl * kLineBytes;
                if (j.m == "memcpy")
                    std::memcpy(d, s, nb);
                else if (j.m == "movsb") {
#if defined(_MSC_VER)
                    __movsb(d, s, nb);
#else
                    asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(nb) : : "memory");
#endif
                } else
                    for (size_t k = 0; k < nb; k += 32)
                        _mm256_stream_si256((__m256i*)(d + k), _mm256_load_si256((const __m256i*)(s + k)));
                R.add_done(bl * kLineBytes * 2);
            }
            _mm_sfence();
            R.sync();
            DDR5_INJECT(copy_after_copy, tid, ji, A, B, nA);
            for (int side = 0; side < 2 && !R.stop; side++)
                for (u64 i = 0; i < nA; i++) {
                    const u8* a = (side ? B : A) + i * kLineBytes;
                    Line e, act;
                    j.g->make(w.g + i, A + i * kLineBytes, false, e);
                    R.vtick();
                    if (!io::verify(a, e, act))
                        R.report(tid, a, e, act, side ? "copy-dst" : "copy-src");
                    if (((i + 1) & 0xFFFF) == 0)
                        R.tick();
                }
            R.sync();
        }
    });
}

} // namespace ddr5
