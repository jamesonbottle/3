#pragma once
// suite.h - run settings and the run_suite() entry point used by both front-ends.
#include <string>
#include <vector>
#include "params.h"
#include "run.h"
#include "seed.h"
#include "types.h"

namespace ddr5 {

// The enumerators are in the order of the CLI --cache / GUI "CPU cache" and "Test set" lists (the front-ends convert
// a list index straight to the enum).
enum class CacheMode : int {
    Default,     // per test
    Bypass,      // clflush every access
    Cached,      // plain cached stores
    NonTemporal, // non-temporal stores
};
enum class SuiteSet : int {
    Full,
    Quick, // no long pauses / hammer
};

struct Settings {
    u64 megabytes = 0;       // 0 = auto (90% of free memory)
    int threads = 0;         // 0 = all logical CPUs
    bool stopOnError = true; // beeping is done by the front-end
    CacheMode cacheMode = CacheMode::Default;
    Rng rngMode = Rng::Default; // hash mixer for 'hash' data; Default = SplitMix
    bool stressFpu = false;     // extra FPU/AVX load threads (more current draw while testing memory)
    bool largePages = true;
    SuiteSet suite = SuiteSet::Full;
    std::string logFile = "ddr5test.log";
    bool logFileSet =
        false; // true = the user named the file (--log): failing to open it is then an error, not a warning
    u64 coverageStop = 0;
    double minutes = 0;
    u64 loops = 0;
    std::string filter;
    u64 testSeconds =
        0; // >0 caps the duration of the timed tests (page shuffle, cache coherency); 0 = as in suite_data.cpp
    bool seedSet = false; // false = pick a fresh random seed for this run; true = use `seed` (reproduce an earlier run)
    u64 seed = 0;
};

std::vector<TestDef> builtin_suite(); // the 28 built-in tests, defined as code in suite_data.cpp
// The tests a run with these settings executes: filters applied, --test-seconds and --cache overrides written into the
// parameters. Exposed for the unit tests; run_suite() is the only production caller.
std::vector<TestDef> select_tests(const Settings& s, std::vector<TestDef> tests);
// Runs the built-in suite until finished or R.stop. Follow it from another thread with R.status() / R.error_count().
int run_suite(const Settings& s, Run& R);

} // namespace ddr5
