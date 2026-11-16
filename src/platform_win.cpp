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
#include <vector>
#include "cpu_set.h"
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
// The CPUs this process may run on, read once (the first call comes before any worker exists; thread affinity does not
// change what the process may use anyway):
//  - the processor groups the process belongs to (GetProcessGroupAffinity);
//  - a process in ONE group: its affinity mask (`start /affinity`, SetProcessAffinityMask, a job's CPU limit that was
//    applied to it); a process spanning several groups has no mask to read, so all active CPUs of those groups count;
//  - a job object with a group / CPU list (JobObjectGroupInformationEx) confines that further. Best effort: a job that
//    reports nothing, or an answer that would leave no CPU at all, changes nothing.
// A CPU rate cap (a job's "x % of the CPU") is NOT a CPU count and is not looked at.
// If the OS calls give nothing usable the set is every active CPU of the machine, which is what ncpu() used to return.
// What the job object the process runs in allows (JobObjectGroupInformationEx = 14: an array of GROUP_AFFINITY). false
// = not in a job, no such limit, or the query failed.
static bool job_group_limit(std::vector<cpuset::GroupMask>& out) {
    GROUP_AFFINITY ga[64];
    DWORD len = 0;
    if (!QueryInformationJobObject(nullptr, (JOBOBJECTINFOCLASS)14, ga, sizeof ga, &len) ||
        len < sizeof(GROUP_AFFINITY))
        return false;
    for (DWORD i = 0; i < len / sizeof(GROUP_AFFINITY); i++)
        out.push_back(cpuset::GroupMask{(unsigned)ga[i].Group, (u64)ga[i].Mask});
    return true;
}
static const std::vector<cpuset::CpuRef>& allowed_cpus() {
    static const std::vector<cpuset::CpuRef> cpus = [] {
        std::vector<cpuset::GroupMask> groups;
        USHORT arr[64];
        USHORT cnt = 64;
        if (GetProcessGroupAffinity(GetCurrentProcess(), &cnt, arr) && cnt > 0) {
            DWORD_PTR pm = 0, sm = 0;
            const bool haveMask = cnt == 1 && GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) != 0;
            for (USHORT i = 0; i < cnt; i++) {
                const u64 active = cpuset::low_bits(GetActiveProcessorCount(arr[i]));
                groups.push_back(cpuset::GroupMask{(unsigned)arr[i], haveMask ? ((u64)pm & active) : active});
            }
        }
        std::vector<cpuset::GroupMask> limit;
        if (!groups.empty() && job_group_limit(limit)) {
            std::vector<cpuset::GroupMask> r = cpuset::restrict_to(groups, limit);
            if (!r.empty()) // an answer with no CPU in it is not believed
                groups = r;
        }
        std::vector<cpuset::CpuRef> v = cpuset::expand(groups);
        if (v.empty()) { // the queries gave nothing usable: every active CPU of the machine
            groups.clear();
            const WORD ng = GetActiveProcessorGroupCount();
            for (WORD g = 0; g < ng; g++)
                groups.push_back(cpuset::GroupMask{(unsigned)g, cpuset::low_bits(GetActiveProcessorCount(g))});
            v = cpuset::expand(groups);
        }
        return v;
    }();
    return cpus;
}
int ncpu() {
    const size_t n = allowed_cpus().size();
    return n ? (int)n : (int)std::thread::hardware_concurrency();
}
// Pin the calling thread to the cpu-th allowed CPU, counting across processor groups and wrapping round when there are
// more threads than CPUs (the same rule as on Linux). false = not pinned: the call failed, or there is no CPU list.
bool pin(int cpu) {
    const cpuset::CpuRef* c = cpuset::nth(allowed_cpus(), cpu);
    if (!c)
        return false;
    GROUP_AFFINITY ga{};
    ga.Group = (WORD)c->group;
    ga.Mask = (KAFFINITY)1 << c->bit;
    return SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr) != 0;
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
u64 avail_commit() {
    // ullAvailPageFile: "the maximum amount of memory the current process can commit" (it is called page file, but it
    // is the commit limit minus what is committed: RAM + page file)
    MEMORYSTATUSEX m;
    return mem_status(m) ? m.ullAvailPageFile : 0;
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
