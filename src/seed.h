#pragma once
// seed.h - run seed, salt and the selectable hash mixer.
#include "types.h"

namespace ddr5 {

// Every random choice in the engine (order permutations, hash/addr seeds, disturb traffic, page shuffle, chase layout,
// coherency) is a pure function of Run::salt, and the salt is a pure function of the run seed and the loop number. So
// one printed number reproduces a whole run (given the same memory size and thread count, which decide the data layout;
// the timed tests also depend on thread timing).
u64 salt_for(u64 runSeed, u64 loop); // differs per loop: a long soak never repeats itself
u64 fresh_seed();                    // random_device alone can be deterministic on old MinGW, so the clock is mixed in

// Selectable mixer for 'hash' data (GUI: Advanced > RNG). Orders keep splitmix so permutations stay stable.
// rng: 0 splitmix64, 1 murmur3 fmix64, 2 xorshift64* (Run::rng, copied into Gen::rng by build_vars).
inline u64 mix64(u64 x, int rng) {
    x += 0x9E3779B97F4A7C15ull;
    switch (rng) {
    case 1:
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ull;
        x ^= x >> 33;
        return x;
    // xorshift64* needs a real finalizer: its single multiply only moves information UP, so the low output bits ignored
    // most of the input (308 of 4096 input-bit -> output-bit pairs never flipped; tools/unit_tests.cpp measures this).
    // Fold the high half back down, multiply again and fold once more, as splitmix/murmur do.
    case 2:
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        x *= 0x2545F4914F6CDD1Dull;
        x ^= x >> 29;
        x *= 0xBF58476D1CE4E5B9ull;
        return x ^ (x >> 32);
    default:
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }
}

} // namespace ddr5
