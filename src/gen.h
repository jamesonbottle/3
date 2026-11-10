#pragma once
// HANDOFF [TODO] gen.h - data generators: stressapptest-style pattern table, Gen, Var, build_vars.
// Source:  engine.h 298 (Beats), 316-343 (SatPat, sat_table, sat_word), 345-362 (Gen), 363 (Var); declare build_vars (415).
// Keep:    Gen::make, Gen::set_beats and sat_word inline in this header (per-line hot path).
// Declare: `const std::vector<SatPat>& sat_table();` and `std::vector<Var> build_vars(const Params&);`. expand_style, beats_from_lanes and rotl16 stay in gen.cpp.
// Needs:   types.h, seed.h (mix64, g_salt), params.h.
// Delete this block when done.
