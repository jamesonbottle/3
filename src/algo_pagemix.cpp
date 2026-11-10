// HANDOFF [TODO] algo_pagemix.cpp - stressapptest-style page shuffle.
// Source:  engine.h 1113 (PagePat), 1114-1142 (pm_fill, pm_check, pm_copy, pm_invert), 1143-1195 (algo_pagemix; comment at 1107-1112).
// Needs:   lineio.h, gen.h, order.h, run.h, algo.h, seed.h, platform.h, log.h. Parameter meanings: suite.cfg header.
// PagePat and the pm_* templates are only used here: file-local.
// Contains two DDR5TEST_INJECT blocks: 1159-1160 (DDR5TEST_STOP_IN_FILL env var stops the run mid-fill) and 1182 (planted flip caught by `pagemix-final`).
// selftest has a dedicated case for each; do not change when `filled[pg]` is set.
// Delete this block when done.
