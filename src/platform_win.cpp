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

static std::atomic<int> g_hits{0};

// First Ctrl-C: ask the run to stop. A second one ends the process at once (a wedged run could not be stopped
// otherwise); 130 = the usual exit status after SIGINT.
static BOOL WINAPI ctrl(DWORD) {
    if (g_hits.fetch_add(1) >= 1)
        ExitProcess(130);
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
// logical CPUs into groups). false = not pinned: the call failed, or there is no such CPU (more threads than CPUs).
bool pin(int cpu) {
    if (cpu < 0)
        return false;
    WORD ng = GetActiveProcessorGroupCount();
    if (ng <= 1) {
        if (cpu >= 64) // a single group holds at most 64 CPUs
            return false;
        return SetThreadAffinityMask(GetCurrentThread(), 1ull << cpu) != 0;
    }
    int idx = cpu;
    for (WORD g = 0; g < ng; g++) {
        int n = (int)GetActiveProcessorCount(g);
        if (idx < n) {
            GROUP_AFFINITY ga{};
            ga.Group = g;
            ga.Mask = (KAFFINITY)1 << idx;
            return SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr) != 0;
        }
        idx -= n;
    }
    return false;
}
// false = the call failed; the caller must then report "unknown", not "0 bytes free"
static bool mem_status(MEMORYSTATUSEX& m) {
    m = MEMORYSTATUSEX{};
    m.dwLength = sizeof(m);
    return GlobalMemoryStatusEx(&m) != 0;
}
u64 avail_phys() {
    MEMORYSTATUSEX m;
    return mem_status(m) ? m.ullAvailPhys : 0;
}
u64 total_phys() {
    MEMORYSTATUSEX m;
    return mem_status(m) ? m.ullTotalPhys : 0;
}
KeepAwake keep_awake(bool on) {
    // ES_CONTINUOUS keeps the state until it is changed again (or the thread ends); without ES_SYSTEM_REQUIRED the
    // clearing call drops the request. Returns the previous state, or NULL when the call failed.
    const EXECUTION_STATE r = SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
    return !on ? KeepAwake::NotSupported : (r ? KeepAwake::On : KeepAwake::Failed);
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
void* alloc(u64 bytes, bool large, bool& got, bool& locked, std::string& note, const std::atomic<bool>* stop) {
    got = false;
    locked = false;
    // The large-page allocation below is ONE OS call (it commits and zeroes everything), so Stop cannot cut it short;
    // only the locking of ordinary pages further down is done in slices.
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
    // without room in the working set VirtualLock fails below, so say why when this does
    if (!SetProcessWorkingSetSizeEx(GetCurrentProcess(), bytes + (256ull << 20), bytes + (512ull << 20),
                                    QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE))
        note = "SetProcessWorkingSetSizeEx failed (error " + std::to_string(GetLastError()) + ")";
    void* p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p) {
        // VirtualLock is the first touch of every page; one call for tens of GB cannot be interrupted, so lock in
        // slices
        DWORD err = 0;
        const LockProgress lp = lock_in_slices((u8*)p, bytes, stop, kLockSlice, [&](u8* q, u64 n) {
            if (VirtualLock(q, (SIZE_T)n))
                return true;
            err = GetLastError();
            return false;
        });
        locked = !lp.failed && !lp.stopped;
        const std::string sep = note.empty() ? "" : "; ";
        if (lp.failed)
            note += sep + "VirtualLock failed" +
                    (lp.done ? " after " + std::to_string(lp.done >> 20) + " of " + std::to_string(bytes >> 20) + " MB"
                             : std::string()) +
                    " (error " + std::to_string(err) + ")";
        else if (lp.stopped)
            note += sep + "stopped while locking the memory (" + std::to_string(lp.done >> 20) + " of " +
                    std::to_string(bytes >> 20) + " MB locked)";
    }
    return p;
}
void release(void* p, u64) {
    VirtualFree(p, 0, MEM_RELEASE);
}
} // namespace plat

} // namespace ddr5
#endif // _WIN32
