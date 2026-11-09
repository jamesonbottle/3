#pragma once
// engine.h - DDR5-aware memory test engine (header-only; no main). Shared by the GUI (main_gui.cpp) and the console tool (main_cli.cpp).
//
// Needs C++20 and AVX2 (all DDR5 platforms - Alder Lake+/Zen4+ - have AVX2).
//   MSVC:  cl /O2 /std:c++20 /arch:AVX2 /EHsc ...
//   MinGW: g++ -O2 -std=c++20 -mavx2 -pthread ...
//
// The test suite is compiled in: suite.cfg -> tools/gen_suite.py -> suite_data.h. No config file is read at run time.

#if defined(_MSC_VER)
  #pragma comment(lib, "advapi32.lib")
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <immintrin.h>

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <csignal>
  #include <pthread.h>
  #include <sched.h>
  #include <sys/mman.h>
  #include <unistd.h>
#endif

#if defined(_MSC_VER)
  #include <intrin.h>
  #define CBAR() _ReadWriteBarrier()
#else
  #define CBAR() asm volatile("" ::: "memory")
#endif

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;

struct alignas(64) Line { u64 w[8]; };
static_assert(sizeof(Line) == 64, "Line must be one 64B burst (BL16 x 32-bit DDR5 subchannel)");

static inline u64 splitmix(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
static inline int pc32(u32 v) {
#if defined(_MSC_VER)
    return (int)__popcnt(v);
#else
    return __builtin_popcount(v);
#endif
}
static inline int clz64(u64 v) {
#if defined(_MSC_VER)
    unsigned long i; _BitScanReverse64(&i, v); return 63 - (int)i;
#else
    return __builtin_clzll(v);
#endif
}
static inline u64 rev64(u64 x) {
    x = ((x >> 1) & 0x5555555555555555ull) | ((x & 0x5555555555555555ull) << 1);
    x = ((x >> 2) & 0x3333333333333333ull) | ((x & 0x3333333333333333ull) << 2);
    x = ((x >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((x & 0x0F0F0F0F0F0F0F0Full) << 4);
    x = ((x >> 8) & 0x00FF00FF00FF00FFull) | ((x & 0x00FF00FF00FF00FFull) << 8);
    x = ((x >> 16) & 0x0000FFFF0000FFFFull) | ((x & 0x0000FFFF0000FFFFull) << 16);
    return (x >> 32) | (x << 32);
}

// ------------------------------------------------------------------ platform layer
namespace plat {
static std::atomic<bool>* g_stop = nullptr;

#ifdef _WIN32
static BOOL WINAPI ctrl(DWORD) { if (g_stop) g_stop->store(true); return TRUE; }
void install_ctrlc(std::atomic<bool>* s) { g_stop = s; SetConsoleCtrlHandler(ctrl, TRUE); }
int ncpu() { return (int)std::thread::hardware_concurrency(); }
void pin(int cpu) { if (cpu < 64) SetThreadAffinityMask(GetCurrentThread(), 1ull << cpu); }
u64 avail_phys() { MEMORYSTATUSEX m{}; m.dwLength = sizeof(m); GlobalMemoryStatusEx(&m); return m.ullAvailPhys; }
u64 total_phys() { MEMORYSTATUSEX m{}; m.dwLength = sizeof(m); GlobalMemoryStatusEx(&m); return m.ullTotalPhys; }
void lower_priority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
static bool lock_priv() {
    HANDLE t; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return false;
    TOKEN_PRIVILEGES tp{}; tp.PrivilegeCount = 1; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueA(nullptr, "SeLockMemoryPrivilege", &tp.Privileges[0].Luid)) { CloseHandle(t); return false; }
    AdjustTokenPrivileges(t, FALSE, &tp, 0, nullptr, nullptr);
    bool ok = GetLastError() == ERROR_SUCCESS; CloseHandle(t); return ok;
}
void* alloc(u64 bytes, bool large, bool& got) {
    got = false;
    if (large && lock_priv()) {
        SIZE_T lp = GetLargePageMinimum();
        if (lp) {
            u64 b = (bytes + lp - 1) / lp * lp;
            void* p = VirtualAlloc(nullptr, b, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
            if (p) { got = true; return p; }
        }
    }
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), bytes + (256ull << 20), bytes + (512ull << 20),
                               QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE);
    void* p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p) VirtualLock(p, bytes);
    return p;
}
void release(void* p, u64) { VirtualFree(p, 0, MEM_RELEASE); }
#else
static void ctrl(int) { if (g_stop) g_stop->store(true); }
void install_ctrlc(std::atomic<bool>* s) { g_stop = s; std::signal(SIGINT, ctrl); }
int ncpu() { return (int)std::thread::hardware_concurrency(); }
void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu % CPU_SETSIZE, &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
}
u64 avail_phys() { return (u64)sysconf(_SC_AVPHYS_PAGES) * (u64)sysconf(_SC_PAGESIZE); }
u64 total_phys() { return (u64)sysconf(_SC_PHYS_PAGES) * (u64)sysconf(_SC_PAGESIZE); }
void lower_priority() {}
void* alloc(u64 bytes, bool, bool& got) {
    got = false;
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    madvise(p, bytes, MADV_HUGEPAGE);
    return p;
}
void release(void* p, u64 b) { munmap(p, b); }
#endif
static double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace plat

static void emitf(const char* fmt, ...);  // defined below; the parser and generators report through it
// ------------------------------------------------------------------ parameters / config
static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static u64 pnum(std::string s) {
    s = trim(s); if (s.empty()) return 0;
    u64 mul = 1; bool hex = s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    if (!hex) {
        char c = (char)toupper((unsigned char)s.back());
        if (c == 'K') { mul = 1ull << 10; s.pop_back(); }
        else if (c == 'M') { mul = 1ull << 20; s.pop_back(); }
        else if (c == 'G') { mul = 1ull << 30; s.pop_back(); }
    }
    return strtoull(s.c_str(), nullptr, hex ? 16 : 10) * mul;
}
static std::vector<std::string> split_ws(const std::string& s, bool commas = false) {
    std::vector<std::string> o; std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || (commas && c == ',')) { if (!cur.empty()) o.push_back(cur), cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) o.push_back(cur);
    return o;
}
struct Params {
    std::map<std::string, std::string> m;
    bool has(const std::string& k) const { return m.count(k) > 0; }
    std::string str(const std::string& k, const std::string& d = "") const { auto i = m.find(k); return i == m.end() ? d : i->second; }
    u64 num(const std::string& k, u64 d) const { auto i = m.find(k); return i == m.end() ? d : pnum(i->second); }
    bool flag(const std::string& k, bool d) const { auto i = m.find(k); if (i == m.end()) return d; std::string v = lower(i->second); return v == "1" || v == "true" || v == "yes" || v == "on"; }
    std::vector<std::string> words(const std::string& k, const std::string& d = "") const { return split_ws(str(k, d)); }
    std::vector<u64> nums(const std::string& k, const std::string& d = "") const {
        std::vector<u64> o; for (auto& w : split_ws(str(k, d), true)) o.push_back(pnum(w)); return o;
    }
};
struct TestDef { std::string name; Params p; };
struct Cfg {
    u64 memPercent = 75, memMB = 0, marginMB = 768, loops = 1, maxLog = 200, cacheMB = 192;
    int threads = 0; bool large = true, stopOnErr = false; std::string logFile = "ddr5test.log";
};

static bool parse_cfg_stream(std::istream& f, const std::string& path, Cfg& cfg, std::vector<TestDef>& tests) {
    std::string line, sect; Params defaults; TestDef* cur = nullptr; int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        size_t h = line.find('#'); if (h != std::string::npos) line = line.substr(0, h);
        line = trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            sect = lower(trim(line.substr(1, line.find(']') - 1)));
            if (sect == "test") { tests.push_back({"test" + std::to_string(tests.size() + 1), defaults}); cur = &tests.back(); }
            else if (sect == "default") { defaults = Params(); cur = nullptr; }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) { emitf("%s:%d: expected key = value\n", path.c_str(), lineno); continue; }
        std::string k = lower(trim(line.substr(0, eq))), v = trim(line.substr(eq + 1));
        if (sect == "global") {
            if (k == "memory_percent") cfg.memPercent = pnum(v);
            else if (k == "memory_mb") cfg.memMB = pnum(v);
            else if (k == "margin_mb") cfg.marginMB = pnum(v);
            else if (k == "threads") cfg.threads = (int)pnum(v);
            else if (k == "loops") cfg.loops = pnum(v);
            else if (k == "large_pages") cfg.large = pnum(v) != 0;
            else if (k == "stop_on_error") cfg.stopOnErr = pnum(v) != 0;
            else if (k == "log") cfg.logFile = v;
            else if (k == "max_errors_logged") cfg.maxLog = pnum(v);
            else if (k == "cache_mb") cfg.cacheMB = pnum(v);
            else emitf("%s:%d: unknown global key '%s'\n", path.c_str(), lineno, k.c_str());
        } else if (sect == "default") defaults.m[k] = v;
        else if (sect == "test" && cur) { if (k == "name") cur->name = v; else cur->p.m[k] = v; }
    }
    return true;
}

// ------------------------------------------------------------------ address orders
// An order is a bijection q -> line index over the part it covers; leftover tail lines are visited sequentially.
static u64 g_salt = 0;  // changes every loop so a long soak never repeats the same seeds/permutations (loop 1 = 0 = reproducible)
struct Order {
    enum K { SEQ, STRIDE, RANDOM, BITREV, GRAY, BUTTERFLY } k = SEQ;
    u64 n = 0, m = 0, S = 1, cnt = 1; int bits = 0, h = 0; u64 fk[4] = {};
    static bool parse(const std::string& s, K& k, bool& rev) {
        std::string v = lower(s); rev = false;
        if (v == "seq" || v == "up") k = SEQ;
        else if (v == "rev" || v == "down") { k = SEQ; rev = true; }
        else if (v == "stride") k = STRIDE; else if (v == "random") k = RANDOM;
        else if (v == "bitrev") k = BITREV; else if (v == "gray") k = GRAY;
        else if (v == "butterfly") k = BUTTERFLY; else return false;
        return true;
    }
    void init(K kk, u64 nn, u64 stride, u64 seed) {
        k = kk; n = nn; m = nn; S = std::max<u64>(1, stride);
        switch (k) {
        case STRIDE: S = std::min(S, n); cnt = n / S; m = cnt * S; break;
        case RANDOM:
            bits = n > 1 ? 64 - clz64(n - 1) : 0; if (bits & 1) bits++; h = bits / 2;
            for (int i = 0; i < 4; i++) fk[i] = splitmix(seed + g_salt * 7919 + 0x1234 * (i + 1));
            break;
        case BITREV: case GRAY: bits = n > 1 ? 63 - clz64(n) : 0; m = 1ull << bits; break;
        default: break;
        }
    }
    inline u64 fe(u64 x) const {
        u64 mask = (1ull << h) - 1, L = x >> h, R = x & mask;
        for (int r = 0; r < 4; r++) { u64 f = splitmix(R ^ fk[r]) & mask; u64 t = L ^ f; L = R; R = t; }
        return (L << h) | R;
    }
    inline u64 at(u64 i) const {
        switch (k) {
        case SEQ: return i;
        case STRIDE: return (i % cnt) * S + i / cnt;
        case RANDOM: { if (!bits) return i; u64 x = i; do x = fe(x); while (x >= n); return x; }
        case BITREV: return bits ? rev64(i) >> (64 - bits) : 0;
        case GRAY: return i ^ (i >> 1);
        case BUTTERFLY: return (i & 1) ? n - 1 - (i >> 1) : (i >> 1);
        }
        return i;
    }
    inline u64 pos(u64 q, bool down) const { if (down) q = n - 1 - q; return q < m ? at(q) : q; }
};

// ------------------------------------------------------------------ data generators (pure functions of line index)
using Beats = std::array<u32, 16>;  // 16 beats x 32 DQ = one BL16 burst on one DDR5 subchannel
// Selectable mixer for 'hash' data (GUI: Advanced > RNG). Orders keep splitmix so permutations stay stable.
static int g_rng = 0;  // 0 splitmix64, 1 murmur3 fmix64, 2 xorshift64*
static inline u64 mix64(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    switch (g_rng) {
    case 1: x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33; return x;
    case 2: x ^= x >> 12; x ^= x << 25; x ^= x >> 27; return x * 0x2545F4914F6CDD1Dull;
    default: x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull; x = (x ^ (x >> 27)) * 0x94D049BB133111EBull; return x ^ (x >> 31);
    }
}
// Data patterns in the style of Google's stressapptest table (the exact table is reimplemented from its public description,
// not copied): walking ones / inverse-alternating ones / zeros, 0/1, 5/A, 8b10b-style, and a few "busy" patterns that toggle
// many lanes per beat. Each 32-bit pattern word is held for 1/2/4/8 consecutive words ("bus width" 32/64/128/256 bit repeat).
// weight[w] = how often stressapptest-style random page filling picks this pattern at width w (0 = not used at that width).
struct SatPat { const char* name; std::array<u32, 64> w; int len; int weight[4]; };
static const std::vector<SatPat>& sat_table() {
    static const std::vector<SatPat> t = [] {
        std::vector<SatPat> v; auto add = [&](const char* n, std::initializer_list<u32> l, int a, int b, int c, int d) {
            SatPat p{}; p.name = n; int i = 0; for (u32 x : l) p.w[i++] = x; p.len = i; p.weight[0] = a; p.weight[1] = b; p.weight[2] = c; p.weight[3] = d; v.push_back(p);
        };
        SatPat w1{}, wi{}, w0{}; w1.name = "walking-ones"; wi.name = "walking-inv-ones"; w0.name = "walking-zeros"; w1.len = wi.len = w0.len = 64;
        for (int i = 0; i < 64; i++) {
            u32 x = i < 32 ? 1u << i : (i < 63 ? 1u << (62 - i) : 0u);    // up 1..0x80000000, then back down to 0
            w1.w[i] = x; w0.w[i] = ~x; u32 u = 1u << (i / 2); wi.w[i] = (i & 1) ? ~u : u;
        }
        int a[4] = {1, 1, 2, 1}, b[4] = {2, 2, 5, 5};
        memcpy(w1.weight, a, sizeof a); memcpy(w0.weight, a, sizeof a); memcpy(wi.weight, b, sizeof b);
        v.push_back(w1); v.push_back(wi); v.push_back(w0);
        add("zero/one", {0x00000000u, 0xFFFFFFFFu}, 5, 5, 15, 5);
        add("zeros", {0x00000000u}, 2, 0, 0, 0);        add("ones", {0xFFFFFFFFu}, 2, 0, 0, 0);
        add("5555", {0x55555555u}, 2, 0, 0, 0);          add("AAAA", {0xAAAAAAAAu}, 2, 0, 0, 0);
        add("5/A", {0x55555555u, 0xAAAAAAAAu}, 1, 1, 1, 1);
        add("5AA5", {0x5AA5A55Au, 0xA55A5AA5u, 0xA55A5AA5u, 0x5AA5A55Au}, 1, 1, 1, 1);
        add("8b10b-long", {0x16161616u}, 2, 0, 0, 0);    add("8b10b-short", {0xB5B5B5B5u}, 2, 0, 0, 0);
        add("8b10b-checker", {0xB5B5B5B5u, 0x4A4A4A4Au}, 1, 0, 0, 1);
        add("5557", {0x55555557u, 0x55575555u}, 0, 2, 0, 0);
        add("0002/FFFD", {0x00020002u, 0xFFFDFFFDu}, 0, 2, 0, 0);
        return v;
    }();
    return t;
}
static inline u32 sat_word(const SatPat& p, u64 idx, int rep) { return p.w[(idx >> rep) % (u64)p.len]; }

struct Gen {
    int kind = 0;  // 0 pattern, 1 hash, 2 address-in-address, 3 stressapptest-style pattern (sp, rep)
    const SatPat* sp = nullptr; int rep = 0;
    Line pat{}, inv{}; u64 seed = 0; int altshift = -1; u64 modn = 0, modoff = 0;  // modulo-N: every Nth line (offset) is inverted
    void set_beats(const Beats& b) {
        for (int j = 0; j < 8; j++) { pat.w[j] = (u64)b[2 * j] | ((u64)b[2 * j + 1] << 32); inv.w[j] = ~pat.w[j]; }
    }
    inline void make(u64 g, const void* addr, bool invert, Line& o) const {
        if (kind == 0) {
            bool s = invert; if (altshift >= 0) s ^= (bool)((g >> altshift) & 1);
            if (modn) s ^= (g % modn) == modoff;
            o = s ? inv : pat;
        }
        else if (kind == 3) { for (int j = 0; j < 8; j++) { u64 v = (u64)sat_word(*sp, g * 16 + 2 * j, rep) | ((u64)sat_word(*sp, g * 16 + 2 * j + 1, rep) << 32); o.w[j] = invert ? ~v : v; } }
        else if (kind == 1) { for (int j = 0; j < 8; j++) { u64 v = mix64(((g << 3) + j) ^ seed); o.w[j] = invert ? ~v : v; } }
        else { for (int j = 0; j < 8; j++) { u64 v = ((u64)(uintptr_t)addr + 8 * j) ^ seed; o.w[j] = invert ? ~v : v; } }
    }
};
struct Var { std::string label; Gen g; };

static Beats beats_from_lanes(const u16* s) {
    Beats b{}; for (int L = 0; L < 32; L++) for (int t = 0; t < 16; t++) if ((s[L] >> t) & 1) b[t] |= 1u << L; return b;
}
static u16 rotl16(u16 v, int r) { r &= 15; return (u16)((v << r) | (v >> ((16 - r) & 15))); }

// DQ-lane stress styles. Lane = bit position inside the 32-bit beat; beat = position in the BL16 burst.
static void expand_style(const std::string& st, const Params& P, std::vector<std::pair<std::string, Beats>>& out) {
    u16 s[32];
    auto uni = [&](const std::string& n, u16 e, u16 o) { for (int L = 0; L < 32; L++) s[L] = (L & 1) ? o : e; out.push_back({n, beats_from_lanes(s)}); };
    if (st == "sso") uni("sso(all lanes toggle in phase)", 0x5555, 0x5555);
    else if (st == "xtalk") uni("xtalk(neighbour lanes opposite phase)", 0x5555, 0xAAAA);
    else if (st == "half") uni("half-nyquist(0011)", 0x3333, 0xCCCC);
    else if (st == "quarter") uni("quarter(00001111)", 0x0F0F, 0xF0F0);
    else if (st == "low") uni("lowfreq(8 ones/8 zeros)", 0x00FF, 0xFF00);
    else if (st == "pulse") {
        for (int L = 0; L < 32; L++) s[L] = (u16)(1u << (L % 16)); out.push_back({"pulse(single 1 in zeros)", beats_from_lanes(s)});
        for (int L = 0; L < 32; L++) s[L] = (u16)~(1u << (L % 16)); out.push_back({"pulse_inv(single 0 in ones)", beats_from_lanes(s)});
    } else if (st == "skew") {
        for (int L = 0; L < 32; L++) s[L] = rotl16(0x0F0F, L); out.push_back({"skew(quarter, phase=lane)", beats_from_lanes(s)});
        for (int L = 0; L < 32; L++) s[L] = rotl16(0x00FF, L); out.push_back({"skew(low, phase=lane)", beats_from_lanes(s)});
    } else if (st == "victim") {
        auto vl = P.nums("victims");
        std::vector<u64> victims = vl; if (victims.empty()) for (u64 v = 0; v < 32; v++) victims.push_back(v);
        struct M { const char* n; u16 v, a; } modes[] = {
            {"toggle/aggr=0", 0x5555, 0x0000}, {"toggle/aggr=1", 0x5555, 0xFFFF}, {"toggle/aggr=opposite", 0x5555, 0xAAAA},
            {"toggle/aggr=inphase", 0x5555, 0x5555}, {"static0/aggr=toggle", 0x0000, 0x5555}, {"static1/aggr=toggle", 0xFFFF, 0x5555}};
        for (u64 v : victims) for (auto& md : modes) {
            for (int L = 0; L < 32; L++) s[L] = (L == (int)v) ? md.v : md.a;
            out.push_back({"victim lane " + std::to_string(v) + " " + md.n, beats_from_lanes(s)});
        }
    } else if (st == "walk1" || st == "walk0") {
        for (int k = 0; k < 32; k++) {
            Beats b; for (int t = 0; t < 16; t++) b[t] = 1u << ((t + k) % 32);
            if (st == "walk0") for (auto& x : b) x = ~x;
            out.push_back({st + " k=" + std::to_string(k), b});
        }
    } else if (st == "prbs") {  // per-lane PRBS7 windows, different phase per lane => pseudo-random ISI/crosstalk
        u8 seq[127]; u32 r = 0x7F;
        for (int i = 0; i < 127; i++) { u32 bit = ((r >> 6) ^ (r >> 5)) & 1; seq[i] = (u8)(r & 1); r = ((r << 1) | bit) & 0x7F; }
        for (int k = 0; k < 8; k++) {
            for (int L = 0; L < 32; L++) { u16 v = 0; for (int t = 0; t < 16; t++) v |= (u16)(seq[(k * 13 + L * 5 + t) % 127] << t); s[L] = v; }
            out.push_back({"prbs7 variant " + std::to_string(k), beats_from_lanes(s)});
        }
    } else emitf("unknown style '%s'\n", st.c_str());
}

static std::vector<Var> build_vars(const Params& P) {
    std::vector<Var> out; std::string data = lower(P.str("data", "pattern"));
    u64 seed = P.num("seed", 0);
    if (data == "sat") {   // every stressapptest-style pattern at each requested bus-width repeat (combinations it never uses are skipped)
        for (auto& sp : sat_table()) for (u64 wd : P.nums("sat_width", "32 64 128 256")) {
            int rep = wd >= 256 ? 3 : wd >= 128 ? 2 : wd >= 64 ? 1 : 0; if (!sp.weight[rep]) continue;
            Var v; v.g.kind = 3; v.g.sp = &sp; v.g.rep = rep; v.label = std::string("sat ") + sp.name + " x" + std::to_string(32u << rep); out.push_back(v);
        }
        return out;
    }
    if (data == "hash" || data == "addr") {
        u64 ns = P.num("seeds", 1);
        for (u64 i = 0; i < ns; i++) {
            Var v; v.g.kind = data == "hash" ? 1 : 2; v.g.seed = (i || g_salt) ? splitmix(seed + i + g_salt) : seed;
            v.label = data + " seed=" + std::to_string(i); out.push_back(v);
        }
        return out;
    }
    if (P.has("style")) {
        std::vector<std::pair<std::string, Beats>> sv;
        for (auto& st : P.words("style")) expand_style(lower(st), P, sv);
        for (auto& [n, b] : sv) { Var v; v.g.set_beats(b); v.label = n; out.push_back(v); }
    } else {
        for (auto& tok : P.words("pattern", "0x00000000")) {
            Beats b{}; std::vector<u32> vals; std::string c;
            for (size_t i = 0; i <= tok.size(); i++) {
                if (i == tok.size() || tok[i] == ':') { if (!c.empty()) vals.push_back((u32)pnum(c)); c.clear(); } else c += tok[i];
            }
            if (vals.empty()) vals.push_back(0);
            for (int t = 0; t < 16; t++) b[t] = vals[t % vals.size()];
            Var v; v.g.set_beats(b); v.label = "pattern " + tok; out.push_back(v);
        }
    }
    if (P.has("altshift")) {  // invert data when bit N of the line index is set; a list sweeps several block sizes
        auto al = P.nums("altshift"); std::vector<Var> o2;
        for (auto& v : out) for (u64 a : al) { Var m = v; m.g.altshift = (int)a; if (al.size() > 1) m.label += " altshift " + std::to_string(a); o2.push_back(m); }
        if (!o2.empty()) out = o2;
    }
    if (P.has("modulo")) {  // memtest-style modulo-N: for each N and each offset, every Nth line gets the inverse
        std::vector<Var> o2;
        for (auto& v : out) for (u64 n : P.nums("modulo")) for (u64 off = 0; n > 1 && off < n; off++) {
            Var m = v; m.g.modn = n; m.g.modoff = off; m.label += " modulo " + std::to_string(n) + " off " + std::to_string(off); o2.push_back(m);
        }
        if (!o2.empty()) out = o2;
    }
    return out;
}

// ------------------------------------------------------------------ line I/O at selectable width / cache behaviour
// W = access width in bytes (8 scalar, 16 SSE2, 32 AVX2); NT = non-temporal stores; FL = clflush after each access
// so verification really comes from DRAM instead of the cache hierarchy.
template <int W, int NT, int FL> struct IO {
    static inline void store(void* d, const Line& s) {
        CBAR();
        if constexpr (W == 32) {
            __m256i a = _mm256_load_si256((const __m256i*)&s), b = _mm256_load_si256((const __m256i*)&s.w[4]);
            if constexpr (NT) { _mm256_stream_si256((__m256i*)d, a); _mm256_stream_si256((__m256i*)d + 1, b); }
            else { _mm256_store_si256((__m256i*)d, a); _mm256_store_si256((__m256i*)d + 1, b); }
        } else if constexpr (W == 16) {
            for (int i = 0; i < 4; i++) {
                __m128i a = _mm_load_si128((const __m128i*)&s + i);
                if constexpr (NT) _mm_stream_si128((__m128i*)d + i, a); else _mm_store_si128((__m128i*)d + i, a);
            }
        } else {
            for (int i = 0; i < 8; i++) {
                if constexpr (NT) _mm_stream_si64((long long*)d + i, (long long)s.w[i]); else ((volatile u64*)d)[i] = s.w[i];
            }
        }
        if constexpr (FL && !NT) _mm_clflush(d);
        CBAR();
    }
    static inline void load(const void* a, Line& t) {
        if constexpr (W == 32) {
            _mm256_store_si256((__m256i*)&t, _mm256_load_si256((const __m256i*)a));
            _mm256_store_si256((__m256i*)&t.w[4], _mm256_load_si256((const __m256i*)a + 1));
        } else if constexpr (W == 16) {
            for (int i = 0; i < 4; i++) _mm_store_si128((__m128i*)&t + i, _mm_load_si128((const __m128i*)a + i));
        } else { for (int i = 0; i < 8; i++) t.w[i] = ((const volatile u64*)a)[i]; }
    }
    static inline bool verify(const void* a, const Line& e, Line& act) {
        CBAR();
        Line t; load(a, t);
        u64 acc = 0; for (int i = 0; i < 8; i++) acc |= t.w[i] ^ e.w[i];
        if (acc) act = t;
        if constexpr (FL) _mm_clflush(a);
        CBAR();
        return acc == 0;
    }
};
template <int V> using IC = std::integral_constant<int, V>;
template <class F> static void dispatch(int width, bool nt, bool fl, F&& f) {
    auto d3 = [&](auto w) {
        if (nt) { if (fl) f(w, IC<1>{}, IC<1>{}); else f(w, IC<1>{}, IC<0>{}); }
        else { if (fl) f(w, IC<0>{}, IC<1>{}); else f(w, IC<0>{}, IC<0>{}); }
    };
    if (width == 8) d3(IC<8>{}); else if (width == 16) d3(IC<16>{}); else d3(IC<32>{});
}

// ------------------------------------------------------------------ run context
struct Chunk { u8* base; u64 lines; u64 g; };
struct View { u8* base; u64 g; u64 n; };
struct Stats {
    std::atomic<u64> lines{0}, bits{0}, lane[32], beat[16], blk[4], abit[48], trans{0}, stable{0}, unstable{0};
    Stats() { clear(); }
    void clear() {
        lines = 0; bits = 0; trans = 0; stable = 0; unstable = 0;
        for (auto& a : lane) a = 0;
        for (auto& a : beat) a = 0;
        for (auto& a : blk) a = 0;
        for (auto& a : abit) a = 0;
    }
};
struct Run {
    Cfg cfg; int T = 1; std::vector<Chunk> chunks; std::atomic<bool> stop{false};
    std::atomic<u64> done{0}; Stats st, total; std::barrier<>* bar = nullptr;
    std::mutex mu; std::string label, testName; FILE* logf = nullptr; u64 logged = 0;
    std::map<std::string, u64> winctr; u8* region = nullptr; u64 regionBytes = 0;
    // coverage (HCI/Karhu-style: how many times the tested region has been verified), timing, stop criteria
    std::atomic<u64> vbytes{0}, vtotal{0}; double tStart = 0, tTestStart = 0, firstErrRun = -1, firstErrTest = -1;
    u64 stopCoverage = 0; double stopSeconds = 0; std::string stopReason;
    std::atomic<u64> errTotal{0}; std::atomic<bool> running{false};

    // call once per verified 64B line; flushes to the shared counters every 64K lines per thread
    void vtick() {
        thread_local u64 c = 0;
        if (++c >= 0x10000) { vbytes.fetch_add(c * 64, std::memory_order_relaxed); vtotal.fetch_add(c * 64, std::memory_order_relaxed); c = 0; }
    }
    void sync() { bar->arrive_and_wait(); }
    void set_label(const std::string& s) { std::lock_guard<std::mutex> l(mu); label = s; }
    void tick() { done.fetch_add(0x10000ull * 64, std::memory_order_relaxed); }
    View view(int tid, u64 wl, u64 ctr) const {
        const Chunk& c = chunks[tid];
        if (!wl || wl >= c.lines) return {c.base, c.g, c.lines};
        u64 nw = c.lines / wl, s = (ctr % nw) * wl; return {c.base + s * 64, c.g + s, wl};
    }
    u64 window_lines(const Params& P) const {
        u64 mb = P.num("window_mb", 0); if (!mb) return 0;
        u64 wl = (mb << 20) / 64 / T; wl = wl / 4096 * 4096; return std::max<u64>(wl, 4096);
    }
    void sleep_ms(u64 ms) {
        double end = plat::now() + ms / 1000.0;
        while (plat::now() < end && !stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    template <class F> void parallel(F fn);
    void report(int tid, const u8* addr, const Line& e, const Line& a, const char* phase, bool reread = true);
};

// output sink: stdout in CLI mode, log pane in the GUI
static std::function<void(const std::string&)> g_emit;
static bool g_console = false;
static void emit(const std::string& t) { if (g_emit) g_emit(t); else { fputs(t.c_str(), stdout); fflush(stdout); } }
static void emitf(const char* fmt, ...) { char b[2048]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); emit(b); }
static void print_clear() { if (g_console) printf("\r%*s\r", 118, ""); }

void Run::report(int tid, const u8* addr, const Line& e, const Line& a, const char* phase, bool reread) {
    const u32* eb = (const u32*)&e; const u32* ab = (const u32*)&a;
    int perblk[4] = {0, 0, 0, 0}; u64 bits = 0; u32 lanemask = 0, beatmask = 0;
    for (int b = 0; b < 16; b++) {
        u32 d = eb[b] ^ ab[b]; if (!d) continue;
        st.beat[b]++; beatmask |= 1u << b; lanemask |= d;
        for (int l = 0; l < 32; l++) if ((d >> l) & 1) st.lane[l]++;
        int pc = pc32(d); bits += pc; perblk[b / 4] += pc;
    }
    // On-die ECC (128 data bits + 8 check bits per codeword) corrects 1 bit invisibly, so what reaches us is
    // mostly >=2 bits inside one 128-bit codeword (or a miscorrection). Bucket per 16-byte block.
    for (int k = 0; k < 4; k++) if (perblk[k]) st.blk[perblk[k] == 1 ? 0 : perblk[k] == 2 ? 1 : perblk[k] <= 4 ? 2 : 3]++;
    u64 nl = st.lines.fetch_add(1) + 1; st.bits += bits; errTotal++;
    double tnow = plat::now();
    { std::lock_guard<std::mutex> l2(mu); if (firstErrTest < 0) firstErrTest = tnow - tTestStart; if (firstErrRun < 0) firstErrRun = tnow - tStart; }
    { u64 off = (u64)(addr - region); for (int b = 6; b < 48; b++) if ((off >> b) & 1) st.abit[b]++; }
    const char* cls = reread ? "n/a" : "(logical check, not a memory line)";
    if (reread && nl < 20000) {  // re-read from DRAM: separates read-path/signalling errors from stored errors
        _mm_clflush(addr); _mm_mfence();
        Line r; IO<32, 0, 0>::load(addr, r);
        bool eq_e = !memcmp(&r, &e, 64), eq_a = !memcmp(&r, &a, 64);
        if (eq_e) { st.trans++; cls = "TRANSIENT(re-read ok: read path/signalling)"; }
        else if (eq_a) { st.stable++; cls = "STABLE(cell/write path/addressing)"; }
        else { st.unstable++; cls = "UNSTABLE(changes every read)"; }
    }
    if (cfg.stopOnErr) stop = true;
    std::lock_guard<std::mutex> l(mu);
    if (logged >= cfg.maxLog) return;
    logged++;
    char buf[1400]; int o = 0;
    o += snprintf(buf + o, sizeof buf - o, "ERROR t=%.1fs test=\"%s\" %s thr=%d addr=%p beats=0x%04X lanes=0x%08X bits=%llu 128b-blocks[1b/2b/3-4b/5+b]=%d/%d/%d/%d %s\n",
                  tnow - tStart, label.c_str(), phase, tid, (const void*)addr, beatmask, lanemask, (unsigned long long)bits,
                  (int)(perblk[0] == 1) + (perblk[1] == 1) + (perblk[2] == 1) + (perblk[3] == 1),
                  (int)(perblk[0] == 2) + (perblk[1] == 2) + (perblk[2] == 2) + (perblk[3] == 2),
                  (int)(perblk[0] >= 3 && perblk[0] <= 4) + (perblk[1] >= 3 && perblk[1] <= 4) + (perblk[2] >= 3 && perblk[2] <= 4) + (perblk[3] >= 3 && perblk[3] <= 4),
                  (int)(perblk[0] >= 5) + (perblk[1] >= 5) + (perblk[2] >= 5) + (perblk[3] >= 5), cls);
    for (int j = 0; j < 8; j++) if (e.w[j] != a.w[j])
        o += snprintf(buf + o, sizeof buf - o, "      qword %d: exp=%016llX act=%016llX xor=%016llX\n", j,
                      (unsigned long long)e.w[j], (unsigned long long)a.w[j], (unsigned long long)(e.w[j] ^ a.w[j]));
    print_clear(); emit(buf);
    if (logf) { fputs(buf, logf); fflush(logf); }
}

template <class F> void Run::parallel(F fn) {
    std::barrier<> b(T); bar = &b; std::atomic<int> running{T}; std::vector<std::thread> th;
    for (int t = 0; t < T; t++) th.emplace_back([&, t] { plat::pin(t); fn(t); b.arrive_and_drop(); running--; });
    double t0 = plat::now(), tl = t0; u64 dl = done.load();
    while (running.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        double t = plat::now();
        if (!stop.load() && regionBytes) {  // user-set stop criteria: coverage target or time limit
            if (stopCoverage && vtotal.load() * 100 >= stopCoverage * regionBytes) { stopReason = "coverage target reached"; stop = true; }
            else if (stopSeconds > 0 && tStart > 0 && t - tStart >= stopSeconds) { stopReason = "time limit reached"; stop = true; }
        }
        if (t - tl >= 0.5) {
            u64 d = done.load(); double rate = (d - dl) / (t - tl) / 1e9; tl = t; dl = d;
            std::string lab; { std::lock_guard<std::mutex> l(mu); lab = label; }
            char buf[220]; snprintf(buf, sizeof buf, "  [%5.0fs] %-50.50s %5.1f GB/s  cov %4.0f%%  errors: %llu", t - t0, lab.c_str(), rate,
                                    regionBytes ? 100.0 * vtotal.load() / regionBytes : 0.0, (unsigned long long)st.lines.load());
            if (g_console) { printf("\r%-118.118s", buf); fflush(stdout); }
        }
    }
    for (auto& x : th) x.join();
    bar = nullptr; print_clear();
}

// walk q = 0..n-1 through an order; f(lineIndex)
template <class F> static inline void walk(Run& R, const Order& o, bool down, F&& f) {
    for (u64 q = 0; q < o.n; q++) {
        f(o.pos(q, down));
        if (((q + 1) & 0xFFFF) == 0) { R.tick(); if (R.stop.load(std::memory_order_relaxed)) return; }
    }
}

struct OrderSpec { Order::K k = Order::SEQ; bool rev = false; };
static OrderSpec ospec(const Params& P, const char* key, const char* def) {
    OrderSpec s; if (!Order::parse(P.str(key, def), s.k, s.rev)) { emitf("bad %s\n", key); } return s;
}
static std::vector<u64> stride_list(const Params& P, bool needed) {
    std::vector<u64> o; if (!needed) { o.push_back(1); return o; }
    for (u64 b : P.nums("stride", "4K")) o.push_back(std::max<u64>(1, b / 64)); return o;
}

// ------------------------------------------------------------------ disturb traffic (manufacturer-style "disturb between write and read")
// Heavy random traffic on a DIFFERENT window of the same thread's chunk while the victim window sits written. Row activations,
// refresh collisions and bus noise happen around data that was just stored; the victim is verified afterwards.
// mode 0 = reads, 1 = writes (scribble), 2 = read+write. Needs window_mb with at least two windows per chunk.
static volatile u64 g_sink = 0;
static void disturb_traffic(Run& R, const View& d, int mode, u64 passes, u64 seed) {
    Order o; o.init(Order::RANDOM, d.n, 1, seed);
    for (u64 p = 0; p < passes && !R.stop; p++)
        walk(R, o, false, [&](u64 i) {
            u8* a = d.base + i * 64;
            if (mode != 1) { Line t; IO<32, 0, 0>::load(a, t); _mm_clflush(a); g_sink = g_sink ^ t.w[0]; }
            if (mode >= 1) { Line l; for (int k = 0; k < 8; k++) l.w[k] = splitmix(seed + i * 8 + k + p); IO<32, 0, 1>::store(a, l); }
        });
}
static int parse_dmode(const std::string& s) { return s == "r" || s == "read" ? 0 : s == "w" || s == "write" ? 1 : s == "rw" ? 2 : -1; }
static bool disturb_possible(Run& R, u64 wl, const char* what) {
    if (wl && R.chunks[0].lines / wl >= 2) return true;
    emitf("%s: disturb needs window_mb small enough for >=2 windows per thread chunk; disturb disabled\n", what);
    return false;
}

// ------------------------------------------------------------------ algo: fill (write -> [disturb] -> [dwell] -> verify xN)
static void algo_fill(Run& R, const Params& P) {
    auto vars = build_vars(P); OrderSpec wo = ospec(P, "order", "seq"), vo = ospec(P, "verify_order", P.str("order", "seq").c_str());
    bool strided = wo.k == Order::STRIDE || vo.k == Order::STRIDE;
    auto strides = stride_list(P, strided);
    bool both = P.flag("both", false), cross = P.flag("cross", false);
    int width = (int)P.num("width", 32); bool nt = P.flag("nt", false), fl = P.flag("flush", true);
    u64 reads = std::max<u64>(1, P.num("reads", 1)), wl = R.window_lines(P);
    auto dwells = P.nums("dwell_ms", "0"); if (dwells.empty()) dwells.push_back(0);
    std::vector<int> dmodes; for (auto& w : P.words("disturb")) { int m = parse_dmode(lower(w)); if (m >= 0) dmodes.push_back(m); }
    if (dmodes.empty()) dmodes.push_back(-1);
    u64 dpasses = std::max<u64>(1, P.num("disturb_passes", 2));
    if (dmodes[0] >= 0 && !disturb_possible(R, wl, R.testName.c_str())) dmodes.assign(1, -1);
    struct Job { std::string label; const Gen* g; bool inv; u64 stride; u64 dwell; int dmode; };
    std::vector<Job> jobs; const char* dn[] = {"r", "w", "rw"};
    for (auto& v : vars) for (u64 s : strides) for (int inv = 0; inv <= (both ? 1 : 0); inv++) for (u64 dw : dwells) for (int dm : dmodes)
        jobs.push_back({v.label + (inv ? " [inverted]" : "") + (strided ? " stride=" + std::to_string(s * 64) + "B" : "") +
                        (dwells.size() > 1 ? " dwell=" + std::to_string(dw) + "ms" : "") + (dmodes.size() > 1 ? std::string(" disturb=") + dn[dm] : ""), &v.g, (bool)inv, s, dw, dm});
    u64 base = R.winctr[R.testName]; R.winctr[R.testName] += jobs.size();
    R.parallel([&](int tid) {
        for (size_t ji = 0; ji < jobs.size(); ji++) {
            if (R.stop) break;
            const Job& j = jobs[ji]; if (tid == 0) R.set_label(j.label);
            View w = R.view(tid, wl, base + ji), vw = R.view(cross ? (tid + 1) % R.T : tid, wl, base + ji);
            Order ow, ov; ow.init(wo.k, w.n, j.stride, 1 + ji); ov.init(vo.k, vw.n, j.stride, 77 + ji);
            dispatch(width, nt, fl, [&](auto Wc, auto NTc, auto FLc) {
                using io = IO<decltype(Wc)::value, decltype(NTc)::value, decltype(FLc)::value>;
                walk(R, ow, wo.rev, [&](u64 i) { u8* a = w.base + i * 64; Line l; j.g->make(w.g + i, a, j.inv, l); io::store(a, l); });
                _mm_sfence(); R.sync();
#ifdef DDR5TEST_INJECT  // self-test only: corrupt memory between write and verify
                if (tid == 0 && ji == 0) { u8* c = w.base + 640 * 64 + 20; c[0] ^= 0x05; c[1] ^= 0x10; w.base[900 * 64 + 4] ^= 0x80; for (int k = 0; k < 40; k++) w.base[(4096 + k) * 64 + 8] ^= 1; _mm_clflush(c); _mm_mfence(); }
                R.sync();
#endif
                if (j.dmode >= 0) { disturb_traffic(R, R.view(tid, wl, base + ji + 1), j.dmode, dpasses, 0xD157 + tid + ji + g_salt); _mm_sfence(); R.sync(); }
                if (j.dwell) { if (tid == 0) R.sleep_ms(j.dwell); R.sync(); }
                for (u64 r = 0; r < reads && !R.stop; r++)
                    walk(R, ov, vo.rev, [&](u64 i) {
                        const u8* a = vw.base + i * 64; Line e, act; j.g->make(vw.g + i, a, j.inv, e); R.vtick();
                        if (!io::verify(a, e, act)) R.report(tid, a, e, act, "verify");
                    });
            });
            R.sync();
        }
    });
}

// ------------------------------------------------------------------ algo: march (user-defined march tests)
struct MOp { char t; int v; };
struct MEl { char dir; bool delay; u64 ms; bool dist = false; int dmode = 0; u64 dpasses = 1; std::vector<MOp> ops; };
static std::vector<MEl> parse_march(const std::string& s) {
    std::vector<MEl> out;
    for (auto& tok : split_ws(s)) {
        if (tok.size() < 3) continue;
        MEl e{}; e.dir = tok[0]; size_t a = tok.find('('), b = tok.rfind(')');
        if (a == std::string::npos || b == std::string::npos) { emitf("bad march element '%s'\n", tok.c_str()); continue; }
        std::string body = tok.substr(a + 1, b - a - 1);
        if (e.dir == 'D' || e.dir == 'd') { e.delay = true; e.ms = pnum(body); out.push_back(e); continue; }
        if (e.dir == 'X' || e.dir == 'x') {  // disturb element: X(r2) = 2 read passes, X(w1), X(rw3) on another window
            size_t k = 0; while (k < body.size() && isalpha((unsigned char)body[k])) k++;
            e.dist = true; e.dmode = std::max(0, parse_dmode(lower(body.substr(0, k)))); e.dpasses = k < body.size() ? std::max<u64>(1, pnum(body.substr(k))) : 1;
            out.push_back(e); continue;
        }
        for (auto& op : split_ws(body, true)) if (op.size() == 2 && (op[0] == 'r' || op[0] == 'w')) e.ops.push_back({op[0], op[1] - '0'});
        out.push_back(e);
    }
    return out;
}
// "label=elements | label2=elements2": several marches in one test
struct MVar { std::string label; std::vector<MEl> els; };
static std::vector<MVar> parse_march_variants(const std::string& str) {
    std::vector<MVar> out; std::vector<std::string> parts; std::string cur;
    for (char c : str + "|") { if (c == '|') { parts.push_back(cur); cur.clear(); } else cur += c; }
    for (auto& raw : parts) {
        std::string part = trim(raw); if (part.empty()) continue;
        std::string label; size_t p = part.find('('), eq = part.find('=');
        if (eq != std::string::npos && (p == std::string::npos || eq < p)) { label = trim(part.substr(0, eq)); part = part.substr(eq + 1); }
        MVar v; v.els = parse_march(part); if (v.els.empty()) continue;
        v.label = label.empty() ? "march" + std::to_string(out.size() + 1) : label; out.push_back(v);
    }
    return out;
}
static void algo_march(Run& R, const Params& P) {
    auto vars = build_vars(P); auto mvs = parse_march_variants(P.str("march"));
    if (mvs.empty()) { emitf("march: no elements\n"); return; }
    std::vector<std::pair<std::string, OrderSpec>> orders;
    for (auto& w : P.words("order", "seq")) { OrderSpec os; if (!Order::parse(w, os.k, os.rev)) { emitf("bad order '%s'\n", w.c_str()); continue; } orders.push_back({w, os}); }
    if (orders.empty()) orders.push_back({"seq", OrderSpec()});
    bool anyStride = false; for (auto& o : orders) if (o.second.k == Order::STRIDE) anyStride = true;
    auto strides = stride_list(P, anyStride);
    auto bursts = P.nums("burst", "1"); if (bursts.empty()) bursts.push_back(1);
    int width = (int)P.num("width", 32); bool nt = P.flag("nt", false), fl = P.flag("flush", true);
    u64 wl = R.window_lines(P);
    bool anyDist = false; for (auto& m : mvs) for (auto& e : m.els) if (e.dist) anyDist = true;
    bool distOk = !anyDist || disturb_possible(R, wl, R.testName.c_str());
    struct Job { std::string label; const Gen* g; size_t mi; OrderSpec os; u64 stride; u64 burst; };
    std::vector<Job> jobs;
    for (auto& v : vars) for (size_t mi = 0; mi < mvs.size(); mi++) for (auto& on : orders) {
        std::vector<u64> sl; if (on.second.k == Order::STRIDE) sl = strides; else sl.push_back(1);
        for (u64 s : sl) for (u64 b : bursts)
            jobs.push_back({v.label + (mvs.size() > 1 ? " | " + mvs[mi].label : "") + (orders.size() > 1 || on.second.k != Order::SEQ || on.second.rev ? " order=" + on.first : "") +
                            (on.second.k == Order::STRIDE ? " stride=" + std::to_string(s * 64) + "B" : "") + (bursts.size() > 1 || b > 1 ? " burst=" + std::to_string(b) : ""),
                            &v.g, mi, on.second, s, b});
    }
    u64 base = R.winctr[R.testName]; R.winctr[R.testName] += jobs.size();
    R.parallel([&](int tid) {
        for (size_t ji = 0; ji < jobs.size(); ji++) {
            if (R.stop) break;
            const Job& j = jobs[ji]; const auto& els = mvs[j.mi].els; if (tid == 0) R.set_label(j.label);
            View w = R.view(tid, wl, base + ji); Order o; o.init(j.os.k, w.n, j.stride, 5 + ji);
            dispatch(width, nt, fl, [&](auto Wc, auto NTc, auto FLc) {
                using io = IO<decltype(Wc)::value, decltype(NTc)::value, decltype(FLc)::value>;
                for (size_t ei = 0; ei < els.size(); ei++) {
                    const MEl& e = els[ei];
                    if (e.delay) { R.sync(); if (tid == 0) R.sleep_ms(e.ms); R.sync(); continue; }
                    if (e.dist) {
                        if (distOk) disturb_traffic(R, R.view(tid, wl, base + ji + 1), e.dmode, e.dpasses, 0xD157 + tid * 31 + ji * 7 + ei + g_salt);
                        _mm_sfence(); R.sync(); continue;
                    }
                    bool down = (e.dir == 'v' || e.dir == 'V') != j.os.rev;
                    auto apply = [&](const MOp& op, u64 i) {
                        u8* a = w.base + i * 64; Line l; j.g->make(w.g + i, a, op.v != 0, l);
                        if (op.t == 'w') io::store(a, l);
                        else { Line act; R.vtick(); if (!io::verify(a, l, act)) R.report(tid, a, l, act, ("march el" + std::to_string(ei) + " r" + std::to_string(op.v)).c_str()); }
                    };
                    u64 cnt = 0;
                    for (u64 q0 = 0; q0 < w.n && !R.stop; q0 += j.burst) {
                        u64 qe = std::min(q0 + j.burst, w.n);
                        for (const MOp& op : e.ops) for (u64 q = q0; q < qe; q++) apply(op, o.pos(q, down));
                        cnt += qe - q0; if (cnt >= 0x10000) { R.tick(); cnt -= 0x10000; }
                    }
                    _mm_sfence(); R.sync();
                }
            });
        }
    });
}

// ------------------------------------------------------------------ algo: hammer (n-sided row hammer / activation-rate stress)
static void algo_hammer(Run& R, const Params& P) {
    auto vars = build_vars(P); auto strides = P.nums("rowstride", "256K 1M");
    auto sides = P.nums("aggressors", "2 9"); u64 acts = P.num("acts", 400000), rounds = P.num("rounds", 24), wl = R.window_lines(P);
    struct Job { std::string label; const Gen* g; u64 sL; u64 A; };
    std::vector<Job> jobs;
    for (auto& v : vars) for (u64 s : strides) for (u64 a : sides)
        jobs.push_back({v.label + " rowstride=" + std::to_string(s) + "B " + std::to_string(a) + "-sided", &v.g, std::max<u64>(1, s / 64), a});
    u64 base = R.winctr[R.testName]; R.winctr[R.testName] += jobs.size();
    R.parallel([&](int tid) {
        u64 rng = splitmix(0xABCD + tid * 7919 + g_salt);
        for (size_t ji = 0; ji < jobs.size(); ji++) {
            if (R.stop) break;
            const Job& j = jobs[ji]; if (tid == 0) R.set_label("hammer " + j.label);
            View w = R.view(tid, wl, base + ji); using io = IO<32, 0, 1>;
            for (u64 i = 0; i < w.n; i++) { u8* a = w.base + i * 64; Line l; j.g->make(w.g + i, a, false, l); io::store(a, l); if (((i + 1) & 0xFFFF) == 0) R.tick(); }
            R.sync();
            u64 span = 2 * (j.A - 1) * j.sL + 1;
            if (span < w.n) {
                for (u64 r = 0; r < rounds && !R.stop; r++) {
                    rng = splitmix(rng); u64 b0 = rng % (w.n - span);
                    std::vector<u8*> ag; for (u64 k = 0; k < j.A; k++) ag.push_back(w.base + (b0 + 2 * k * j.sL) * 64);
                    for (u8* a : ag) { Line l; j.g->make(0, a, true, l); io::store(a, l); }
                    for (u64 it = 0; it < acts; it++) {
                        for (u8* a : ag) { (void)*(volatile u64*)a; _mm_clflush(a); }
                        _mm_mfence();
                    }
                    for (size_t k = 0; k < ag.size(); k++) { u64 i = (ag[k] - w.base) / 64; Line l; j.g->make(w.g + i, ag[k], false, l); io::store(ag[k], l); }
                    R.done.fetch_add(acts * j.A * 64, std::memory_order_relaxed);
                }
            }
            R.sync();
            for (u64 i = 0; i < w.n && !R.stop; i++) {
                const u8* a = w.base + i * 64; Line e, act; j.g->make(w.g + i, a, false, e); R.vtick();
                if (!io::verify(a, e, act)) R.report(tid, a, e, act, "hammer-victim");
                if (((i + 1) & 0xFFFF) == 0) R.tick();
            }
            R.sync();
        }
    });
}

// ------------------------------------------------------------------ algo: copy (real-world bulk copy engines)
static void algo_copy(Run& R, const Params& P) {
    auto vars = build_vars(P); auto methods = P.words("method", "memcpy movsb ntcopy"); u64 blk = std::max<u64>(64, P.num("block", 64 * 1024)); u64 wl = R.window_lines(P);
    struct Job { std::string label; const Gen* g; std::string m; };
    std::vector<Job> jobs; for (auto& v : vars) for (auto& m : methods) jobs.push_back({v.label + " via " + m, &v.g, lower(m)});
    u64 base = R.winctr[R.testName]; R.winctr[R.testName] += jobs.size();
    R.parallel([&](int tid) {
        for (size_t ji = 0; ji < jobs.size(); ji++) {
            if (R.stop) break;
            const Job& j = jobs[ji]; if (tid == 0) R.set_label("copy " + j.label);
            View w = R.view(tid, wl, base + ji); u64 bl = blk / 64, nA = w.n / 2 / bl * bl; using io = IO<32, 0, 1>;
            u8* A = w.base; u8* B = w.base + nA * 64;
            for (u64 i = 0; i < nA; i++) { u8* a = A + i * 64; Line l; j.g->make(w.g + i, a, false, l); io::store(a, l); if (((i + 1) & 0xFFFF) == 0) R.tick(); }
            R.sync();
            for (u64 o = 0; o < nA && !R.stop; o += bl) {
                u8* s = A + o * 64; u8* d = B + o * 64; size_t nb = bl * 64;
                if (j.m == "memcpy") std::memcpy(d, s, nb);
                else if (j.m == "movsb") {
#if defined(_MSC_VER)
                    __movsb(d, s, nb);
#else
                    asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(nb) : : "memory");
#endif
                } else for (size_t k = 0; k < nb; k += 32) _mm256_stream_si256((__m256i*)(d + k), _mm256_load_si256((const __m256i*)(s + k)));
                R.done.fetch_add(bl * 64 * 2, std::memory_order_relaxed);
            }
            _mm_sfence(); R.sync();
            for (int side = 0; side < 2 && !R.stop; side++)
                for (u64 i = 0; i < nA; i++) {
                    const u8* a = (side ? B : A) + i * 64; Line e, act; j.g->make(w.g + i, A + i * 64, false, e); R.vtick();
                    if (!io::verify(a, e, act)) R.report(tid, a, e, act, side ? "copy-dst" : "copy-src");
                    if (((i + 1) & 0xFFFF) == 0) R.tick();
                }
            R.sync();
        }
    });
}

// ------------------------------------------------------------------ algo: chase (random pointer chase, K independent chains = max bank-level parallelism)
static void algo_chase(Run& R, const Params& P) {
    auto mlps = P.nums("mlp", "12"); if (mlps.empty()) mlps.push_back(12);
    u64 seeds = std::max<u64>(1, P.num("seeds", 2)), wl = R.window_lines(P);
    u64 base = R.winctr[R.testName]; R.winctr[R.testName] += seeds * mlps.size();
    R.parallel([&](int tid) {
        for (u64 sd = 0; sd < seeds; sd++) for (size_t mi = 0; mi < mlps.size(); mi++) {
            if (R.stop) break;
            u64 K = std::max<u64>(1, mlps[mi]);
            if (tid == 0) R.set_label("chase seed " + std::to_string(sd) + " mlp=" + std::to_string(K));
            View w = R.view(tid, wl, base + sd * mlps.size() + mi); Order o;
            u64 seed = splitmix(sd * 31 + tid + g_salt + mi * 977), salt = splitmix(seed ^ 0x5A5A);
            o.init(Order::RANDOM, w.n, 1, seed); using io = IO<32, 0, 1>;
            auto make = [&](u64 idx, u64 next, Line& l) {
                l.w[0] = next; l.w[1] = ~next ^ salt; for (int k = 2; k < 8; k++) l.w[k] = splitmix((idx << 3 | k) ^ seed);
            };
            for (u64 i = 0; i < w.n; i++) {
                u64 idx = o.at(i), nx = o.at(i + 1 == w.n ? 0 : i + 1); Line l; make(idx, nx, l); io::store(w.base + idx * 64, l);
                if (((i + 1) & 0xFFFF) == 0) { R.tick(); if (R.stop) break; }
            }
            _mm_sfence(); R.sync();
            u64 steps = w.n / K; std::vector<u64> p(K), bad(K, 0);
            for (u64 k = 0; k < K; k++) p[k] = o.at(k * steps);
            for (u64 st = 0; st < steps && !R.stop; st++) {
                for (u64 k = 0; k < K; k++) {
                    if (bad[k]) continue;
                    const u8* a = w.base + p[k] * 64; Line act, e; CBAR(); IO<32, 0, 0>::load(a, act); _mm_clflush(a); CBAR();
                    make(p[k], act.w[0], e); R.vtick();
                    if (memcmp(&act, &e, 64) != 0 || act.w[0] >= w.n) { R.report(tid, a, e, act, "chase"); bad[k] = 1; continue; }
                    p[k] = act.w[0];
                }
                if (((st + 1) & 0xFFFF) == 0) { R.done.fetch_add(0x10000ull * 64 * K, std::memory_order_relaxed); }
            }
            R.sync();
        }
    });
}

// ------------------------------------------------------------------ algo: rwstorm (concurrent read + write traffic on the same memory)
// Even threads keep rewriting their chunk with IDENTICAL data while their odd partner thread verifies that chunk at the
// same time. Stores and loads are in flight together (bus turnaround, write queue drain, read-after-write hazards in the
// controller, power-delivery load) yet every read must still match. Finally every thread verifies its own chunk.
static void algo_rwstorm(Run& R, const Params& P) {
    auto vars = build_vars(P); OrderSpec wo = ospec(P, "order", "random"), vo = ospec(P, "verify_order", "seq");
    auto strides = stride_list(P, wo.k == Order::STRIDE || vo.k == Order::STRIDE);
    int width = (int)P.num("width", 32); bool nt = P.flag("nt", false), fl = P.flag("flush", true);
    u64 passes = std::max<u64>(1, P.num("passes", 4)), wl = R.window_lines(P);
    struct Job { std::string label; const Gen* g; u64 stride; };
    std::vector<Job> jobs; for (auto& v : vars) for (u64 s : strides) jobs.push_back({v.label, &v.g, s});
    u64 base = R.winctr[R.testName]; R.winctr[R.testName] += jobs.size();
    R.parallel([&](int tid) {
        for (size_t ji = 0; ji < jobs.size(); ji++) {
            if (R.stop) break;
            const Job& j = jobs[ji]; if (tid == 0) R.set_label("rwstorm " + j.label);
            bool writer = (tid % 2) == 0; int partner = writer ? tid : tid - 1;
            View w = R.view(tid, wl, base + ji), pv = R.view(partner, wl, base + ji);
            Order ow, ov, of; ow.init(wo.k, w.n, j.stride, 3 + ji); ov.init(vo.k, pv.n, j.stride, 9 + ji); of.init(Order::SEQ, w.n, 1, 0);
            dispatch(width, nt, fl, [&](auto Wc, auto NTc, auto FLc) {
                using io = IO<decltype(Wc)::value, decltype(NTc)::value, decltype(FLc)::value>;
                auto wr = [&](u64 i) { u8* a = w.base + i * 64; Line l; j.g->make(w.g + i, a, false, l); io::store(a, l); };
                auto rd = [&](const View& v) { return [&, v](u64 i) {
                    const u8* a = v.base + i * 64; Line e, act; j.g->make(v.g + i, a, false, e); R.vtick();
                    if (!io::verify(a, e, act)) R.report(tid, a, e, act, "rwstorm-read"); }; };
                walk(R, of, false, wr); _mm_sfence(); R.sync();   // initial fill of own chunk
                for (u64 p = 0; p < passes && !R.stop; p++) {
                    if (writer) walk(R, ow, wo.rev, wr);           // rewrite same data
                    else walk(R, ov, vo.rev, rd(pv));              // concurrently verify partner's chunk
                }
                _mm_sfence(); R.sync();
                walk(R, of, false, rd(w));                         // final check of own chunk
            });
            R.sync();
        }
    });
}

// ------------------------------------------------------------------ algo: probe (row-hit vs row-conflict latency by address distance)
// Informational, not pass/fail. Reads line A, then times a read of line B = A + stride (both flushed from cache first).
// B in the same bank but a different row => row conflict (PRE + ACT + CAS: ~tRP + tRCD + CL), noticeably slower.
// B in another bank, or the same row => faster. The slow-fraction per stride shows which address bits feed the bank
// function, and (slow - fast) approximates tRP + tRCD as seen from the core (includes noise, so treat as +-several ns).
static void algo_probe(Run& R, const Params& P) {
    auto strides = P.nums("stride", "64 128 256 512 1K 2K 4K 8K 16K 32K 64K 128K 256K 512K 1M 2M 4M 8M 16M");
    u64 samples = std::max<u64>(1000, P.num("samples", 20000));
    R.parallel([&](int tid) {
        if (tid != 0) return;  // single thread, rest idle, so measurements are clean
        const Chunk& c = R.chunks[0];
        R.set_label("probe: calibrating TSC");
        double t0 = plat::now(); u64 c0 = __rdtsc(); R.sleep_ms(80); double t1 = plat::now(); u64 c1 = __rdtsc();
        double ghz = (double)(c1 - c0) / (t1 - t0) / 1e9;
        std::vector<double> oh;  // measurement overhead
        for (int i = 0; i < 2001; i++) { _mm_lfence(); u64 a = __rdtsc(); _mm_lfence(); u64 b = __rdtsc(); _mm_lfence(); oh.push_back((double)(b - a)); }
        std::sort(oh.begin(), oh.end()); double ovh = oh[oh.size() / 2];
        std::vector<std::vector<double>> res(strides.size()); std::vector<double> all; u64 rng = splitmix(0x9876);
        for (size_t si = 0; si < strides.size() && !R.stop; si++) {
            u64 sl = std::max<u64>(1, strides[si] / 64); if (sl + 2 >= c.lines) continue;
            R.set_label("probe stride " + std::to_string(strides[si]) + "B");
            for (u64 k = 0; k < samples && !R.stop; k++) {
                rng = splitmix(rng); u64 bl = rng % (c.lines - sl - 1);
                const u8* A = c.base + bl * 64; const u8* B = A + sl * 64;
                (void)*(volatile const u64*)A; (void)*(volatile const u64*)B;   // prime TLB
                _mm_clflush(A); _mm_clflush(B); _mm_mfence();
                CBAR(); (void)*(volatile const u64*)A; _mm_lfence();
                u64 a = __rdtsc(); _mm_lfence(); (void)*(volatile const u64*)B; _mm_lfence(); u64 b = __rdtsc(); _mm_lfence(); CBAR();
                double ns = std::max(0.0, ((double)(b - a) - ovh) / ghz);
                res[si].push_back(ns); all.push_back(ns);
            }
        }
        if (all.empty()) return;
        std::sort(all.begin(), all.end());
        auto pct = [](std::vector<double>& v, double p) { return v[std::min(v.size() - 1, (size_t)(p * v.size()))]; };
        double lo = all[all.size() / 10], hi = pct(all, 0.97), thr = (lo + hi) / 2;
        print_clear();
        emitf("    TSC %.3f GHz, overhead %.0f cycles removed. fast cluster ~%.0f ns, slow cluster ~%.0f ns, threshold %.0f ns\n", ghz, ovh, lo, hi, thr);
        emitf("    stride        median   p10    p90   slow%%   (slow = likely same bank, different row)\n");
        double minSlow = 100, maxSlow = 0;
        for (size_t si = 0; si < strides.size(); si++) {
            if (res[si].empty()) continue; auto& v = res[si]; std::sort(v.begin(), v.end());
            size_t slow = 0; for (double x : v) if (x > thr) slow++;
            double sp = 100.0 * slow / v.size(); minSlow = std::min(minSlow, sp); maxSlow = std::max(maxSlow, sp);
            emitf("    %-12llu %6.0f %6.0f %6.0f  %5.1f%%\n", (unsigned long long)strides[si], pct(v, 0.5), pct(v, 0.1), pct(v, 0.9), sp);
        }
        if (maxSlow - minSlow >= 15.0) {
            emitf("    slow - fast = %.0f ns  ~ tRP + tRCD (core-visible, noisy; compare with BIOS/ZenTimings/HWiNFO values)\n", hi - lo);
            emitf("    Strides with a high slow%% hit the same bank in another row; the bits that flip between them feed the bank/row hash.\n");
        } else {
            emitf("    No stride separates into a distinct fast/slow group (slow%% spread %.1f..%.1f): bank structure NOT resolvable here,\n"
                   "    so no tRP+tRCD estimate is given. Typical causes: virtual machine, small-page TLB noise (use large pages), SMT/background load.\n", minSlow, maxSlow);
        }
    });
}

// ------------------------------------------------------------------ algo: pagemix (stressapptest-style page shuffle)
// All memory is split into 4 KB pages and every page gets a random pattern from the stressapptest-style table. Threads then pick
// random page pairs and (a) copy source -> destination while verifying the source line by line, (b) invert a page in place
// 4 times (up, down, down, up - net identity, with flushes), (c) refill a page with a new pattern, or (d) re-check a page. The
// destination takes the source's pattern, so data keeps moving between cores, banks and channels while every line stays
// predictable. Hot (cache-resident) and flushed (DRAM round-trip) variants are mixed. Finally every page is verified.
struct PagePat { u16 pat; u8 rep, inv; };
template <int FL> static void pm_fill(u8* p, const Gen& g, bool inv) {
    using io = IO<32, 0, FL>; for (int l = 0; l < 64; l++) { Line e; g.make(l, p + l * 64, inv, e); io::store(p + l * 64, e); }
}
template <int FL> static void pm_check(Run& R, int tid, u8* p, const Gen& g, bool inv, const char* ph) {
    using io = IO<32, 0, FL>;
    for (int l = 0; l < 64; l++) {
        Line e, a; g.make(l, p + l * 64, inv, e); R.vtick();
        if (!io::verify(p + l * 64, e, a)) { R.report(tid, p + l * 64, e, a, ph); io::store(p + l * 64, e); }
    }
}
template <int FL> static void pm_copy(Run& R, int tid, u8* s, u8* d, const Gen& g, bool inv) {
    using io = IO<32, 0, FL>;
    for (int l = 0; l < 64; l++) {
        Line e, a; g.make(l, s + l * 64, inv, e); R.vtick();
        if (!io::verify(s + l * 64, e, a)) { R.report(tid, s + l * 64, e, a, "pagemix-src"); io::store(s + l * 64, e); }
        io::store(d + l * 64, e);   // what was read equals e when the check passes; after a failure the destination gets good data
    }
}
template <int FL> static void pm_invert(u8* p) {
    static const int dir[4] = {0, 1, 1, 0};   // up, down, down, up
    const __m256i ones = _mm256_set1_epi32(-1);
    for (int pass = 0; pass < 4; pass++) for (int k = 0; k < 64; k++) {
        u8* a = p + (dir[pass] ? 63 - k : k) * 64; CBAR();
        __m256i x = _mm256_xor_si256(_mm256_load_si256((const __m256i*)a), ones), y = _mm256_xor_si256(_mm256_load_si256((const __m256i*)a + 1), ones);
        _mm256_store_si256((__m256i*)a, x); _mm256_store_si256((__m256i*)a + 1, y);
        if constexpr (FL) _mm_clflush(a);
        CBAR();
    }
}
static void algo_pagemix(Run& R, const Params& P) {
    double secs = (double)P.num("seconds", 30); const auto& tab = sat_table();
    u64 np = R.regionBytes / 4096; if (np < (u64)R.T * 8) { emitf("    pagemix: region too small, skipped\n"); return; }
    std::unique_ptr<std::atomic<u8>[]> busy(new std::atomic<u8>[np]); for (u64 i = 0; i < np; i++) busy[i].store(0, std::memory_order_relaxed);
    std::vector<PagePat> pp(np);
    u64 wsum = 0; for (auto& t : tab) for (int w = 0; w < 4; w++) wsum += t.weight[w];
    auto pick = [&](u64 r) { PagePat q{}; u64 x = r % (wsum * 2); q.inv = (u8)(x & 1); x >>= 1;
        for (size_t i = 0; i < tab.size(); i++) for (int w = 0; w < 4; w++) { if (x < (u64)tab[i].weight[w]) { q.pat = (u16)i; q.rep = (u8)w; return q; } x -= tab[i].weight[w]; }
        return q; };
    auto gen = [&](const PagePat& q) { Gen g; g.kind = 3; g.sp = &tab[q.pat]; g.rep = q.rep; return g; };
    R.parallel([&](int tid) {
        u64 rng = splitmix(0x5A7 + tid * 7919 + g_salt); auto nx = [&] { rng = splitmix(rng); return rng; };
        if (tid == 0) R.set_label("page shuffle: fill");
        for (u64 pg = tid; pg < np && !R.stop; pg += R.T) {
            pp[pg] = pick(nx()); u8* a = R.region + pg * 4096; Gen g = gen(pp[pg]); pm_fill<1>(a, g, pp[pg].inv);
            if (((pg / R.T + 1) & 0x3FF) == 0) R.tick();
        }
        _mm_sfence(); R.sync();
        if (tid == 0) R.set_label("page shuffle: copy / invert / refill / check");
        double end = plat::now() + secs; u64 n = 0;
        while (!R.stop) {
            if ((++n & 63) == 0 && plat::now() >= end) break;
            u64 r = nx(); u64 s = (r >> 8) % np, d = nx() % np; int op = (int)(r % 10); bool fl = (r >> 5) & 1;
            if (op <= 5 && d == s) continue;
            if (busy[s].exchange(1, std::memory_order_acquire)) continue;
            bool two = op <= 5;
            if (two && busy[d].exchange(1, std::memory_order_acquire)) { busy[s].store(0, std::memory_order_release); continue; }
            u8* sa = R.region + s * 4096; u8* da = R.region + d * 4096; PagePat sq = pp[s]; Gen sg = gen(sq);
            if (op <= 5) { if (fl) pm_copy<1>(R, tid, sa, da, sg, sq.inv); else pm_copy<0>(R, tid, sa, da, sg, sq.inv); pp[d] = sq; R.done.fetch_add(8192, std::memory_order_relaxed); }
            else if (op == 6) { PagePat nq = pick(nx()); Gen ng = gen(nq); if (fl) pm_fill<1>(sa, ng, nq.inv); else pm_fill<0>(sa, ng, nq.inv); pp[s] = nq; R.done.fetch_add(4096, std::memory_order_relaxed); }
            else if (op <= 8) { if (fl) pm_invert<1>(sa); else pm_invert<0>(sa); R.done.fetch_add(4096 * 4, std::memory_order_relaxed); }
            else { if (fl) pm_check<1>(R, tid, sa, sg, sq.inv, "pagemix-check"); else pm_check<0>(R, tid, sa, sg, sq.inv, "pagemix-check"); R.done.fetch_add(4096, std::memory_order_relaxed); }
            if (two) busy[d].store(0, std::memory_order_release);
            busy[s].store(0, std::memory_order_release);
        }
        _mm_sfence(); R.sync();
#ifdef DDR5TEST_INJECT  // self-test only
        if (tid == 0) { R.region[3 * 4096 + 100] ^= 0x04; _mm_clflush(R.region + 3 * 4096 + 64); _mm_mfence(); }
        R.sync();
#endif
        if (tid == 0) R.set_label("page shuffle: final verify of all pages");
        for (u64 pg = tid; pg < np; pg += R.T) {
            Gen g = gen(pp[pg]); pm_check<1>(R, tid, R.region + pg * 4096, g, pp[pg].inv, "pagemix-final");
            if (((pg / R.T + 1) & 0x3FF) == 0) R.tick();
        }
        R.sync();
    });
}

// ------------------------------------------------------------------ algo: coherency (stressapptest-style cache-coherency check)
// A few cache lines are shared by all threads. Every thread owns one byte in each line and increments it at random lines (false
// sharing: the line ping-pongs between cores while each core only touches its own byte). After a round every thread sums its bytes
// over all lines and compares the low 8 bits with the number of increments it made. A lost or stale write = coherency fault. This
// exercises the cores' caches and the fabric between them (CCX/CCD, ring), not DRAM cells.
static void algo_coherency(Run& R, const Params& P) {
    auto lines = P.nums("lines", "2 8 64"); u64 inc = std::max<u64>(1, P.num("inc", 1000)); double secs = (double)P.num("seconds", 8);
    int nt = std::min(R.T, 64); std::atomic<bool> fin{false};
    R.parallel([&](int tid) {
        for (u64 L : lines) {
            L = std::min<u64>(std::max<u64>(L, 1), 64); u8* mem = R.region;
            if (tid == 0) { R.set_label("cache coherency, " + std::to_string(L) + " shared lines, " + std::to_string(nt) + " threads"); std::memset(mem, 0, L * 64); fin = false; }
            double end = plat::now() + secs; u32 x = (u32)(splitmix(tid + 1 + g_salt + L * 977) | 1); R.sync();
            for (;;) {
                if (tid < nt) {
                    for (u64 i = 0; i < inc; i++) {
                        x = (x >> 1) ^ ((0u - (x & 1)) & 0xD0000001u); u64 line = x % L;
                        u64 off = (line & (u64)tid & 1) ? (u64)(nt & ~1) - tid : (u64)tid;   // owner byte position differs between lines
                        volatile u8* c = mem + line * 64 + off; *c = (u8)(*c + 1);
                    }
                }
#ifdef DDR5TEST_INJECT  // self-test only: one lost/extra increment
                if (tid == 0) { static bool once = false; if (!once) { once = true; mem[0]++; } }
#endif
                R.sync();
                if (tid < nt) {
                    u64 sum = 0;
                    for (u64 line = 0; line < L; line++) {
                        u64 off = (line & (u64)tid & 1) ? (u64)(nt & ~1) - tid : (u64)tid; volatile u8* c = mem + line * 64 + off; sum += *c; *c = 0;
                    }
                    if ((sum & 0xFF) != (inc & 0xFF)) { Line e{}, a{}; e.w[0] = inc & 0xFF; a.w[0] = sum & 0xFF; R.report(tid, mem + tid, e, a, "coherency", false); }
                    R.done.fetch_add(inc * 8, std::memory_order_relaxed);
                }
                R.sync();
                if (tid == 0) fin = R.stop.load() || plat::now() >= end;
                R.sync();
                if (fin.load()) break;
            }
            R.sync();
        }
    });
}

// ====================================================================== suite + run API (shared by the GUI and the CLI front-ends)
using AlgoFn = void (*)(Run&, const Params&);
static const std::map<std::string, AlgoFn> ALGOS = {{"fill", algo_fill}, {"march", algo_march}, {"hammer", algo_hammer}, {"copy", algo_copy},
                                                    {"chase", algo_chase}, {"rwstorm", algo_rwstorm}, {"probe", algo_probe},
                                                    {"pagemix", algo_pagemix}, {"coherency", algo_coherency}};

#include "suite_data.h"   // generated from suite.cfg by tools/gen_suite.py: the whole test list is compiled in

static std::string suite_text() { std::string t; for (int i = 0; i < SUITE_CFG_NPARTS; i++) t += SUITE_CFG_PARTS[i]; return t; }

struct Settings {
    u64 megabytes = 0;            // 0 = auto (90% of free memory)
    int threads = 0;              // 0 = all logical CPUs
    bool stopOnError = true;      // beeping is done by the front-end
    int cacheMode = 0;            // 0 default (per test), 1 bypass (clflush every access), 2 cached, 3 non-temporal stores
    int rngMode = 0;              // 0 default, 1 SplitMix64, 2 Murmur3 fmix64, 3 xorshift64*
    bool stressFpu = false;       // extra FPU/AVX load threads (more current draw while testing memory)
    bool largePages = true;
    int suite = 0;                // 0 full, 1 quick (no long pauses / hammer)
    std::string logFile = "ddr5test.log";
    u64 coverageStop = 0; double minutes = 0; u64 loops = 0; std::string filter;
};

static void emit_skew(const char* prefix, const Stats& s, u64 regionBytes) {
    u64 n = s.lines.load(); if (n < 16) return;
    std::string out;
    for (int b = 6; b < 48 && (1ull << b) < regionBytes; b++) {
        double f = (double)s.abit[b].load() / (double)n;
        if (f >= 0.85 || f <= 0.15) { char buf[64]; snprintf(buf, sizeof buf, " bit%d=%d(%.0f%%)", b, f >= 0.85 ? 1 : 0, 100.0 * (f >= 0.85 ? f : 1 - f)); out += buf; }
    }
    if (out.empty()) emitf("%saddress bits: errors are spread evenly (no single address bit explains them)\n", prefix);
    else emitf("%saddress-bit skew (>=85%% of errors share a value):%s\n%s-> errors cluster on one side of these bits: suspect one rank/bank/row/subchannel region\n", prefix, out.c_str(), prefix);
}

static volatile float g_fpu_sink = 0;
static void fpu_stress(std::atomic<bool>& stop) {
    plat::lower_priority();
    __m256 x[8]; for (int i = 0; i < 8; i++) x[i] = _mm256_set1_ps(1.0f + 0.01f * (float)i);
    const __m256 m = _mm256_set1_ps(1.0000001f), k = _mm256_set1_ps(0.0000001f);
    while (!stop.load(std::memory_order_relaxed)) {
        for (int it = 0; it < 20000; it++) for (int i = 0; i < 8; i++) x[i] = _mm256_add_ps(_mm256_mul_ps(x[i], m), k);
        float t[8]; _mm256_storeu_ps(t, x[0]); g_fpu_sink = g_fpu_sink + t[0];
    }
}

// Runs the built-in suite until finished or R.stop. Poll R (vtotal, errTotal, label, tStart, ...) from another thread.
static int run_suite(const Settings& s, Run& R) {
    try {
        Cfg cfg; std::vector<TestDef> tests;
        { std::istringstream is(suite_text()); parse_cfg_stream(is, "<built-in suite>", cfg, tests); }
        cfg.threads = s.threads; cfg.large = s.largePages; cfg.stopOnErr = s.stopOnError; cfg.logFile = s.logFile; cfg.loops = s.loops;
        g_rng = s.rngMode == 2 ? 1 : s.rngMode == 3 ? 2 : 0;
        std::vector<TestDef> sel; std::string flt = lower(s.filter);
        for (auto& t : tests) {
            if (t.p.has("enabled") && !t.p.flag("enabled", true)) continue;
            if (s.suite == 1 && !t.p.flag("quick", false)) continue;
            if (!flt.empty() && lower(t.name).find(flt) == std::string::npos) continue;
            std::string algo = lower(t.p.str("algo", "fill")); if (!ALGOS.count(algo)) continue;
            if (s.cacheMode && (algo == "fill" || algo == "march" || algo == "rwstorm")) {
                t.p.m["flush"] = s.cacheMode == 1 ? "1" : "0"; t.p.m["nt"] = s.cacheMode == 3 ? "1" : "0";
            }
            sel.push_back(t);
        }
        if (sel.empty()) { emitf("no tests selected\n"); return 2; }

        R.cfg = cfg; R.T = cfg.threads > 0 ? cfg.threads : std::max(1, plat::ncpu());
        R.stopCoverage = s.coverageStop; R.stopSeconds = s.minutes * 60.0; R.stop = false;
        const u64 MB2 = 2ull << 20; u64 avail = plat::avail_phys(), want;
        if (s.megabytes) want = s.megabytes << 20; else want = avail / 100 * 90;
        if (want > avail / 100 * 97) { want = avail / 100 * 97; emitf("requested memory exceeds free memory; reduced to %llu MB to avoid paging\n", (unsigned long long)(want >> 20)); }
        u64 per = want / R.T / MB2 * MB2; if (!per) { emitf("not enough memory (need at least 2 MB per thread)\n"); return 2; }
        u64 total = per * R.T; bool gotLP = false;
        emitf("%d threads, testing %.2f GiB of %.2f GiB free, %zu tests\n", R.T, total / 1073741824.0, avail / 1073741824.0, sel.size());
        R.region = (u8*)plat::alloc(total, cfg.large, gotLP);
        if (!R.region) { emitf("allocation of %llu MB failed\n", (unsigned long long)(total >> 20)); return 2; }
        R.regionBytes = total;
        emitf("memory locked, large pages: %s\n", gotLP ? "yes (2MB contiguous physical runs)" : cfg.large ? "no (needs 'Lock pages in memory' right; using normal pages)" : "off");
        R.chunks.clear(); for (int t = 0; t < R.T; t++) R.chunks.push_back({R.region + (u64)t * per, per / 64, (u64)t * (per / 64)});
        R.running = true;
        R.parallel([&](int tid) { std::memset(R.chunks[tid].base, 0, per); });
        // CLI: errors also go to the log file. GUI: the front-end's emit sink already writes everything (incl. errors) to its log.
        R.logf = g_emit ? nullptr : fopen(cfg.logFile.c_str(), "a");
        if (R.logf) fprintf(R.logf, "---- ddr5test run, %.2f GiB, %d threads ----\n", total / 1073741824.0, R.T);

        std::atomic<bool> fpuStop{false}; std::vector<std::thread> fpu;
        if (s.stressFpu) for (int i = 0; i < R.T; i++) fpu.emplace_back(fpu_stress, std::ref(fpuStop));

        double t0 = plat::now(); R.tStart = t0; int failedTests = 0;
        for (u64 loop = 1; (!cfg.loops || loop <= cfg.loops) && !R.stop; loop++) {
            g_salt = (loop - 1) * 0x9E3779B97F4A7C15ull;  // loop 1 is reproducible; later loops use fresh seeds/permutations
            for (size_t ti = 0; ti < sel.size() && !R.stop; ti++) {
                TestDef& t = sel[ti]; R.testName = t.name; R.st.clear(); R.done = 0; R.vbytes = 0; R.firstErrTest = -1; R.tTestStart = plat::now();
                R.set_label(t.name);
                emitf("[loop %llu  %zu/%zu] %s\n", (unsigned long long)loop, ti + 1, sel.size(), t.name.c_str());
                double ts = plat::now(); ALGOS.at(lower(t.p.str("algo", "fill")))(R, t.p); double el = plat::now() - ts;
                u64 bad = R.st.lines.load();
                emitf("    %s  %.1fs  %.1f GB/s avg   coverage this test %.1fx\n", bad ? "FAIL" : (R.stop ? "STOP" : "pass"), el, R.done.load() / 1e9 / std::max(el, 1e-9), (double)R.vbytes.load() / (double)total);
                if (bad) {
                    failedTests++;
                    emitf("    %llu bad lines, %llu bits; first error %.1f s into this test (%.1f s into the run)\n", (unsigned long long)bad, (unsigned long long)R.st.bits.load(), R.firstErrTest, R.firstErrRun);
                    emit_skew("    ", R.st, total);
                    std::string l1 = "    DQ lane errors  : "; for (int l = 0; l < 32; l++) if (R.st.lane[l]) l1 += "[" + std::to_string(l) + "]=" + std::to_string(R.st.lane[l].load()) + " ";
                    std::string l2 = "    BL16 beat errors: "; for (int b = 0; b < 16; b++) if (R.st.beat[b]) l2 += "[" + std::to_string(b) + "]=" + std::to_string(R.st.beat[b].load()) + " ";
                    emit(l1 + "\n" + l2 + "\n");
                    emitf("    128b ODECC-word bit-count 1/2/3-4/5+: %llu/%llu/%llu/%llu   re-read: transient=%llu stable=%llu unstable=%llu\n",
                          (unsigned long long)R.st.blk[0].load(), (unsigned long long)R.st.blk[1].load(), (unsigned long long)R.st.blk[2].load(), (unsigned long long)R.st.blk[3].load(),
                          (unsigned long long)R.st.trans.load(), (unsigned long long)R.st.stable.load(), (unsigned long long)R.st.unstable.load());
                    R.total.lines += bad; R.total.bits += R.st.bits.load();
                    for (int b = 0; b < 48; b++) R.total.abit[b] += R.st.abit[b].load();
                }
            }
        }
        fpuStop = true; for (auto& th : fpu) th.join();
        double el = plat::now() - t0;
        emitf("\nfinished in %.0fs: %s (%llu bad lines, %d failing test runs)%s%s\n", el, R.total.lines ? "ERRORS FOUND" : "no errors", (unsigned long long)R.total.lines.load(), failedTests,
              R.stopReason.empty() ? "" : "  - ", R.stopReason.c_str());
        emitf("total coverage: %.1fx of the %.2f GiB tested region verified\n", (double)R.vtotal.load() / (double)total, total / 1073741824.0);
        if (R.total.lines) { emitf("first error: %.1f s into the run\nwhole run:\n", R.firstErrRun); emit_skew("    ", R.total, total); }
        if (R.logf) { fclose(R.logf); R.logf = nullptr; }
        plat::release(R.region, total); R.region = nullptr;
        R.running = false;
        return R.total.lines ? 1 : 0;
    } catch (const std::exception& e) {
        emitf("internal error: %s\n", e.what()); R.running = false; return 3;
    }
}
