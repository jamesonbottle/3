#pragma once
// HANDOFF [TODO] run.h - Chunk, View, Stats, Run, walk().
// Source:  engine.h 513-576 (Chunk, View, Stats, t_vcount, Run), 629-652 (Run::parallel body), 654-659 (walk).
// Declare Run::report here, define it in run.cpp. Run::parallel is a template: its body stays in this header. walk() likewise.
// t_vcount must become `inline thread_local u64 t_vcount` (a `static thread_local` in a header gives every TU its own counter and loses counts).
// Run must stay default-constructible with public members: tools/unit_tests.cpp does `Run R; R.T = 1; R.chunks.push_back(...)`.
// Keep inline in the class: view, dview, window_lines, sleep_ms, vtick, vflush, sync, set_label, tick (unit_tests calls view/dview).
// Needs:   types.h, params.h, order.h, platform.h, log.h, <barrier> <atomic> <mutex> <map> <thread>.
// Delete this block when done.
