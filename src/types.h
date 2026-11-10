#pragma once
// HANDOFF [TODO] types.h - tiny base included by everything.
// Source:  engine.h 49-55 (<intrin.h>, CBAR()), 57 (u8..u64), 59-60 (Line + static_assert), 62 (splitmix), 68 (pc32), 75 (clz64), 82 (rev64).
// Keep:    `static inline` -> `inline`. Includes only <cstdint>, <immintrin.h>, and <intrin.h> under MSVC. No <windows.h>.
// Not here: Beats (gen.h), mix64 (seed.h), platform code (platform.h).
// Delete this block when done.
