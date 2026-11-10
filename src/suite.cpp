// HANDOFF [TODO] suite.cpp - built-in suite loading, run_suite, failure summary, FPU stress.
// Source:  engine.h 1245 (#include "suite_data.h"), 1247 (suite_text), 1265-1274 (emit_skew), 1276-1286 (g_fpu_sink, fpu_stress), 1288-1372 (run_suite).
// Include suite_data.h in THIS file only (it defines static arrays; a second include would duplicate the 16 KB of text).
// emit_skew and fpu_stress stay file-local. Needs: everything (params, seed, run, algo, platform, log, lineio for fpu intrinsics).
// Preserve every emitted string: selftest greps `run seed: 0x... (random|given)`, `coverage this test 3.0x`, `42 bad lines, 44 bits`.
// run_suite sets g_rng from Settings and g_salt = salt_for(runSeed, loop) per loop; keep that order.
// Delete this block when done.
