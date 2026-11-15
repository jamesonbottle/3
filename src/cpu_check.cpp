// cpu_check.cpp - run-time AVX2 check (see cpu_check.h). Built WITHOUT -mavx2 / /arch:AVX2: see CMakeLists.txt.
#include "cpu_check.h"
#include <cstdlib>
#if defined(_MSC_VER)
#include <immintrin.h>
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace ddr5 {

Avx2Status avx2_decide(bool leaf7Ok, unsigned l1ecx, unsigned l7ebx, unsigned long long xcr0) {
    const bool osxsave = (l1ecx >> 27) & 1, avx = (l1ecx >> 28) & 1, avx2 = leaf7Ok && ((l7ebx >> 5) & 1);
    if (!avx || !avx2)
        return Avx2Status::NoCpu;
    if (!osxsave || (xcr0 & 6) != 6) // XCR0 bit 1 (SSE) and bit 2 (YMM) must both be enabled by the OS
        return Avx2Status::NoOs;
    return Avx2Status::Ok;
}

Avx2Status avx2_status() {
    unsigned l1ecx = 0, l7ebx = 0;
    unsigned long long xcr0 = 0;
    bool leaf7Ok = false;
#if defined(_MSC_VER)
    int r[4] = {0, 0, 0, 0};
    __cpuid(r, 0);
    const int maxLeaf = r[0];
    if (maxLeaf < 1)
        return Avx2Status::NoCpu;
    __cpuid(r, 1);
    l1ecx = (unsigned)r[2];
    if (maxLeaf >= 7) {
        __cpuidex(r, 7, 0);
        l7ebx = (unsigned)r[1];
        leaf7Ok = true;
    }
    if ((l1ecx >> 27) & 1) // XGETBV faults when the OS has not set OSXSAVE
        xcr0 = _xgetbv(0);
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d))
        return Avx2Status::NoCpu;
    l1ecx = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) { // false also when leaf 7 is above the CPU's highest leaf
        l7ebx = b;
        leaf7Ok = true;
    }
    if ((l1ecx >> 27) & 1) { // XGETBV faults when the OS has not set OSXSAVE; inline asm so no -mxsave is needed
        unsigned lo = 0, hi = 0;
        asm volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        xcr0 = ((unsigned long long)hi << 32) | lo;
    }
#endif
#ifdef DDR5TEST_INJECT
    if (const char* f = getenv("DDR5TEST_FAKE_AVX2")) { // self-test only: pretend to be a machine without it
        if (f[0] == 'c')
            l7ebx &= ~(1u << 5);
        else if (f[0] == 'o')
            xcr0 &= ~4ull;
    }
#endif
    return avx2_decide(leaf7Ok, l1ecx, l7ebx, xcr0);
}

const char* avx2_message(Avx2Status s) {
    switch (s) {
    case Avx2Status::NoCpu:
        return "This program needs a processor with AVX2 (Intel Haswell / 4th generation Core, AMD Excavator / Ryzen "
               "or newer), and this one does not report it. In a virtual machine the host may be hiding AVX2 from "
               "the guest.";
    case Avx2Status::NoOs:
        return "The processor supports AVX2, but the operating system has not enabled the AVX register state, so the "
               "program cannot use it. Update the operating system (Windows 7 SP1 or newer is needed for AVX); in a "
               "virtual machine, check that AVX is passed through to the guest.";
    case Avx2Status::Ok:
        break;
    }
    return nullptr;
}

} // namespace ddr5
