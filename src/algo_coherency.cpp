// HANDOFF [TODO] algo_coherency.cpp - stressapptest-style cache-coherency check (tests cores/fabric, not DRAM cells).
// Source:  engine.h 1201-1238 (algo_coherency; comment at 1196-1200).
// Needs:   lineio.h, gen.h, order.h, run.h, algo.h, seed.h, platform.h, log.h. Parameter meanings: suite.cfg header.
// Contains the fifth (last) DDR5TEST_INJECT block (1217): one extra increment via a function-local `static bool once`. selftest expects exit 1 and `coherency` in the log.
// Delete this block when done.
