// log.cpp - definitions for log.h.
#include "log.h"
#include <cstdarg>
#include <cstdio>

namespace ddr5 {

static void vappendf(std::string& s, const char* fmt, va_list ap) {
    va_list again; // vsnprintf consumes ap; a second pass is needed when the text did not fit
    va_copy(again, ap);
    char small[256];
    int n = vsnprintf(small, sizeof small, fmt, ap);
    if (n >= 0) {
        if ((size_t)n < sizeof small)
            s.append(small, (size_t)n);
        else {
            size_t old = s.size();
            s.resize(old + (size_t)n + 1); // + the terminating NUL that vsnprintf writes
            vsnprintf(&s[old], (size_t)n + 1, fmt, again);
            s.resize(old + (size_t)n);
        }
    }
    va_end(again);
}
void appendf(std::string& s, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vappendf(s, fmt, a);
    va_end(a);
}

void Logger::emit(const std::string& t) const {
    if (sink)
        sink(t);
    else {
        fputs(t.c_str(), stdout);
        fflush(stdout);
    }
}
void Logger::emitf(const char* fmt, ...) const {
    std::string s; // grows to fit: a long test name or path must not be cut off
    va_list a;
    va_start(a, fmt);
    vappendf(s, fmt, a);
    va_end(a);
    emit(s);
}
void Logger::print_clear() const {
    if (console && tty)
        printf("\r%*s\r", 118, "");
}

} // namespace ddr5
