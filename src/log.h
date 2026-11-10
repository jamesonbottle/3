#pragma once
// log.h - output sink and progress-line helpers. Parser, run and algos all report through emit()/emitf().
#include <functional>
#include <string>

extern std::function<void(const std::string&)>
    g_emit;            // output sink: stdout in CLI mode (when empty), log pane in the GUI
extern bool g_console; // console front-end: live progress output
extern bool
    g_tty; // stdout is a terminal (live progress line); false when piped / in CI (a progress line every 30 s instead)
void emit(const std::string& t);
void emitf(const char* fmt, ...);
void print_clear();
