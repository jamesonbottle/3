#pragma once
// platform.h - OS abstraction (declarations only). Implemented in platform_win.cpp / platform_posix.cpp.
#include <atomic>
#include <chrono>
#include "types.h"

namespace plat {
void install_ctrlc(std::atomic<bool>* s);   // Ctrl-C / SIGINT sets *s
int ncpu();                                 // logical CPUs
void pin(int cpu);                          // pin the calling thread to logical CPU `cpu`
u64 avail_phys();
u64 total_phys();
void lower_priority();
// got = large pages obtained; locked = the memory is really pinned in RAM (large pages always are; otherwise VirtualLock/mlock must have succeeded)
void* alloc(u64 bytes, bool large, bool& got, bool& locked);
void release(void* p, u64 bytes);
inline double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace plat
