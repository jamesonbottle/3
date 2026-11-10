#pragma once
// suite.h - run settings and the run_suite() entry point used by both front-ends.
#include <string>
#include "run.h"
#include "types.h"

namespace ddr5 {

struct Settings {
    u64 megabytes = 0;       // 0 = auto (90% of free memory)
    int threads = 0;         // 0 = all logical CPUs
    bool stopOnError = true; // beeping is done by the front-end
    int cacheMode = 0;       // 0 default (per test), 1 bypass (clflush every access), 2 cached, 3 non-temporal stores
    int rngMode = 0;         // 0 default, 1 SplitMix64, 2 Murmur3 fmix64, 3 xorshift64*
    bool stressFpu = false;  // extra FPU/AVX load threads (more current draw while testing memory)
    bool largePages = true;
    int suite = 0; // 0 full, 1 quick (no long pauses / hammer)
    std::string logFile = "ddr5test.log";
    u64 coverageStop = 0;
    double minutes = 0;
    u64 loops = 0;
    std::string filter;
    u64 testSeconds = 0; // >0 caps the duration of the timed tests (page shuffle, cache coherency); 0 = as in suite.cfg
    bool seedSet = false; // false = pick a fresh random seed for this run; true = use `seed` (reproduce an earlier run)
    u64 seed = 0;
};

std::string suite_text(); // the built-in suite.cfg text (compiled in, see suite_data.h)
// Runs the built-in suite until finished or R.stop. Poll R (vtotal, errTotal, label, tStart, ...) from another thread.
int run_suite(const Settings& s, Run& R);

} // namespace ddr5
