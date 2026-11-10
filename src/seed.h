#pragma once
// HANDOFF [TODO] seed.h - run seed, salt and the selectable hash mixer.
// Source:  engine.h 245-253 (g_salt + the comment above it at 245-247, salt_for, fresh_seed) and 299-315 (g_rng, mix64 with the xorshift64* finalizer comment).
// Expose:  extern u64 g_salt; extern int g_rng; u64 salt_for(u64, u64); u64 fresh_seed();
//          `inline u64 mix64(u64)` stays in this header (hot path of Gen::make and evo_make) and reads the extern g_rng.
// Needs:   types.h. Keep the xorshift64* comment: tools/unit_tests.cpp measures exactly what it describes.
// Delete this block when done.
