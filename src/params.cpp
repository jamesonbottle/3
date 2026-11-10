// params.cpp - definitions for params.h.
#include "params.h"
#include <cctype>
#include <cstdlib>
#include <sstream>

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

bool parse_cfg_stream(std::istream& f, const std::string& path, Cfg& cfg, std::vector<TestDef>& tests) {
    std::string line, sect;
    Params defaults;
    TestDef* cur = nullptr;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        size_t h = line.find('#');
        if (h != std::string::npos)
            line = line.substr(0, h);
        line = trim(line);
        if (line.empty() || line[0] == ';')
            continue;
        if (line[0] == '[') {
            sect = lower(trim(line.substr(1, line.find(']') - 1)));
            if (sect == "test") {
                tests.push_back({"test" + std::to_string(tests.size() + 1), defaults});
                cur = &tests.back();
            } else if (sect == "default") {
                defaults = Params();
                cur = nullptr;
            }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            emitf("%s:%d: expected key = value\n", path.c_str(), lineno);
            continue;
        }
        std::string k = lower(trim(line.substr(0, eq))), v = trim(line.substr(eq + 1));
        if (sect == "global") {
            if (k == "memory_percent")
                cfg.memPercent = pnum(v);
            else if (k == "memory_mb")
                cfg.memMB = pnum(v);
            else if (k == "margin_mb")
                cfg.marginMB = pnum(v);
            else if (k == "threads")
                cfg.threads = (int)pnum(v);
            else if (k == "loops")
                cfg.loops = pnum(v);
            else if (k == "large_pages")
                cfg.large = pnum(v) != 0;
            else if (k == "stop_on_error")
                cfg.stopOnErr = pnum(v) != 0;
            else if (k == "log")
                cfg.logFile = v;
            else if (k == "max_errors_logged")
                cfg.maxLog = pnum(v);
            else if (k == "cache_mb")
                cfg.cacheMB = pnum(v);
            else
                emitf("%s:%d: unknown global key '%s'\n", path.c_str(), lineno, k.c_str());
        } else if (sect == "default")
            defaults.m[k] = v;
        else if (sect == "test" && cur) {
            if (k == "name")
                cur->name = v;
            else
                cur->p.m[k] = v;
        }
    }
    return true;
}

} // namespace ddr5
