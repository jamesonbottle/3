// seed.cpp - definitions for seed.h.
#include "seed.h"
#include <chrono>
#include <random>

u64 g_salt = 0;
int g_rng = 0;
// Differs per loop: a long soak never repeats itself.
u64 salt_for(u64 runSeed, u64 loop) {
    return splitmix(splitmix(runSeed) ^ (loop * 0xD6E8FEB86659FD93ull));
}

// random_device alone can be deterministic on old MinGW, so the clock is mixed in.
u64 fresh_seed() {
    std::random_device rd;
    u64 a = ((u64)rd() << 32) | rd();
    return splitmix(a ^ splitmix((u64)std::chrono::high_resolution_clock::now().time_since_epoch().count()));
}
