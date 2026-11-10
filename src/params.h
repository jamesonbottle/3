#pragma once
// params.h - string helpers, Params, suite.cfg parser.
#include <istream>
#include <map>
#include <string>
#include <vector>
#include "log.h"
#include "types.h"

namespace ddr5 {

std::string trim(std::string s);
std::string lower(std::string s);
u64 pnum(std::string s); // number with optional K/M/G suffix, or 0x hex
std::vector<std::string> split_ws(const std::string& s, bool commas = false);

struct Params {
    std::map<std::string, std::string> m;
    bool has(const std::string& k) const { return m.count(k) > 0; }
    std::string str(const std::string& k, const std::string& d = "") const {
        auto i = m.find(k);
        return i == m.end() ? d : i->second;
    }
    u64 num(const std::string& k, u64 d) const {
        auto i = m.find(k);
        return i == m.end() ? d : pnum(i->second);
    }
    bool flag(const std::string& k, bool d) const {
        auto i = m.find(k);
        if (i == m.end())
            return d;
        std::string v = lower(i->second);
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }
    std::vector<std::string> words(const std::string& k, const std::string& d = "") const {
        return split_ws(str(k, d));
    }
    std::vector<u64> nums(const std::string& k, const std::string& d = "") const {
        std::vector<u64> o;
        for (auto& w : split_ws(str(k, d), true))
            o.push_back(pnum(w));
        return o;
    }
};
struct TestDef {
    std::string name;
    Params p;
};
struct Cfg {
    u64 memPercent = 75, memMB = 0, marginMB = 768, loops = 1, maxLog = 200, cacheMB = 192;
    int threads = 0;
    bool large = true, stopOnErr = false;
    std::string logFile = "ddr5test.log";
};

bool parse_cfg_stream(std::istream& f, const std::string& path, Cfg& cfg, std::vector<TestDef>& tests);

} // namespace ddr5
