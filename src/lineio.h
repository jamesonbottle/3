#pragma once
// HANDOFF [TODO] lineio.h - 64-byte line store/load/verify at selectable width and cache behaviour.
// Source:  engine.h 466-512: template IO<W, NT, FL> (store/load/verify), `IC` alias, dispatch().
// Header-only by necessity (templates + inline). Needs types.h and <type_traits>. This is the only header that uses _mm256_*/_mm_stream_*.
// Keep the CBAR() fences exactly where they are: they stop the compiler from eliding or reordering the verify reads.
// Delete this block when done.
