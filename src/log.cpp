// log.cpp - definitions for log.h.
#include "log.h"
#include <cstdarg>
#include <cstdio>

namespace ddr5 {

std::function<void(const std::string&)> g_emit;
bool g_console = false;
bool g_tty = true; // console front-end: stdout is a terminal (live progress line); false when piped / in CI (a progress
                   // line every 30 s instead)
void emit(const std::string& t) {
    if (g_emit)
        g_emit(t);
    else {
        fputs(t.c_str(), stdout);
        fflush(stdout);
    }
}
void emitf(const char* fmt, ...) {
    char b[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    emit(b);
}
void print_clear() {
    if (g_console && g_tty)
        printf("\r%*s\r", 118, "");
}

} // namespace ddr5
