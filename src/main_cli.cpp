// ddr5test CLI front-end (console). Same engine and built-in suite as the GUI; used for headless runs and for
// development on Linux.
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>
#include "log.h"
#include "params.h"
#include "platform.h"
#include "run.h"
#include "suite.h"
#include "types.h"
#ifdef _WIN32
#include <io.h>
#define IS_TTY(fd) _isatty(fd)
#else
#define IS_TTY(fd) isatty(fd)
#endif

static void usage() {
    puts("ddr5test_cli [-mb MB] [-t threads] [-l loops] [-f name-filter] [--quick] [--coverage PCT] [--minutes N]\n"
         "             [--test-seconds N] [--cache default|bypass|cached|nt] [--rng default|splitmix|murmur|xorshift] "
         "[--fpu]\n"
         "             [--seed N|0xHEX]   (each run picks a random seed and prints it; pass it back with the same -mb "
         "/ -t to repeat the run)\n"
         "             [--keep-going | --stop-on-error] [--no-large-pages] [--log FILE] [--list]\n"
         "exit code: 0 = no errors, 1 = memory errors found, 2 = could not start (bad option / not enough memory), 3 = "
         "internal error");
}

int main(int argc, char** argv) {
    Settings s;
    s.loops = 1;
    s.stopOnError = false;
    bool list = false;
    for (int i = 1; i < argc; i++) {
        // Option values are checked: a typo such as "-mb abc" must not silently turn into "use 90 % of free RAM" (exit
        // code 2 instead).
        std::string a = argv[i];
        bool bad = false;
        auto nx = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "option %s needs a value\n", a.c_str());
                bad = true;
                return "";
            }
            return argv[++i];
        };
        auto num = [&](u64 maxv) -> u64 { // non-negative whole number <= maxv
            std::string v = nx();
            if (bad)
                return 0;
            char* e = nullptr;
            errno = 0;
            unsigned long long x = v.empty() || v[0] == '-' || v[0] == '+' ? 0 : strtoull(v.c_str(), &e, 10);
            if (v.empty() || v[0] == '-' || v[0] == '+' || !e || *e || errno || x > maxv) {
                fprintf(stderr, "bad value '%s' for %s (expected a whole number from 0 to %llu)\n", v.c_str(),
                        a.c_str(), (unsigned long long)maxv);
                bad = true;
                return 0;
            }
            return x;
        };
        auto choice = [&](std::initializer_list<const char*> names)
            -> int { // index of the (case-insensitive) name, or -1 after reporting an error
            std::string v = lower(nx());
            if (bad)
                return -1;
            int k = 0;
            for (const char* n : names) {
                if (v == n)
                    return k;
                k++;
            }
            std::string all;
            for (const char* n : names)
                all += std::string(all.empty() ? "" : "|") + n;
            fprintf(stderr, "bad value '%s' for %s (expected %s)\n", v.c_str(), a.c_str(), all.c_str());
            bad = true;
            return -1;
        };
        const u64 BIG = ~0ull >> 1;
        if (a == "-mb")
            s.megabytes = num(BIG >> 20);
        else if (a == "-t")
            s.threads = (int)num(4096);
        else if (a == "-l")
            s.loops = num(BIG);
        else if (a == "-f")
            s.filter = nx();
        else if (a == "--quick")
            s.suite = 1;
        else if (a == "--coverage")
            s.coverageStop = num(BIG >> 8);
        else if (a == "--minutes") {
            std::string v = nx();
            char* e = nullptr;
            double m = v.empty() ? -1.0 : strtod(v.c_str(), &e);
            if (!bad && (v.empty() || !e || *e || !(m >= 0.0) || m > 1e7)) {
                fprintf(stderr, "bad value '%s' for --minutes (expected a number of minutes >= 0)\n", v.c_str());
                bad = true;
            }
            s.minutes = m;
        } else if (a == "--test-seconds")
            s.testSeconds = num(BIG);
        else if (a == "--cache") {
            int k = choice({"default", "bypass", "cached", "nt"});
            if (k >= 0)
                s.cacheMode = k;
        } else if (a == "--rng") {
            int k = choice({"default", "splitmix", "murmur", "xorshift"});
            if (k >= 0)
                s.rngMode = k;
        } else if (a ==
                   "--seed") { // any 64-bit value, decimal or 0x hex (the run prints its seed in exactly this form)
            std::string v = nx();
            if (!bad) {
                bool hex = v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X');
                const char* d = v.c_str() + (hex ? 2 : 0);
                bool plain = *d && *d != '-' && *d != '+' &&
                             !isspace((unsigned char)*d); // strtoull would silently accept a sign or leading blanks
                char* e = nullptr;
                errno = 0;
                unsigned long long x = plain ? strtoull(d, &e, hex ? 16 : 10) : 0;
                if (!plain || !e || *e || errno) {
                    fprintf(stderr, "bad value '%s' for --seed (expected a 64-bit number, decimal or 0x hex)\n",
                            v.c_str());
                    bad = true;
                } else {
                    s.seed = x;
                    s.seedSet = true;
                }
            }
        } else if (a == "--fpu")
            s.stressFpu = true;
        else if (a == "--keep-going")
            s.stopOnError = false;
        else if (a == "--stop-on-error")
            s.stopOnError = true;
        else if (a == "--no-large-pages")
            s.largePages = false;
        else if (a == "--log")
            s.logFile = nx();
        else if (a == "--list")
            list = true;
        else {
            usage();
            return a == "-h" || a == "--help" ? 0 : 2;
        }
        if (bad)
            return 2;
    }
    if (list) {
        Cfg cfg;
        std::vector<TestDef> tests;
        std::istringstream is(suite_text());
        parse_cfg_stream(is, "<built-in>", cfg, tests);
        int n = 0;
        for (auto& t : tests)
            printf("%3d %s %-9s %s\n", ++n, t.p.flag("quick", false) ? "Q" : " ", t.p.str("algo", "fill").c_str(),
                   t.name.c_str());
        return 0;
    }
    g_console = true;
    g_tty = IS_TTY(1) != 0;
    static Run R;
    plat::install_ctrlc(&R.stop);
    return run_suite(s, R);
}
