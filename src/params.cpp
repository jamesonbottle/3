// params.cpp - definitions for params.h.
#include "params.h"
#include <cctype>
#include <cstdlib>

namespace ddr5 {

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::string lower(std::string s) {
    for (auto& c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}
u64 pnum(std::string s) {
    s = trim(s);
    if (s.empty())
        return 0;
    u64 mul = 1;
    bool hex = s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    if (!hex) {
        char c = (char)toupper((unsigned char)s.back());
        if (c == 'K') {
            mul = 1ull << 10;
            s.pop_back();
        } else if (c == 'M') {
            mul = 1ull << 20;
            s.pop_back();
        } else if (c == 'G') {
            mul = 1ull << 30;
            s.pop_back();
        }
    }
    return strtoull(s.c_str(), nullptr, hex ? 16 : 10) * mul;
}
std::vector<std::string> split_ws(const std::string& s, bool commas) {
    std::vector<std::string> o;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || (commas && c == ',')) {
            if (!cur.empty())
                o.push_back(cur), cur.clear();
        } else
            cur += c;
    }
    if (!cur.empty())
        o.push_back(cur);
    return o;
}

} // namespace ddr5
