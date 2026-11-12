// algo_march.cpp - user-defined march tests.
#include "algo.h"
#include "gen.h"
#include "lineio.h"
#include "log.h"
#include "order.h"
#include "platform.h"
#include "run.h"
#include "seed.h"
#include <cctype>
#include <string>
#include <vector>

namespace ddr5 {

// ------------------------------------------------------------------ algo: march (user-defined march tests)
static std::vector<MEl> parse_march(const std::string& s, const Logger& log, const std::string& where) {
    std::vector<MEl> out;
    for (auto& tok : split_ws(s)) {
        if (tok.size() < 3)
            continue;
        MEl e{};
        e.dir = tok[0];
        size_t a = tok.find('('), b = tok.rfind(')');
        if (a == std::string::npos || b == std::string::npos) {
            log.emitf("%sbad march element '%s'\n", where.c_str(), tok.c_str());
            continue;
        }
        std::string body = tok.substr(a + 1, b - a - 1);
        if (e.dir == 'D' || e.dir == 'd') {
            e.delay = true;
            if (!pnum_checked(body, e.ms)) {
                log.emitf("%sbad march element '%s' (delay must be a whole number of ms)\n", where.c_str(),
                          tok.c_str());
                continue;
            }
            out.push_back(e);
            continue;
        }
        if (e.dir == 'X' || e.dir == 'x') { // disturb element: X(r2) = 2 read passes, X(w1), X(rw3) on another window
            size_t k = 0;
            while (k < body.size() && isalpha((unsigned char)body[k]))
                k++;
            e.dist = true;
            e.dmode = std::max(0, parse_dmode(lower(body.substr(0, k))));
            e.dpasses = 1;
            if (k < body.size()) {
                u64 np = 0;
                if (!pnum_checked(body.substr(k), np)) {
                    log.emitf("%sbad march element '%s' (disturb passes must be a whole number)\n", where.c_str(),
                              tok.c_str());
                    continue;
                }
                e.dpasses = std::max<u64>(1, np);
            }
            out.push_back(e);
            continue;
        }
        for (auto& op : split_ws(body, true))
            if (op.size() == 2 && (op[0] == 'r' || op[0] == 'w'))
                e.ops.push_back({op[0], op[1] - '0'});
        out.push_back(e);
    }
    return out;
}
std::vector<MVar> parse_march_variants(const std::string& str, const Logger& log, const std::string& where) {
    std::vector<MVar> out;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : str + "|") {
        if (c == '|') {
            parts.push_back(cur);
            cur.clear();
        } else
            cur += c;
    }
    for (auto& raw : parts) {
        std::string part = trim(raw);
        if (part.empty())
            continue;
        std::string label;
        size_t p = part.find('('), eq = part.find('=');
        if (eq != std::string::npos && (p == std::string::npos || eq < p)) {
            label = trim(part.substr(0, eq));
            part = part.substr(eq + 1);
        }
        MVar v;
        v.els = parse_march(part, log, where);
        if (v.els.empty())
            continue;
        v.label = label.empty() ? "march" + std::to_string(out.size() + 1) : label;
        out.push_back(v);
    }
    return out;
}
// ------------------------------------------------------------------ options (the only place march reads its keys)
// `stride` (stride_list), `window_mb` (Run::window_lines) and the data keys (build_vars) belong to shared helpers.
const std::set<std::string>& MarchOpts::keys() {
    static const std::set<std::string> k = {"burst", "flush", "march", "nt", "order", "width"};
    return k;
}
MarchOpts MarchOpts::from(const Params& P, const Logger& log) {
    MarchOpts o;
    o.variants = parse_march_variants(P.str("march"), log, P.where());
    if (o.variants.empty())
        return o; // algo_march reports "no elements"; a bad `order` is not reported then (as before)
    for (auto& w : P.words("order", "seq")) {
        OrderSpec os;
        if (!Order::parse(w, os.k, os.rev)) {
            log.emitf("%sbad order '%s'\n", P.where().c_str(), w.c_str());
            continue;
        }
        o.orders.push_back({w, os});
    }
    if (o.orders.empty())
        o.orders.push_back({"seq", OrderSpec()});
    bool anyStride = false;
    for (auto& on : o.orders)
        if (on.second.k == Order::STRIDE)
            anyStride = true;
    o.strides = stride_list(P, anyStride);
    o.bursts = P.nums("burst", "1");
    if (o.bursts.empty())
        o.bursts.push_back(1);
    o.width = (int)P.num("width", 32);
    o.nt = P.flag("nt", false);
    o.flush = P.flag("flush", true);
    for (auto& m : o.variants)
        for (auto& e : m.els)
            if (e.dist)
                o.anyDist = true;
    return o;
}

void algo_march(Run& R, const Params& P) {
    auto vars = build_vars(P, R.salt, R.rng, R.log);
    const MarchOpts opt = MarchOpts::from(P, R.log);
    if (opt.variants.empty()) {
        R.log.emitf("march: no elements\n");
        return;
    }
    u64 wl = R.window_lines(P);
    bool distOk = !opt.anyDist || disturb_possible(R, wl, R.testName.c_str());
    struct Job {
        std::string label;
        const Gen* g;
        size_t mi;
        OrderSpec os;
        u64 stride;
        u64 burst;
    };
    std::vector<Job> jobs;
    for (auto& v : vars)
        for (size_t mi = 0; mi < opt.variants.size(); mi++)
            for (auto& on : opt.orders) {
                std::vector<u64> sl;
                if (on.second.k == Order::STRIDE)
                    sl = opt.strides;
                else
                    sl.push_back(1);
                for (u64 s : sl)
                    for (u64 b : opt.bursts)
                        jobs.push_back(
                            {v.label + (opt.variants.size() > 1 ? " | " + opt.variants[mi].label : "") +
                                 (opt.orders.size() > 1 || on.second.k != Order::SEQ || on.second.rev
                                      ? " order=" + on.first
                                      : "") +
                                 (on.second.k == Order::STRIDE ? " stride=" + std::to_string(s * kLineBytes) + "B"
                                                               : "") +
                                 (opt.bursts.size() > 1 || b > 1 ? " burst=" + std::to_string(b) : ""),
                             &v.g, mi, on.second, s, b});
            }
    u64 base = R.winctr[R.testName];
    R.winctr[R.testName] += jobs.size();
    R.parallel([&](int tid) {
        for (size_t ji = 0; ji < jobs.size(); ji++) {
            if (R.stop)
                break;
            const Job& j = jobs[ji];
            const auto& els = opt.variants[j.mi].els;
            if (tid == 0)
                R.set_label(j.label);
            View w = R.view(tid, wl, base + ji);
            Order o;
            o.init(j.os.k, w.n, j.stride, 5 + ji, R.salt);
            dispatch(opt.width, opt.nt, opt.flush, [&](auto Wc, auto NTc, auto FLc) {
                using io = IO<decltype(Wc)::value, decltype(NTc)::value, decltype(FLc)::value>;
                for (size_t ei = 0; ei < els.size(); ei++) {
                    const MEl& e = els[ei];
                    if (e.delay) {
                        R.sync();
                        if (tid == 0)
                            R.sleep_ms(e.ms);
                        R.sync();
                        continue;
                    }
                    if (e.dist) {
                        if (distOk)
                            disturb_traffic(R, R.dview(tid, wl, base + ji), e.dmode, e.dpasses,
                                            0xD157 + tid * 31 + ji * 7 + ei + R.salt);
                        _mm_sfence();
                        R.sync();
                        continue;
                    }
                    bool down = (e.dir == 'v' || e.dir == 'V') != j.os.rev;
                    auto apply = [&](const MOp& op, u64 i) {
                        u8* a = w.base + i * kLineBytes;
                        Line l;
                        j.g->make(w.g + i, a, op.v != 0, l);
                        if (op.t == 'w')
                            io::store(a, l);
                        else {
                            Line act;
                            R.vtick();
                            if (!io::verify(a, l, act))
                                R.report(tid, a, l, act,
                                         ("march el" + std::to_string(ei) + " r" + std::to_string(op.v)).c_str());
                        }
                    };
                    u64 cnt = 0;
                    for (u64 q0 = 0; q0 < w.n && !R.stop; q0 += j.burst) {
                        u64 qe = std::min(q0 + j.burst, w.n);
                        for (const MOp& op : e.ops)
                            for (u64 q = q0; q < qe; q++)
                                apply(op, o.pos(q, down));
                        cnt += qe - q0;
                        if (cnt >= kTickLines) {
                            R.tick();
                            cnt -= kTickLines;
                        }
                    }
                    _mm_sfence();
                    R.sync();
                }
            });
        }
    });
}

} // namespace ddr5
