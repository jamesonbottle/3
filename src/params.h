#pragma once
// params.h - string helpers, Params, TestDef, Cfg.
#include <map>
#include <set>
#include <string>
#include <vector>
#include "log.h"
#include "types.h"

namespace ddr5 {

std::string trim(std::string s);
std::string lower(std::string s);
// Strict number parser: a whole number with an optional K/M/G suffix (x1024 each), or 0x hex digits. Rejects empty
// input, a sign, decimals, trailing text ("4KB"), "0x" without digits and anything that does not fit in 64 bits (also
// after the suffix multiply). Leading/trailing blanks are ignored.
bool pnum_checked(const std::string& s, u64& out);
u64 pnum(std::string s); // lenient wrapper for callers without a place to report: 0 when pnum_checked() fails
std::vector<std::string> split_ws(const std::string& s, bool commas = false);

struct Params {
    std::map<std::string, std::string> m;
    std::string test; // name of the test these values belong to; only used to word messages (empty = none)
    // Values that num()/nums()/flag()/parse_num() could not parse, worded for the user ("test 'x': key 'k': ...").
    // They are recorded, not thrown: the accessor still returns a harmless default (0 / false) so a run is never
    // changed by them; run_suite validates every selected test up front (bad_values() in algo.h) and refuses to start.
    mutable std::vector<std::string> bad;
    // Test hook: when set, every key looked up through has/str/num/flag (so also words/nums) is added to it. Lets a
    // unit test learn which keys a helper reads, without a second list to keep in step. Not thread-safe; null in real
    // runs.
    mutable std::set<std::string>* reads = nullptr;
    void touch(const std::string& k) const {
        if (reads)
            reads->insert(k);
    }
    std::string where() const { return test.empty() ? "" : "test '" + test + "': "; }
    void note_bad(const std::string& what) const;                    // thread-safe append to `bad`
    u64 parse_num(const std::string& k, const std::string& v) const; // pnum_checked(v), recording a failure under key k
    bool has(const std::string& k) const {
        touch(k);
        return m.count(k) > 0;
    }
    std::string str(const std::string& k, const std::string& d = "") const {
        touch(k);
        auto i = m.find(k);
        return i == m.end() ? d : i->second;
    }
    u64 num(const std::string& k, u64 d) const {
        touch(k);
        auto i = m.find(k);
        return i == m.end() ? d : parse_num(k, i->second);
    }
    bool flag(const std::string& k, bool d) const {
        touch(k);
        auto i = m.find(k);
        if (i == m.end())
            return d;
        std::string v = lower(trim(i->second));
        if (v == "1" || v == "true" || v == "yes" || v == "on")
            return true;
        if (!(v == "0" || v == "false" || v == "no" || v == "off"))
            note_bad("key '" + k + "': '" + i->second + "' is not a flag (use 1/0, true/false, yes/no or on/off)");
        return false;
    }
    std::vector<std::string> words(const std::string& k, const std::string& d = "") const {
        return split_ws(str(k, d));
    }
    std::vector<u64> nums(const std::string& k, const std::string& d = "") const {
        std::vector<u64> o;
        for (auto& w : split_ws(str(k, d), true))
            o.push_back(parse_num(k, w));
        return o;
    }
};
struct TestDef {
    std::string name;
    Params p;                  // the [default] values, overridden by the keys of the [test] block
    std::set<std::string> own; // keys the [test] block itself set (the rest of `p` was inherited from [default])
    // Name of the test algorithm (the `algo` key, lower-cased; "fill" when absent). The one place that decides this.
    std::string algo() const { return lower(p.str("algo", "fill")); }
};
struct Cfg {
    u64 loops = 1, maxLog = 200;
    int threads = 0;
    bool large = true, stopOnErr = false;
    std::string logFile = "ddr5test.log";
};

} // namespace ddr5
