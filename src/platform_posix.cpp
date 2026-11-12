// platform_posix.cpp - Linux/POSIX implementation of platform.h.
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // CPU_SET / pthread_setaffinity_np
#endif
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include "platform.h"

namespace ddr5 {

namespace plat {
static std::atomic<bool>* g_stop = nullptr;

static void ctrl(int) { // async-signal-safe: one atomic store
    if (g_stop)
        g_stop->store(true);
}
// First Ctrl-C: ask the run to stop (it ends the current test and prints the summary). SA_RESETHAND puts the default
// action back as soon as the handler has been entered, so a second Ctrl-C kills the process even if a run is wedged.
void install_ctrlc(std::atomic<bool>* s) {
    g_stop = s;
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = ctrl;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGINT, &sa, nullptr);
}
// The CPUs this process may run on (taskset, a cpuset or a container limit shrink it), read once, in ascending order.
// std::thread::hardware_concurrency() ignores the restriction, so it is only the fallback for when the affinity
// cannot be read (for example more than CPU_SETSIZE CPUs). The first call comes before any worker pins itself, so the
// mask it reads is the one the process was started with.
static const std::vector<int>& allowed_cpus() {
    static const std::vector<int> cpus = [] {
        std::vector<int> v;
        cpu_set_t s;
        CPU_ZERO(&s);
        if (sched_getaffinity(0, sizeof s, &s) == 0)
            for (int i = 0; i < CPU_SETSIZE; i++)
                if (CPU_ISSET(i, &s))
                    v.push_back(i);
        if (v.empty())
            for (int i = 0, n = (int)std::thread::hardware_concurrency(); i < n && i < CPU_SETSIZE; i++)
                v.push_back(i);
        return v;
    }();
    return cpus;
}
int ncpu() {
    return (int)allowed_cpus().size();
}
bool pin(int cpu) {
    const std::vector<int>& a = allowed_cpus();
    if (a.empty() || cpu < 0)
        return false;
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(a[(size_t)cpu % a.size()], &s);
    return pthread_setaffinity_np(pthread_self(), sizeof(s), &s) == 0;
}
// "MemAvailable" in /proc/meminfo (kB): what a new allocation can get without swapping, because the page cache and
// other reclaimable memory count as available. 0 if the file or the line is missing.
static u64 mem_available() {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f)
        return 0;
    char line[256];
    u64 bytes = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long kb;
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
            bytes = (u64)kb * 1024;
            break;
        }
    }
    fclose(f);
    return bytes;
}
// pages * page size from sysconf; 0 (unknown) if either call fails (-1 must not become a huge unsigned number)
static u64 sysconf_bytes(int pagesName) {
    long pages = sysconf(pagesName), size = sysconf(_SC_PAGESIZE);
    return pages < 0 || size <= 0 ? 0 : (u64)pages * (u64)size;
}
u64 avail_phys() {
    u64 m = mem_available();
    return m ? m : sysconf_bytes(_SC_AVPHYS_PAGES); // old kernels have no MemAvailable
}
u64 total_phys() {
    return sysconf_bytes(_SC_PHYS_PAGES);
}
void lower_priority() {}
void* alloc(u64 bytes, bool, bool& got, bool& locked, std::string& note) {
    got = false;
    locked = false;
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return nullptr;
    madvise(p, bytes, MADV_HUGEPAGE);
    locked = mlock(p, bytes) == 0;
    if (!locked)
        note = std::string("mlock failed: ") + strerror(errno) + " (see ulimit -l / RLIMIT_MEMLOCK)";
    return p;
}
void release(void* p, u64 b) {
    munmap(p, b);
}
} // namespace plat

} // namespace ddr5
#endif // !_WIN32
