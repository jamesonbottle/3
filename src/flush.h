#pragma once
// flush.h - cache-line flush used by every "flush = 1" access (so the next read really comes from DRAM).
//
// CLFLUSH is ordered with respect to other CLFLUSHes, so a thread can have only one in flight; CLFLUSHOPT has no such
// ordering and lets them overlap, which is several times faster on many CPUs (measured on a VM: 0.8 -> 5.7 GB/s for a
// plain pattern test, see AUDIT_PLAN.md N1). A flush never changes data, so a reordered or late flush can only make a
// later read hit the cache (a missed check), never produce a false error. The phases are still separated by
// _mm_sfence() + a thread barrier (locked operations), which order CLFLUSHOPT.
//
// The mode is chosen once, before the threads start (set_flush_mode); the hot path only reads a plain bool.
#include "types.h"

namespace ddr5 {

enum class FlushMode : int {
    Auto,      // CLFLUSHOPT when the CPU has it, otherwise CLFLUSH
    ClFlush,   // always CLFLUSH (the reference behaviour, for comparison)
    ClFlushOpt // CLFLUSHOPT; falls back to CLFLUSH (and says so) when the CPU does not have it
};

bool cpu_has_clflushopt();        // CPUID.(EAX=7,ECX=0):EBX bit 23
void set_flush_mode(FlushMode m); // call while no run is active; resolves Auto / unsupported against the CPU
bool flush_uses_opt();            // what the hot path will execute now
const char* flush_mode_name();    // "CLFLUSHOPT" or "CLFLUSH", for the log

extern bool g_flush_opt; // written only by set_flush_mode(), never while workers run

inline void flush_line(const void* p) {
    if (g_flush_opt) {
#if defined(_MSC_VER)
        _mm_clflushopt((void*)p);
#else
        // inline asm so no -mclflushopt is needed (the build only enables AVX2)
        asm volatile("clflushopt %0" ::"m"(*(const char*)p) : "memory");
#endif
    } else {
        _mm_clflush(p);
    }
}

} // namespace ddr5
