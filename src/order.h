#pragma once
// HANDOFF [TODO] order.h - address orders (seq/stride/random/bitrev/gray/butterfly).
// Source:  engine.h 243-244 (section comment) and 254-295 (struct Order).
// Keep:    at(), fe() and pos() inline in this header (called once per line in every walk). parse() and init() may move to order.cpp.
// Needs:   types.h (splitmix, clz64, rev64), seed.h (g_salt), params.h (lower).
// Do not change the permutation maths: every --seed reproduction depends on it. Covered by unit_tests test_orders.
// Delete this block when done.
