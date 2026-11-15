#pragma once
// coverage.h - which pages of the tested region have really been verified (real addresses, not a byte counter).
//
// One bit per kPageBytes (4 KiB) page of the region, counted from the region's first byte, so 64 GiB needs 2 MiB of
// bits. A page is marked only when a verify covered ALL of its bytes: mark() sets the pages that lie completely inside
// the given range and ignores a partial page at either end. Marking twice is harmless (the bit stays set and counts
// once), which the rotating windows need because the last window of a chunk overlaps the one before it.
//
// The region tail that is not a whole page does not count, neither as a page nor in the total.
//
// Threading: reset() runs before the workers exist (it is not thread-safe). mark() and the readers may run on any
// thread at any time; they use relaxed atomics, so a reader sees a bit some time after it was set, never a wrong one.
// This file is pure logic: no algorithm, no run state.
#include <atomic>
#include <memory>
#include "types.h"

namespace ddr5 {

class CoverageMap {
  public:
    static_assert(std::atomic<u64>::is_always_lock_free, "the page bits are updated from every worker thread");

    // Sizes the map for a region of `regionBytes` and clears every bit. Not thread-safe.
    void reset(u64 regionBytes) {
        total_ = regionBytes / kPageBytes;
        nwords_ = (total_ + 63) / 64;
        words_.reset(nwords_ ? new std::atomic<u64>[nwords_]() : nullptr); // () = every word starts at 0
    }

    // Marks every page that lies completely inside [offset, offset + bytes) (offsets from the region start). A partial
    // page at the start or the end is not marked, a range that reaches past the region is cut at its end, an empty
    // range or one that starts past the end does nothing. Never writes outside the map.
    void mark(u64 offset, u64 bytes) {
        if (!bytes || !total_)
            return;
        u64 endByte = offset + bytes;
        if (endByte < offset) // the sum wrapped around: treat as "to the end of the address space"
            endByte = ~(u64)0;
        u64 first = offset / kPageBytes + (offset % kPageBytes ? 1 : 0); // first page that starts inside the range
        u64 end = endByte / kPageBytes;                                  // one past the last page that ends inside it
        if (end > total_)
            end = total_;
        if (first >= end)
            return;
        for (u64 w = first / 64; w <= (end - 1) / 64; w++) {
            u64 lo = w * 64 > first ? 0 : first - w * 64;   // first bit of this word in the range
            u64 hi = w * 64 + 64 < end ? 64 : end - w * 64; // one past the last bit
            u64 m = (hi - lo == 64) ? ~(u64)0 : (((u64)1 << (hi - lo)) - 1) << lo;
            if ((words_[w].load(std::memory_order_relaxed) & m) != m) // skip the write when nothing changes
                words_[w].fetch_or(m, std::memory_order_relaxed);
        }
    }

    u64 pages_total() const { return total_; }
    u64 pages_covered() const {
        u64 n = 0;
        for (u64 w = 0; w < nwords_; w++) {
            u64 v = words_[w].load(std::memory_order_relaxed);
            n += (u64)pc32((u32)v) + (u64)pc32((u32)(v >> 32));
        }
        return n;
    }
    // Percent of the pages that have been marked, 0 when the map is empty.
    double percent() const { return total_ ? 100.0 * (double)pages_covered() / (double)total_ : 0.0; }
    bool marked(u64 page) const {
        return page < total_ && (words_[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1;
    }
    // Index of the first unmarked page at or after `from`; pages_total() when there is none.
    u64 first_unmarked(u64 from = 0) const {
        for (u64 p = from; p < total_;) {
            u64 v = words_[p / 64].load(std::memory_order_relaxed) >> (p % 64);
            if (!(v & 1))
                return p;
            // skip the run of set bits that starts here (a full word at a time when aligned)
            p += (p % 64 == 0 && v == ~(u64)0) ? 64 : 1;
        }
        return total_;
    }

  private:
    u64 total_ = 0;  // pages in the region (whole pages only)
    u64 nwords_ = 0; // 64-bit words holding the bits
    std::unique_ptr<std::atomic<u64>[]> words_;
};

} // namespace ddr5
