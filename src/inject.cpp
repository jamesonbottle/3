// inject.cpp - the planted faults behind the DDR5_INJECT hooks (see inject.h). Compiled only into ddr5engine_inject.
#include "inject.h"

#ifndef DDR5TEST_INJECT
#error "inject.cpp belongs only in the fault-injection engine (ddr5engine_inject, -DDDR5TEST_INJECT)"
#endif

#include "algo.h"
#include "lineio.h"
#include "run.h"
#include <cstdlib>

namespace ddr5::inject {

// self-test only: corrupt memory between write and verify (algo_fill, after the first sync)
void after_fill(Run& R, int tid, size_t ji, const View& w) {
    if (tid == 0 && ji == 0) {
        u8* c = w.base + 640 * 64 + 20;
        c[0] ^= 0x05;
        c[1] ^= 0x10;
        w.base[900 * 64 + 4] ^= 0x80;
        for (int k = 0; k < 40; k++)
            w.base[(4096 + k) * 64 + 8] ^= 1;
        _mm_clflush(c);
        _mm_mfence();
    }
    R.sync();
}

// self-test only, last pass: (1) undo one rewrite = a lost write, (2) flip one bit in a line
// that was not rewritten
void evolve_last_pass(Run& R, int tid, size_t ji, const View& w, u64 p, u64 passes, u64 seed, u64 maxp, StoreFn store) {
    if (p == passes) {
        if (tid == 0 && ji == 0) {
            bool lost = false, flip = false;
            for (u64 i = 0; i < w.n && !(lost && flip); i++) {
                u64 g = w.g + i, per = evo_period(g, seed, maxp);
                u8* a = w.base + i * 64;
                if (!lost && p % per == 0) {
                    Line old;
                    evo_make(g, p / per - 1, seed, R.rng, old);
                    store(a, old);
                    lost = true;
                } else if (!flip && p % per != 0) {
                    a[8] ^= 0x20;
                    _mm_clflush(a);
                    flip = true;
                }
            }
            _mm_sfence();
            _mm_mfence();
        }
        R.sync();
    }
}

// self-test only: with DDR5TEST_STOP_IN_FILL set, stop in the middle of the fill phase (pages
// after this point are never filled)
void pagemix_fill_stop(Run& R, int tid, u64 pg) {
    if (tid == 0 && pg >= 1000 && getenv("DDR5TEST_STOP_IN_FILL"))
        R.stop = true;
}

// self-test only: one planted bit flip before the final verify of all pages
void pagemix_flip(Run& R, int tid) {
    if (tid == 0) {
        R.layout.region[3 * 4096 + 100] ^= 0x04;
        _mm_clflush(R.layout.region + 3 * 4096 + 64);
        _mm_mfence();
    }
    R.sync();
}

// self-test only: one lost/extra increment
void coherency_extra(u8* mem, int tid) {
    if (tid == 0) {
        static bool once = false;
        if (!once) {
            once = true;
            mem[0]++;
        }
    }
}

} // namespace ddr5::inject
