// platform_win.cpp - Windows implementation of platform.h.
#ifdef _WIN32
#if defined(_MSC_VER)
#pragma comment(lib, "advapi32.lib")
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <thread>
#include "platform.h"

namespace ddr5 {

namespace plat {
static std::atomic<bool>* g_stop = nullptr;

static BOOL WINAPI ctrl(DWORD) {
    if (g_stop)
        g_stop->store(true);
    return TRUE;
}
void install_ctrlc(std::atomic<bool>* s) {
    g_stop = s;
    SetConsoleCtrlHandler(ctrl, TRUE);
}
int ncpu() {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (int)n : (int)std::thread::hardware_concurrency();
}
// Pin thread number `cpu` to logical CPU `cpu`, counting across processor groups (Windows splits machines with >64
// logical CPUs into groups).
void pin(int cpu) {
    WORD ng = GetActiveProcessorGroupCount();
    if (ng <= 1) {
        if (cpu < 64)
            SetThreadAffinityMask(GetCurrentThread(), 1ull << cpu);
        return;
    }
    int idx = cpu;
    for (WORD g = 0; g < ng; g++) {
        int n = (int)GetActiveProcessorCount(g);
        if (idx < n) {
            GROUP_AFFINITY ga{};
            ga.Group = g;
            ga.Mask = (KAFFINITY)1 << idx;
            SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr);
            return;
        }
        idx -= n;
    }
}
u64 avail_phys() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    GlobalMemoryStatusEx(&m);
    return m.ullAvailPhys;
}
u64 total_phys() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    GlobalMemoryStatusEx(&m);
    return m.ullTotalPhys;
}
void lower_priority() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}
static bool lock_priv() {
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t))
        return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueA(nullptr, "SeLockMemoryPrivilege", &tp.Privileges[0].Luid)) {
        CloseHandle(t);
        return false;
    }
    AdjustTokenPrivileges(t, FALSE, &tp, 0, nullptr, nullptr);
    bool ok = GetLastError() == ERROR_SUCCESS;
    CloseHandle(t);
    return ok;
}
// got = large pages obtained; locked = the memory is really pinned in RAM (large pages always are; otherwise
// VirtualLock/mlock must have succeeded)
void* alloc(u64 bytes, bool large, bool& got, bool& locked) {
    got = false;
    locked = false;
    if (large && lock_priv()) {
        SIZE_T lp = GetLargePageMinimum();
        if (lp) {
            u64 b = (bytes + lp - 1) / lp * lp;
            void* p = VirtualAlloc(nullptr, b, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
            if (p) {
                got = true;
                locked = true;
                return p;
            }
        }
    }
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), bytes + (256ull << 20), bytes + (512ull << 20),
                               QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE);
    void* p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p)
        locked = VirtualLock(p, bytes) != 0;
    return p;
}
void release(void* p, u64) {
    VirtualFree(p, 0, MEM_RELEASE);
}
} // namespace plat

} // namespace ddr5
#endif // _WIN32
