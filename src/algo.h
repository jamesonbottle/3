#pragma once
// HANDOFF [TODO] algo.h - what the ten algo_*.cpp files share, plus the registry.
// Source:  engine.h 1240 (AlgoFn), 661-691 (OrderSpec, ospec, stride_list, disturb_traffic, parse_dmode, disturb_possible), 993-997 (evo_period, evo_make).
// Declare: AlgoFn; the ten `void algo_X(Run&, const Params&)` (fill march hammer copy chase rwstorm evolve probe pagemix coherency);
//          `extern const std::map<std::string, AlgoFn> ALGOS;` (name kept: suite and unit_tests use it); the helpers above;
//          `std::vector<MVar> parse_march_variants(const std::string&)` + MVar/MEl/MOp (unit_tests calls it; defined in algo_march.cpp).
// Keep evo_period and evo_make `inline` here (unit_tests test_evolve uses them). g_sink stays file-local in algo.cpp.
// Needs:   types.h, params.h, order.h, run.h, seed.h.
// Delete this block when done.
