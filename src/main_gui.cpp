// DDR5 RAM Test - Windows GUI front-end. Native Win32, no runtime dependencies.
// Layout follows the familiar two-tab memory-tester window: "Testing" (status, settings, Start) and "Advanced".
// The whole test suite is compiled in (see src/suite_data.cpp); there is no config file at runtime.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "log.h"
#include "platform.h"
#include "run.h"
#include "stop_gate.h"
#include "suite.h"
#include "types.h"
#include <memory>
#include <unordered_map>

using namespace ddr5;

#if defined(_MSC_VER)
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

namespace {

enum Ids {
    ID_TAB = 100,
    ID_TOTAL,
    ID_FREE,
    ID_MB,
    ID_MBUD,
    ID_THR,
    ID_THRUD,
    ID_BEEP,
    ID_STOPERR,
    ID_DUR,
    ID_COV,
    ID_ERR,
    ID_TEST,
    ID_START,
    ID_CACHE,
    ID_RNG,
    ID_SET,
    ID_FPU
};
const UINT WM_RUN_DONE = WM_APP + 1;
const UINT_PTR TIMER_ID = 1;
const wchar_t* const APP_TITLE = L"DDR5 RAM Test";

struct App {
    HINSTANCE inst = nullptr;
    HWND wnd = nullptr, tab = nullptr;
    HFONT font = nullptr, fontBig = nullptr;
    int dpi = 96;
    std::vector<HWND> page[2];
    HWND hTotal = nullptr, hFree = nullptr, hMB = nullptr, hMBud = nullptr, hThr = nullptr, hThrUd = nullptr,
         hBeep = nullptr, hStopErr = nullptr, hDur = nullptr, hCov = nullptr, hErr = nullptr, hTest = nullptr,
         hStart = nullptr, hCache = nullptr, hRng = nullptr, hSet = nullptr, hFpu = nullptr;
    std::vector<HWND> lockWhileRunning;
    Logger logger; // declared before `run`, so it outlives it
    std::unique_ptr<Run> run;
    std::thread worker;
    std::atomic<bool> busy{false};
    double endT = 0;
    u64 lastErr = 0;
    bool stopRequested = false;
    StopGate gate; // Stop / close rules (stop_gate.h); UI thread only
    bool mbAuto = true,
         setting = true; // mbAuto: Megabytes follows free RAM and thread count until the user types a value
    std::mutex logMu;
    FILE* log = nullptr;
    std::string lastLine;
    int logWhere = 0; // 0 = next to the exe, 1 = %TEMP% (exe folder not writable), 2 = no log file could be opened
    std::atomic<bool> logFailed{false}; // a write to the log file failed (disk full...): details are missing from it
    std::unordered_map<HWND, std::wstring> shown; // last text per control, to avoid flicker
    u64 totalMB = 0;
} A;

int S(int v) {
    return MulDiv(v, A.dpi, 96);
}

HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, int page,
        DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, S(x), S(y), S(w), S(h),
                             A.wnd, (HMENU)(INT_PTR)id, A.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)A.font, TRUE);
    if (page >= 0)
        A.page[page].push_back(c);
    return c;
}
HWND group(const wchar_t* text, int x, int y, int w, int h, int page) {
    return mk(L"BUTTON", text, BS_GROUPBOX, x, y, w, h, 0, page);
}
HWND label(const wchar_t* text, int x, int y, int w, int page, DWORD style = SS_LEFT, int id = 0) {
    return mk(L"STATIC", text, style, x, y, w, 16, id, page);
}

void setText(HWND h, const std::wstring& t) {
    auto& cur = A.shown[h];
    if (cur == t)
        return;
    cur = t;
    SetWindowTextW(h, t.c_str());
}
std::wstring two(u64 v) {
    return (v < 10 ? L"0" : L"") + std::to_wstring(v);
}

std::wstring exe_dir_file(const wchar_t* name) {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, (DWORD)(sizeof p / sizeof p[0]));
    std::wstring s(p, n);
    size_t k = s.find_last_of(L"\\/");
    s = k == std::wstring::npos ? L"" : s.substr(0, k + 1);
    return s + name;
}

void show_page(int sel) {
    for (int p = 0; p < 2; p++)
        for (HWND h : A.page[p])
            ShowWindow(h, p == sel ? SW_SHOW : SW_HIDE);
}

void set_running_ui(bool running) {
    for (HWND h : A.lockWhileRunning)
        EnableWindow(h, !running);
    setText(A.hStart, running ? L"Stop" : L"Start");
    EnableWindow(A.hStart, TRUE);
}

// What the status line says while a stop is under way.
const wchar_t* stopping_text() {
    if (A.gate.force_offered())
        return L"Stuck? Force quit ends the program";
    return A.gate.closing() ? L"Stopping, then closing..." : L"Stopping...";
}

void refresh() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    GlobalMemoryStatusEx(&m);
    A.totalMB = m.ullTotalPhys >> 20;
    setText(A.hTotal, std::to_wstring(m.ullTotalPhys >> 20) + L" MB");
    setText(A.hFree, std::to_wstring(m.ullAvailPhys >> 20) + L" MB");
    if (!A.run)
        return;
    Run& R = *A.run;
    bool busy = A.busy.load();
    if (busy && A.gate.tick(plat::now())) { // the stop is taking a while: the button becomes the way out
        setText(A.hStart, L"Force quit");
        EnableWindow(A.hStart, TRUE);
    }
    const Run::Status S = R.status();
    double t = S.tStart > 0 ? (busy ? plat::now() : A.endT) - S.tStart : 0;
    if (t < 0)
        t = 0;
    u64 s = (u64)t;
    setText(A.hDur,
            std::to_wstring(s / 86400) + L":" + two(s / 3600 % 24) + L":" + two(s / 60 % 60) + L":" + two(s % 60));
    double cov = S.regionBytes ? 100.0 * (double)S.vtotal / (double)S.regionBytes : 0.0;
    setText(A.hCov, std::to_wstring((u64)cov) + L" %");
    u64 err = S.errors;
    setText(A.hErr, std::to_wstring(err));
    if (err != A.lastErr) {
        if (err > A.lastErr && SendMessageW(A.hBeep, BM_GETCHECK, 0, 0) == BST_CHECKED)
            MessageBeep(MB_ICONERROR);
        A.lastErr = err;
        InvalidateRect(A.hErr, nullptr, TRUE);
    }
    const std::string& lab = S.label;
    if (busy && A.gate.stopping())
        setText(A.hTest, stopping_text());
    else if (busy)
        setText(A.hTest, S.tStart > 0 ? std::wstring(lab.begin(), lab.end()) : L"Allocating memory...");
    else if (!A.stopRequested && err == 0 && S.tStart > 0)
        setText(A.hTest, L"Finished");
}

// ~90% of free RAM, rounded down so every thread gets the same whole number of 2 MB blocks (no unused remainder)
u64 auto_mb(int threads) {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    GlobalMemoryStatusEx(&m);
    u64 want = (m.ullAvailPhys >> 20) / 100 * 90, step = (u64)std::max(1, threads) * 2;
    return std::max<u64>(want / step * step, 64);
}
void apply_auto_mb() {
    wchar_t b[32];
    GetWindowTextW(A.hThr, b, 32);
    int t = _wtoi(b);
    if (t < 1)
        t = std::max(1, plat::ncpu());
    A.setting = true;
    SetWindowTextW(A.hMB, std::to_wstring(auto_mb(t)).c_str());
    A.setting = false;
}
u64 read_uint(HWND edit, u64 lo, u64 hi, u64 def) {
    wchar_t b[32];
    GetWindowTextW(edit, b, 32);
    u64 v = (u64)_wtoi64(b);
    if (!v)
        v = def;
    return std::min(std::max(v, lo), hi);
}

void start_run() {
    if (A.busy.load())
        return;
    if (A.worker.joinable())
        A.worker.join();
    Settings s;
    int cpus = std::max(1, plat::ncpu());
    s.threads = (int)read_uint(A.hThr, 1, 256, (u64)cpus);
    wchar_t mbt[32];
    GetWindowTextW(A.hMB, mbt, 32);
    s.megabytes =
        (A.mbAuto || _wtoi64(mbt) <= 0) ? auto_mb(s.threads) : read_uint(A.hMB, 64, std::max<u64>(A.totalMB, 64), 64);
    A.setting = true;
    SetWindowTextW(A.hMB, std::to_wstring(s.megabytes).c_str());
    A.setting = false;
    SetWindowTextW(A.hThr, std::to_wstring(s.threads).c_str());
    s.stopOnError = SendMessageW(A.hStopErr, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.cacheMode = (CacheMode)std::max(0, (int)SendMessageW(A.hCache, CB_GETCURSEL, 0, 0)); // list order = enum order
    s.rngMode = (Rng)std::max(0, (int)SendMessageW(A.hRng, CB_GETCURSEL, 0, 0));
    s.suite = (SuiteSet)std::max(0, (int)SendMessageW(A.hSet, CB_GETCURSEL, 0, 0));
    s.stressFpu = SendMessageW(A.hFpu, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.largePages = true;
    s.loops = 0; // runs until stopped

    { // everything the engine reports goes to the log file next to the exe
        std::lock_guard<std::mutex> l(A.logMu);
        A.log = _wfopen(exe_dir_file(L"ddr5ramtest.log").c_str(), L"a");
        A.lastLine.clear();
        A.logWhere = 0;
        A.logFailed = false;
        if (!A.log) { // exe folder not writable (e.g. Program Files): fall back to the temp folder rather than losing
                      // the error details
            wchar_t tp[MAX_PATH + 1];
            DWORD n = GetTempPathW(MAX_PATH, tp);
            if (n && n <= MAX_PATH) {
                A.log = _wfopen((std::wstring(tp, n) + L"ddr5ramtest.log").c_str(), L"a");
                A.logWhere = 1;
            }
            if (!A.log)
                A.logWhere = 2;
        }
        if (A.log) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            if (fprintf(A.log,
                        "---- DDR5 RAM Test run %04d-%02d-%02d %02d:%02d:%02d  %llu MB, %d threads, cache=%d rng=%d "
                        "set=%d fpu=%d stop-on-error=%d ----\n",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, (unsigned long long)s.megabytes,
                        s.threads, (int)s.cacheMode, (int)s.rngMode, (int)s.suite, (int)s.stressFpu,
                        (int)s.stopOnError) < 0 ||
                fflush(A.log) != 0)
                A.logFailed = true;
        }
    }
    A.logger.sink = [](const std::string& t) {
        std::lock_guard<std::mutex> l(A.logMu);
        if (A.log && (fputs(t.c_str(), A.log) == EOF || fflush(A.log) != 0))
            A.logFailed = true; // shown once, in the end text (on_run_done); the lines still reach the log pane
        size_t e = t.find_last_not_of("\r\n");
        if (e != std::string::npos) {
            size_t b = t.find_last_of('\n', e);
            b = b == std::string::npos ? 0 : b + 1;
            A.lastLine = t.substr(b, e - b + 1);
        }
    };
    A.run = std::make_unique<Run>(A.logger);
    A.lastErr = 0;
    A.stopRequested = false;
    A.endT = 0;
    setText(A.hDur, L"0:00:00:00");
    setText(A.hCov, L"0 %");
    setText(A.hErr, L"0");
    setText(A.hTest, L"Allocating memory...");
    InvalidateRect(A.hErr, nullptr, TRUE);
    A.busy = true;
    A.gate.on_run_start();
    set_running_ui(true);
    Run* R = A.run.get();
    HWND wnd = A.wnd;
    A.worker = std::thread([s, R, wnd] {
        int rc = run_suite(s, *R);
        PostMessageW(wnd, WM_RUN_DONE, (WPARAM)rc, 0);
    });
}

void request_stop() {
    if (!A.busy.load() || !A.run)
        return;
    A.stopRequested = true;
    A.run->stop = true;
    setText(A.hStart, L"Stopping..."); // turns into "Force quit" after a few seconds (StopGate::tick)
    EnableWindow(A.hStart, FALSE);
    setText(A.hTest, L"Stopping...");
}

// The way out of a stop that does not finish (one long OS call, a wedged run): end the process at once, like the CLI's
// second Ctrl-C. The log lines are flushed as they are written, so only the open file handle is dropped here. The
// worker may hold the log mutex, so it is only tried; TerminateProcess (not ExitProcess) because the static App would
// otherwise destroy a joinable std::thread.
[[noreturn]] void force_exit() {
    if (A.logMu.try_lock()) {
        if (A.log) {
            fclose(A.log);
            A.log = nullptr;
        }
        A.logMu.unlock();
    }
    TerminateProcess(GetCurrentProcess(), 1);
    std::abort();
}

// Carry out what the StopGate decided.
void apply_gate(StopGate::Act a) {
    if (a == StopGate::Act::ForceExit)
        force_exit();
    if (a == StopGate::Act::RequestStop)
        request_stop();
    else if (a == StopGate::Act::CloseNow)
        DestroyWindow(A.wnd);
}

void on_run_done(int rc, bool closing) {
    if (A.worker.joinable())
        A.worker.join();
    A.endT = plat::now();
    A.busy = false;
    std::string last;
    {
        std::lock_guard<std::mutex> l(A.logMu);
        last = A.lastLine;
        if (A.log) {
            fclose(A.log);
            A.log = nullptr;
        }
    }
    set_running_ui(false);
    refresh();
    if (A.run && A.run->error_count() > 0)
        setText(A.hTest, A.logFailed       ? L"Errors found - the log file could not be written completely (disk full?)"
                         : A.stopRequested ? L"Stopped (errors found)"
                         : A.logWhere == 0 ? L"Errors found - see ddr5ramtest.log"
                         : A.logWhere == 1 ? L"Errors found - log is in %TEMP%"
                                           : L"Errors found (no log file could be written)");
    else if (A.stopRequested)
        setText(A.hTest, L"Stopped");
    if (rc >= 2) {
        setText(A.hTest, L"Could not run");
        if (closing)
            return; // the user is closing the window: no box in the way
        std::wstring msg(last.begin(), last.end());
        if (msg.empty())
            msg = L"The test could not start.";
        MessageBoxW(A.wnd, msg.c_str(), APP_TITLE, MB_OK | MB_ICONWARNING);
    }
}

void build_ui() {
    A.font = CreateFontW(-MulDiv(9, A.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                         0, L"Segoe UI");
    A.fontBig = CreateFontW(-MulDiv(11, A.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, 0, L"Segoe UI");

    A.tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, S(6), S(6),
                            S(238), S(388), A.wnd, (HMENU)(INT_PTR)ID_TAB, A.inst, nullptr);
    SendMessageW(A.tab, WM_SETFONT, (WPARAM)A.font, TRUE);
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"Testing";
    TabCtrl_InsertItem(A.tab, 0, &ti);
    ti.pszText = (LPWSTR)L"Advanced";
    TabCtrl_InsertItem(A.tab, 1, &ti);

    const int X = 14, W = 222;
    // ---- Testing page
    int y = 38;
    group(L"System status", X, y, W, 58, 0);
    label(L"Total memory:", X + 10, y + 20, 100, 0);
    A.hTotal = label(L"", X + 110, y + 20, 100, 0, SS_RIGHT, ID_TOTAL);
    label(L"Free memory:", X + 10, y + 38, 100, 0);
    A.hFree = label(L"", X + 110, y + 38, 100, 0, SS_RIGHT, ID_FREE);

    y += 64;
    group(L"Test settings", X, y, W, 122, 0);
    label(L"Megabytes:", X + 10, y + 26, 90, 0);
    A.hMB = mk(L"EDIT", L"", ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP, X + 108, y + 22, 102, 22, ID_MB, 0,
               WS_EX_CLIENTEDGE);
    A.hMBud = mk(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0,
                 ID_MBUD, 0);
    label(L"Threads:", X + 10, y + 54, 90, 0);
    A.hThr = mk(L"EDIT", L"", ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP, X + 108, y + 50, 102, 22, ID_THR, 0,
                WS_EX_CLIENTEDGE);
    A.hThrUd = mk(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0,
                  ID_THRUD, 0);
    A.hBeep = mk(L"BUTTON", L"Beep on error", BS_AUTOCHECKBOX | WS_TABSTOP, X + 10, y + 82, 190, 18, ID_BEEP, 0);
    A.hStopErr = mk(L"BUTTON", L"Stop on error", BS_AUTOCHECKBOX | WS_TABSTOP, X + 10, y + 100, 190, 18, ID_STOPERR, 0);
    SendMessageW(A.hBeep, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(A.hStopErr, BM_SETCHECK, BST_CHECKED, 0);

    y += 128;
    group(L"Test status", X, y, W, 102, 0);
    label(L"Duration:", X + 10, y + 20, 90, 0);
    A.hDur = label(L"0:00:00:00", X + 110, y + 20, 100, 0, SS_RIGHT, ID_DUR);
    label(L"Coverage:", X + 10, y + 38, 90, 0);
    A.hCov = label(L"0 %", X + 110, y + 38, 100, 0, SS_RIGHT, ID_COV);
    label(L"Error count:", X + 10, y + 56, 90, 0);
    A.hErr = label(L"0", X + 110, y + 56, 100, 0, SS_RIGHT, ID_ERR);
    A.hTest = label(L"", X + 10, y + 78, W - 20, 0, SS_LEFT | SS_ENDELLIPSIS, ID_TEST);

    y += 108;
    A.hStart = mk(L"BUTTON", L"Start", BS_PUSHBUTTON | WS_TABSTOP, X, y, W, 44, ID_START, 0);
    SendMessageW(A.hStart, WM_SETFONT, (WPARAM)A.fontBig, TRUE);

    // ---- Advanced page
    y = 38;
    group(L"Advanced", X, y, W, 112, 1);
    label(L"CPU cache:", X + 10, y + 28, 80, 1);
    A.hCache = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X + 92, y + 24, 118, 160, ID_CACHE, 1);
    label(L"RNG:", X + 10, y + 58, 80, 1);
    A.hRng = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X + 92, y + 54, 118, 160, ID_RNG, 1);
    label(L"Test set:", X + 10, y + 88, 80, 1);
    A.hSet = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X + 92, y + 84, 118, 160, ID_SET, 1);
    for (const wchar_t* t : {L"Default", L"Bypass (flush)", L"Cached", L"Non-temporal"})
        SendMessageW(A.hCache, CB_ADDSTRING, 0, (LPARAM)t);
    for (const wchar_t* t : {L"Default", L"SplitMix64", L"Murmur3 mix", L"Xorshift64*"})
        SendMessageW(A.hRng, CB_ADDSTRING, 0, (LPARAM)t);
    for (const wchar_t* t : {L"Full", L"Quick"})
        SendMessageW(A.hSet, CB_ADDSTRING, 0, (LPARAM)t);
    SendMessageW(A.hCache, CB_SETCURSEL, 0, 0);
    SendMessageW(A.hRng, CB_SETCURSEL, 0, 0);
    SendMessageW(A.hSet, CB_SETCURSEL, 0, 0);

    y += 118;
    group(L"Experimental", X, y, W, 48, 1);
    A.hFpu = mk(L"BUTTON", L"Stress FPU", BS_AUTOCHECKBOX | WS_TABSTOP, X + 10, y + 20, 190, 18, ID_FPU, 1);

    y += 56;
    mk(L"STATIC",
       L"CPU cache: Default follows each test; Bypass reads straight from DRAM (clflush every access); Cached uses "
       L"normal write-back traffic; Non-temporal uses streaming stores.\n\n"
       L"RNG: data generator for hash tests.\n\nTest set: Quick runs a shorter subset.\n\nStress FPU adds an AVX load "
       L"on every thread (more power draw).",
       SS_LEFT, X, y, W, 150, 0, 1);

    // spin buttons: ranges, defaults, acceleration
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    GlobalMemoryStatusEx(&m);
    A.totalMB = m.ullTotalPhys >> 20;
    u64 freeMB = m.ullAvailPhys >> 20;
    u64 defMB = std::max<u64>(64, freeMB / 100 * 90);
    int cpus = std::max(1, plat::ncpu());
    SendMessageW(A.hMBud, UDM_SETBUDDY, (WPARAM)A.hMB, 0);
    SendMessageW(A.hMBud, UDM_SETRANGE32, 64, (LPARAM)std::max<u64>(A.totalMB, 64));
    SendMessageW(A.hMBud, UDM_SETPOS32, 0, (LPARAM)defMB);
    UDACCEL acc[3] = {{0, 16}, {1, 128}, {3, 1024}};
    SendMessageW(A.hMBud, UDM_SETACCEL, 3, (LPARAM)acc);
    SendMessageW(A.hThrUd, UDM_SETBUDDY, (WPARAM)A.hThr, 0);
    SendMessageW(A.hThrUd, UDM_SETRANGE32, 1, 256);
    SendMessageW(A.hThrUd, UDM_SETPOS32, 0, cpus);

    A.lockWhileRunning = {A.hMB, A.hMBud, A.hThr, A.hThrUd, A.hStopErr, A.hCache, A.hRng, A.hSet, A.hFpu};
    show_page(0);
    refresh();
    apply_auto_mb();
    A.setting = false;
}

void apply_caption_style(
    HWND h) { // dark red caption like the reference window (Windows 11 only; silently ignored elsewhere)
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm)
        return; // System32 only: no DLL search-order hijack
    using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    Fn f = (Fn)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (f) {
        COLORREF cap = RGB(128, 0, 0), txt = RGB(255, 255, 255), brd = RGB(128, 0, 0);
        f(h, 35, &cap, sizeof cap);
        f(h, 36, &txt, sizeof txt);
        f(h, 34, &brd, sizeof brd);
    }
    FreeLibrary(dwm);
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        A.wnd = h;
        build_ui();
        SetTimer(h, TIMER_ID, 500, nullptr);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ID)
            refresh();
        return 0;
    case WM_NOTIFY: {
        NMHDR* n = (NMHDR*)lp;
        if (n->idFrom == ID_TAB && n->code == TCN_SELCHANGE)
            show_page(TabCtrl_GetCurSel(A.tab));
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && !A.setting) {
            if (LOWORD(wp) == ID_MB) {
                wchar_t b[32];
                GetWindowTextW(A.hMB, b, 32);
                A.mbAuto = b[0] == 0;
                if (A.mbAuto)
                    apply_auto_mb();
            } // typed a value = manual; cleared = automatic
            else if (LOWORD(wp) == ID_THR && A.mbAuto)
                apply_auto_mb(); // thread count changed: refill memory automatically
        }
        if (LOWORD(wp) == ID_START && HIWORD(wp) == BN_CLICKED) {
            if (A.busy.load())
                apply_gate(A.gate.on_stop_click(plat::now()));
            else
                start_run();
        }
        return 0;
    case WM_RUN_DONE: { // gate first: refresh() inside on_run_done must not offer "Force quit" for a run that has ended
        StopGate::Act a = A.gate.on_run_done(); // CloseNow if the window was closed while the run was ending
        on_run_done((int)wp, a == StopGate::Act::CloseNow);
        apply_gate(a);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        if ((HWND)lp == A.hErr)
            SetTextColor(dc, A.lastErr > 0 ? RGB(200, 0, 0) : RGB(0, 128, 128));
        else
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_CLOSE: // never waits for the worker: the window stays alive until WM_RUN_DONE (or a forced exit)
        apply_gate(A.gate.on_close(plat::now()));
        if (A.gate.running())
            setText(A.hTest, stopping_text());
        return 0;
    case WM_DESTROY:
        KillTimer(h, TIMER_ID);
        {
            std::lock_guard<std::mutex> l(A.logMu);
            if (A.log) {
                fclose(A.log);
                A.log = nullptr;
            }
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    A.inst = inst;
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HDC dc = GetDC(nullptr);
    A.dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    if (A.dpi < 96)
        A.dpi = 96;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"Ddr5RamTestWindow";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, ex = WS_EX_CONTROLPARENT;
    RECT r{0, 0, S(250), S(400)};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN), w = r.right - r.left,
        hgt = r.bottom - r.top;
    HWND hw = CreateWindowExW(ex, wc.lpszClassName, APP_TITLE, style, (sw - w) / 2, (sh - hgt) / 3, w, hgt, nullptr,
                              nullptr, inst, nullptr);
    if (!hw)
        return 1;
    apply_caption_style(hw);
    ShowWindow(hw, show);
    UpdateWindow(hw);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hw, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (A.worker.joinable())
        A.worker.join();
    return (int)msg.wParam;
}
