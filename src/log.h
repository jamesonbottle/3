#pragma once
// log.h - output sink and progress-line helpers. Parser, run and algos report through a Logger (Run::log).
#include <functional>
#include <string>

namespace ddr5 {

// printf-style formatting appended to `s`. The text is never truncated and never overruns: the string grows to fit.
// (A formatting error appends nothing.)
void appendf(std::string& s, const char* fmt, ...);

// Where text output goes. The front-end owns one Logger; it must outlive every Run and every parse call that uses it.
struct Logger {
    std::function<void(const std::string&)> sink; // output sink: stdout in CLI mode (when empty), log pane in the GUI
    bool console = false;                         // console front-end: live progress output
    bool tty = true; // stdout is a terminal (live progress line); false when piped / in CI (a progress line every 30 s
                     // instead)
    void emit(const std::string& t) const;
    void emitf(const char* fmt, ...) const;
    void print_clear() const;
};

} // namespace ddr5
