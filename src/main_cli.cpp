// ddr5test CLI front-end (console). Same engine and built-in suite as the GUI; used for headless runs and for development on Linux.
#include "engine.h"

static void usage() {
    puts("ddr5test_cli [-mb MB] [-t threads] [-l loops] [-f name-filter] [--quick] [--coverage PCT] [--minutes N]\n"
         "             [--cache default|bypass|cached|nt] [--rng default|splitmix|murmur|xorshift] [--fpu]\n"
         "             [--keep-going] [--no-large-pages] [--log FILE] [--list]");
}

int main(int argc, char** argv) {
    Settings s; s.loops = 1; s.stopOnError = false; bool list = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i]; auto nx = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "-mb") s.megabytes = strtoull(nx().c_str(), nullptr, 10);
        else if (a == "-t") s.threads = atoi(nx().c_str());
        else if (a == "-l") s.loops = strtoull(nx().c_str(), nullptr, 10);
        else if (a == "-f") s.filter = nx();
        else if (a == "--quick") s.suite = 1;
        else if (a == "--coverage") s.coverageStop = strtoull(nx().c_str(), nullptr, 10);
        else if (a == "--minutes") s.minutes = atof(nx().c_str());
        else if (a == "--cache") { std::string v = lower(nx()); s.cacheMode = v == "bypass" ? 1 : v == "cached" ? 2 : v == "nt" ? 3 : 0; }
        else if (a == "--rng") { std::string v = lower(nx()); s.rngMode = v == "splitmix" ? 1 : v == "murmur" ? 2 : v == "xorshift" ? 3 : 0; }
        else if (a == "--fpu") s.stressFpu = true;
        else if (a == "--keep-going") s.stopOnError = false;
        else if (a == "--stop-on-error") s.stopOnError = true;
        else if (a == "--no-large-pages") s.largePages = false;
        else if (a == "--log") s.logFile = nx();
        else if (a == "--list") list = true;
        else { usage(); return a == "-h" || a == "--help" ? 0 : 2; }
    }
    if (list) {
        Cfg cfg; std::vector<TestDef> tests; std::istringstream is(suite_text()); parse_cfg_stream(is, "<built-in>", cfg, tests);
        int n = 0; for (auto& t : tests) printf("%3d %s %-8s %s\n", ++n, t.p.flag("quick", false) ? "Q" : " ", t.p.str("algo", "fill").c_str(), t.name.c_str());
        return 0;
    }
    g_console = true;
    static Run R; plat::install_ctrlc(&R.stop);
    return run_suite(s, R);
}
