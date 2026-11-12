#pragma once
// platform.h - OS abstraction (declarations only). Implemented in platform_win.cpp / platform_posix.cpp.
#include <atomic>
#include <chrono>
#include <string>
#include "types.h"

namespace ddr5 {

namespace plat {
void install_ctrlc(std::atomic<bool>* s); // Ctrl-C / SIGINT sets *s
// Logical CPUs this process may run on: on Linux the taskset/cpuset-restricted set, not the machine's CPU count.
int ncpu();
// Pin the calling thread to the cpu-th usable CPU (Linux: the cpu-th of the allowed set, wrapping round when there
// are fewer CPUs than threads; Windows: logical CPU number `cpu` across processor groups). false = the thread was not
// pinned (the call failed, or Windows has no such CPU): the run is still valid, the thread just may migrate.
bool pin(int cpu);
// Free and installed physical memory in bytes. 0 = unknown (the OS call failed): callers must not read it as "no
// memory". Linux reports MemAvailable (free memory plus what the OS can reclaim from caches), not just the free pages.
u64 avail_phys();
u64 total_phys();
void lower_priority();
// got = large pages obtained; locked = the memory is really pinned in RAM (large pages always are; otherwise
// VirtualLock/mlock must have succeeded). `note` gets a line of explanation when something was not as asked but the
// allocation still worked (for example why the memory could not be locked); it is left alone otherwise.
void* alloc(u64 bytes, bool large, bool& got, bool& locked, std::string& note);
void release(void* p, u64 bytes);
inline double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
} // namespace plat

} // namespace ddr5
