#pragma once
// HANDOFF [TODO] log.h - output sink and progress-line helpers.
// Source:  engine.h 577-582 (g_emit, g_console, g_tty, emit, emitf, print_clear) and the forward declaration at 165.
// Declare: extern std::function<void(const std::string&)> g_emit; extern bool g_console, g_tty; void emit(const std::string&);
//          void emitf(const char* fmt, ...); void print_clear();
// Needs:   <functional>, <string> only. params.cpp, run.cpp, every algo and the front-ends report through this.
// Delete this block when done.
