#pragma once
// sizing.h - how much memory a run asks for, and what to do when the OS refuses it. Pure logic (no OS calls), so
// tools/unit_tests.cpp can test it; suite.cpp (CLI and GUI run) and main_gui.cpp (the "Megabytes" default) use it.
//
//  1. plan():            requested size (0 = automatic) -> size to allocate. A fixed floor is always left for the OS
//  and
//                        the GUI, and the size never exceeds what the process can still commit (Windows: RAM + page
//                        file).
//  2. alloc_with_retry(): the allocation can still fail (another program took the memory in the meantime, the commit
//  limit
//                        moved, address-space limits). It is then tried again at smaller sizes, with a log line each
//                        time, instead of ending the run with "could not run".
#include <algorithm>
#include <atomic>
#include "types.h"

namespace ddr5 {
namespace sizing {

inline constexpr u64 kMB = 1ull << 20;
inline constexpr u64 kPiece = 2 * kMB; // every thread tests a whole number of these (large-page size)

inline constexpr u64 kAutoPercent = 90;      // automatic size: this share of the free memory ...
inline constexpr u64 kCapPercent = 97;       // ... a requested size is capped at this share
inline constexpr u64 kAutoFloor = 512 * kMB; // ... and the automatic size always leaves at least this much free
inline constexpr u64 kCapFloor = 256 * kMB; // a requested size leaves at least this much too: platform_win.cpp asks for
                                            // a working set of size + 256 MB, which must fit in the free memory
inline constexpr u64 kCommitMargin = 256 * kMB; // left over from the commit limit for everything else on the machine

inline constexpr u64 kRetryStepPercent = 10; // after a refused allocation: 90%, 80%, ... of the first size
inline constexpr u64 kRetryMinPercent = 20;  // ... but never below this share of it

enum class Limit { None, Free, Commit }; // what reduced the requested size, if anything
struct Plan {
    u64 bytes = 0;
    Limit limited = Limit::None;
};

inline u64 minus_floor(u64 v, u64 floor) {
    return v > floor ? v - floor : 0;
}

// Size to test when none was requested: 90% of the free memory, but at least kAutoFloor stays free.
inline u64 auto_bytes(u64 avail) {
    return std::min(avail / 100 * kAutoPercent, minus_floor(avail, kAutoFloor));
}

// requested = bytes asked for (0 = automatic). avail = free physical memory, availCommit = what the process can still
// commit; 0 means unknown for both (that limit is then not applied; automatic sizing needs avail).
inline Plan plan(u64 requested, u64 avail, u64 availCommit) {
    Plan p;
    p.bytes = requested ? requested : auto_bytes(avail);
    if (avail) {
        const u64 cap = std::min(avail / 100 * kCapPercent, minus_floor(avail, kCapFloor));
        if (p.bytes > cap) {
            p.bytes = cap;
            p.limited = Limit::Free;
        }
    }
    if (availCommit) {
        const u64 cap = minus_floor(availCommit, kCommitMargin);
        if (p.bytes > cap) {
            p.bytes = cap;
            p.limited = Limit::Commit;
        }
    }
    return p;
}

// Bytes per thread for `bytes` split over `threads`: whole pieces, so no thread has an unused remainder.
inline u64 per_thread(u64 bytes, int threads) {
    return bytes / (u64)std::max(1, threads) / kPiece * kPiece;
}

// Size (bytes per thread) of retry number `step` (1, 2, ...) after `first` was refused; 0 = no further retry.
inline u64 retry_per(u64 first, int step) {
    const u64 pct = 100 - kRetryStepPercent * (u64)step;
    if (step < 1 || pct < kRetryMinPercent || pct >= 100)
        return 0;
    return first / 100 * pct / kPiece * kPiece;
}

struct AllocOutcome {
    void* p = nullptr; // nullptr = every size was refused
    u64 per = 0;       // bytes per thread of the last size tried (the one that worked when p is set)
    int attempts = 0;
};

// alloc(bytes) -> void* is tried with per * threads bytes, then with the sizes retry_per() gives, until one works. A
// retry that would round to the size just tried is skipped. log(failedBytes, nextBytes) is called before each retry. A
// raised *stop (may be null) ends the retrying: the user wants out, not a smaller test.
template <class AllocFn, class LogFn>
AllocOutcome alloc_with_retry(u64 per, int threads, const std::atomic<bool>* stop, AllocFn alloc, LogFn log) {
    const u64 t = (u64)std::max(1, threads), first = per;
    AllocOutcome o;
    int step = 0;
    for (;;) {
        o.attempts++;
        o.per = per;
        o.p = per ? alloc(per * t) : nullptr;
        if (o.p || (stop && stop->load(std::memory_order_relaxed)))
            return o;
        u64 next = 0;
        while (!next) {
            const u64 n = retry_per(first, ++step);
            if (!n)
                return o; // nothing smaller is worth running
            if (n < per)
                next = n;
        }
        log(per * t, next * t);
        per = next;
    }
}

} // namespace sizing
} // namespace ddr5
