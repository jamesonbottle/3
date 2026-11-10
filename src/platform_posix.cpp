// platform_posix.cpp - Linux/POSIX implementation of platform.h.
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // CPU_SET / pthread_setaffinity_np
#endif
#include <csignal>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#include "platform.h"

namespace plat {
static std::atomic<bool>* g_stop = nullptr;

static void ctrl(int) { if (g_stop) g_stop->store(true); }
void install_ctrlc(std::atomic<bool>* s) { g_stop = s; std::signal(SIGINT, ctrl); }
int ncpu() { return (int)std::thread::hardware_concurrency(); }
void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu % CPU_SETSIZE, &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
}
u64 avail_phys() { return (u64)sysconf(_SC_AVPHYS_PAGES) * (u64)sysconf(_SC_PAGESIZE); }
u64 total_phys() { return (u64)sysconf(_SC_PHYS_PAGES) * (u64)sysconf(_SC_PAGESIZE); }
void lower_priority() {}
void* alloc(u64 bytes, bool, bool& got, bool& locked) {
    got = false; locked = false;
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    madvise(p, bytes, MADV_HUGEPAGE);
    locked = mlock(p, bytes) == 0;
    return p;
}
void release(void* p, u64 b) { munmap(p, b); }
}  // namespace plat
#endif  // !_WIN32
