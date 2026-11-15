#pragma once
// cpu_check.h - may the AVX2 code of this program run on this machine? (AUDIT_PLAN.md E3)
//
// The whole build is compiled for AVX2 (-mavx2 / /arch:AVX2), so on a CPU without it the first AVX2 instruction ends
// the program with an illegal-instruction crash and no message. The front-ends call avx2_status() as their very first
// statement and stop with avx2_message() instead.
//
// cpu_check.cpp is the one source of the project that is built WITHOUT AVX2 (its own static library in CMakeLists.txt),
// because the compiler is free to use AVX2 in any translation unit that has it enabled, this check's own code included.
// This header declares only plain types for the same reason: nothing here may pull in AVX2 code.
namespace ddr5 {

enum class Avx2Status {
    Ok,    // the CPU has AVX2 and the OS saves the YMM registers: AVX2 code can run
    NoCpu, // the CPU does not report AVX2 (or AVX)
    NoOs   // the CPU has AVX2 but the OS has not enabled the YMM state (XSAVE / XCR0), so AVX2 code would still fault
};

// The decision on its own, from the raw CPU data, so tools/unit_tests.cpp can test it without special hardware.
//   leaf7Ok = CPUID's highest leaf is >= 7 (otherwise l7ebx is not valid)
//   l1ecx   = CPUID.(EAX=1):ECX      bit 27 OSXSAVE, bit 28 AVX
//   l7ebx   = CPUID.(EAX=7,ECX=0):EBX  bit 5 AVX2
//   xcr0    = XGETBV(0), read only when OSXSAVE is set: bit 1 SSE state, bit 2 YMM state
Avx2Status avx2_decide(bool leaf7Ok, unsigned l1ecx, unsigned l7ebx, unsigned long long xcr0);

// Reads this CPU and decides. In the fault-injection build (DDR5TEST_INJECT) the environment variable
// DDR5TEST_FAKE_AVX2 = cpu | os makes the raw data look like a CPU without AVX2 / an OS without YMM state.
Avx2Status avx2_status();

// Text for the user (plain ASCII, no trailing newline). nullptr for Ok.
const char* avx2_message(Avx2Status s);

} // namespace ddr5
