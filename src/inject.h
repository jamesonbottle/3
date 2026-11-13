// inject.h - fault-injection hooks for tools/selftest.sh.
// The algorithms call DDR5_INJECT(hook, args...) at a few points between write and verify.
// - Normal build: it expands to nothing (no code, no branch).
// - Fault-injection engine (-DDDR5TEST_INJECT, library ddr5engine_inject): it calls ddr5::inject::<hook>. The bodies
//   live in inject.cpp and plant known faults that the self-test must find.
// Every thread reaches every hook; the bodies pick the thread that acts.
#pragma once

#ifdef DDR5TEST_INJECT
#include "order.h"
#include "run.h"
#include "types.h"
#include <cstddef>

namespace ddr5::inject {

using StoreFn = void (*)(void*, const Line&); // IO<...>::store of the calling test

void after_fill(Run& R, int tid, size_t ji, const View& w);
void march_after_element(int tid, size_t ji, size_t ei, const View& w);
void chase_after_fill(int tid, u64 sd, u64 K, const View& w, const Order& o, u64 steps);
void copy_after_copy(int tid, size_t ji, u8* A, u8* B, u64 nA);
void copy_fill_stop(Run& R, int tid, u64 i);
void rwstorm_before_final(int tid, size_t ji, const View& w);
void rwstorm_read_fault(int tid, size_t ji, u64 i, Line& e);
void evolve_last_pass(Run& R, int tid, size_t ji, const View& w, u64 p, u64 passes, u64 seed, u64 maxp, StoreFn store);
void evolve_stop_in_write(Run& R, int tid, u64 p);
void after_fill_throw(int tid);
void pagemix_fill_stop(Run& R, int tid, u64 pg);
void pagemix_flip(Run& R, int tid);
void pagemix_final_stop(Run& R, int tid, u64 pg);
void coherency_extra(u8* mem, int tid);

} // namespace ddr5::inject

#define DDR5_INJECT(hook, ...) ::ddr5::inject::hook(__VA_ARGS__)
#else
#define DDR5_INJECT(hook, ...) ((void)0)
#endif
