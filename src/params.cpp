// params.cpp - definitions for params.h.
#include "params.h"
#include <cctype>
#include <cstdlib>
#include <mutex>

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
bool pnum_checked(const std::string& in, u64& out) {
    std::string s = trim(in);
    bool hex = s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    u64 mul = 1;
    if (hex)
        s.erase(0, 2);
    else if (!s.empty()) {
        char c = (char)toupper((unsigned char)s.back());
        if (c == 'K' || c == 'M' || c == 'G') {
            mul = 1ull << (c == 'K' ? 10 : c == 'M' ? 20 : 30);
            s.pop_back();
        }
    }
    if (s.empty())
        return false;
    const u64 base = hex ? 16 : 10;
    u64 v = 0;
    for (char ch : s) {
        unsigned d;
        if (ch >= '0' && ch <= '9')
            d = (unsigned)(ch - '0');
        else if (hex && ch >= 'a' && ch <= 'f')
            d = (unsigned)(ch - 'a' + 10);
        else if (hex && ch >= 'A' && ch <= 'F')
            d = (unsigned)(ch - 'A' + 10);
        else
            return false; // sign, decimal point, trailing text
        if (v > (~0ull - d) / base)
            return false; // would not fit in 64 bits
        v = v * base + d;
    }
    if (mul > 1 && v > ~0ull / mul)
        return false;
    out = v * mul;
    return true;
}
u64 pnum(std::string s) {
    u64 v = 0;
    return pnum_checked(s, v) ? v : 0;
}
void Params::note_bad(const std::string& what) const {
    static std::mutex mu; // an algo may read its Params from several threads; bad values are rare, so one lock is fine
    std::lock_guard<std::mutex> l(mu);
    bad.push_back(where() + what);
}
u64 Params::parse_num(const std::string& k, const std::string& v) const {
    u64 x = 0;
    if (pnum_checked(v, x))
        return x;
    note_bad("key '" + k + "': '" + v + "' is not a whole number (digits with an optional K/M/G suffix, or 0x hex)");
    return 0;
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
