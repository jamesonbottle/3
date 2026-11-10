// HANDOFF [TODO] algo_fill.cpp - write -> [disturb] -> [dwell] -> verify xN.
// Source:  engine.h 692-737 (algo_fill). Moved verbatim; the function becomes non-static.
// Needs:   lineio.h, gen.h, order.h, run.h, algo.h, seed.h, platform.h, log.h. Parameter meanings: suite.cfg header.
// Contains the first DDR5TEST_INJECT block (721, right after the first R.sync()): it plants the 42 lines / 44 bits tools/selftest.sh counts.
// Keep it inside #ifdef. The inject build must compile this file (and the other three INJECT files) with -DDDR5TEST_INJECT; see CMakeLists.txt step 5.
// Delete this block when done.
