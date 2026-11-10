// HANDOFF [TODO] algo_copy.cpp - bulk copy engines (memcpy, rep movsb, non-temporal copy).
// Source:  engine.h 872-908 (algo_copy).
// Needs:   lineio.h, gen.h, order.h, run.h, algo.h, seed.h, platform.h, log.h. Parameter meanings: suite.cfg header.
// Keep the `#if defined(_MSC_VER)` split: __movsb on MSVC, inline asm `rep movsb` elsewhere.
// Delete this block when done.
