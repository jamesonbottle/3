// HANDOFF [TODO] algo_evolve.cpp - data that changes from pass to pass (lost-write detector).
// Source:  engine.h 998-1049 (algo_evolve; keep the long comment at 985-992). evo_period/evo_make (993-997) live in algo.h as inline.
// Needs:   lineio.h, gen.h, order.h, run.h, algo.h, seed.h, platform.h, log.h. Parameter meanings: suite.cfg header.
// Contains the second DDR5TEST_INJECT block (1024, last pass): one lost write + one stray bit flip. selftest expects exactly 2 `evolve-verify` lines.
// Delete this block when done.
