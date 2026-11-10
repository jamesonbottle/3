// HANDOFF [TODO] algo_probe.cpp - row-hit vs row-conflict latency by address distance (informational, not pass/fail).
// Source:  engine.h 1055-1106 (algo_probe; comment at 1050-1054).
// Needs:   lineio.h, gen.h, order.h, run.h, algo.h, seed.h, platform.h, log.h. Parameter meanings: suite.cfg header.
// Uses __rdtsc and _mm_lfence: <immintrin.h> on GCC/clang, <intrin.h> on MSVC (types.h already includes both as needed). No built-in test calls it.
// Delete this block when done.
