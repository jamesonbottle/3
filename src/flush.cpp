// flush.cpp - run-time choice between CLFLUSH and CLFLUSHOPT (see flush.h).
#include "flush.h"
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace ddr5 {

bool g_flush_opt = false;

bool cpu_has_clflushopt() {
#if defined(_MSC_VER)
    int r[4] = {0, 0, 0, 0};
    __cpuid(r, 0);
    if (r[0] < 7)
        return false;
    __cpuidex(r, 7, 0);
    return (r[1] >> 23) & 1;
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) // also false when leaf 7 is above the CPU's maximum leaf
        return false;
    return (b >> 23) & 1;
#endif
}

void set_flush_mode(FlushMode m) {
    g_flush_opt = m != FlushMode::ClFlush && cpu_has_clflushopt();
}

bool flush_uses_opt() {
    return g_flush_opt;
}

const char* flush_mode_name() {
    return g_flush_opt ? "CLFLUSHOPT" : "CLFLUSH";
}

} // namespace ddr5
