// "Start a run" in the GUI has two steps that can throw: allocating the Run object and creating the worker thread
// (std::thread throws std::system_error when the OS refuses a new thread). Pure logic with no window code, so
// tools/unit_tests.cpp can test it on every platform; src/main_gui.cpp::start_run() uses it (audit item F2).
//
// The rule for the caller: do every step that can throw FIRST, into local variables, and only when try_start()
// returned true switch the window to "running" (A.busy, the StopGate, the controls). A failed start then has nothing
// to undo in the UI: the Start button stays enabled and no WM_RUN_DONE is ever waited for.
#pragma once
#include <exception>
#include <string>

namespace ddr5 {

// Runs `step`. Returns true if it finished. Returns false if it threw, with the reason in `why` (what() of a
// std::exception, "unknown error" otherwise); no exception leaves this function.
template <class Step>
bool try_start(Step&& step, std::string& why) {
    why.clear();
    try {
        step();
        return true;
    } catch (const std::exception& e) {
        why = e.what();
    } catch (...) {}
    if (why.empty())
        why = "unknown error";
    return false;
}

} // namespace ddr5
