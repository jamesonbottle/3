#pragma once
// platform.h - OS abstraction (declarations only). Implemented in platform_win.cpp / platform_posix.cpp.
#include <algorithm>
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
// Locking is the first touch of every page, so for tens of GB it takes many seconds. It is done in slices of kLockSlice
// bytes, and a Stop request is looked at between two of them.
inline constexpr u64 kLockSlice = 64ull << 20;
struct LockProgress {
    u64 done = 0;         // bytes locked: whole slices, only the last one may be shorter
    bool failed = false;  // lock() reported failure for the slice at `done`; the slices before it stay locked
    bool stopped = false; // *stop was raised: the slices from `done` on were not attempted
};
// Calls lock(p + off, n) -> bool for consecutive slices of at most `slice` bytes (0 = one slice) covering [p, p +
// bytes), until all are done, one fails or *stop (may be null) is true. `stop` is read before every slice, so it is
// read once more than lock() runs. This is the slicing logic of alloc() below, separate from the OS call so
// tools/unit_tests.cpp can test it.
template <class LockFn>
LockProgress lock_in_slices(u8* p, u64 bytes, const std::atomic<bool>* stop, u64 slice, LockFn lock) {
    LockProgress r;
    if (!slice)
        slice = bytes;
    while (r.done < bytes) {
        if (stop && stop->load(std::memory_order_relaxed)) {
            r.stopped = true;
            break;
        }
        const u64 n = std::min(slice, bytes - r.done);
        if (!lock(p + r.done, n)) {
            r.failed = true;
            break;
        }
        r.done += n;
    }
    return r;
}
// got = large pages obtained; locked = the memory is really pinned in RAM (large pages always are; otherwise
// VirtualLock/mlock must have succeeded). `note` gets a line of explanation when something was not as asked but the
// allocation still worked (for example why the memory could not be locked); it is left alone otherwise.
// `stop` (may be null) makes the locking of ordinary pages interruptible: when it becomes true the lock ends early and
// the memory is still returned, with locked = false and a note that says so; the caller ends the run. Large pages are
// one OS call that cannot be interrupted.
void* alloc(u64 bytes, bool large, bool& got, bool& locked, std::string& note, const std::atomic<bool>* stop = nullptr);
void release(void* p, u64 bytes);
inline double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
} // namespace plat

} // namespace ddr5
