// log.cpp - definitions for log.h.
#include "log.h"
#include <cstdarg>
#include <cstdio>

namespace ddr5 {

void Logger::emit(const std::string& t) const {
    if (sink)
        sink(t);
    else {
        fputs(t.c_str(), stdout);
        fflush(stdout);
    }
}
void Logger::emitf(const char* fmt, ...) const {
    char b[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    emit(b);
}
void Logger::print_clear() const {
    if (console && tty)
        printf("\r%*s\r", 118, "");
}

} // namespace ddr5
