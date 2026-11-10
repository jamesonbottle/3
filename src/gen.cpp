// gen.cpp - sat_table, expand_style, build_vars.
#include "gen.h"
#include <cstring>
#include <utility>
#include "log.h"

namespace ddr5 {

const std::vector<SatPat>& sat_table() {
    static const std::vector<SatPat> t = [] {
        std::vector<SatPat> v;
        auto add = [&](const char* n, std::initializer_list<u32> l, int a, int b, int c, int d) {
            SatPat p{};
            p.name = n;
            int i = 0;
            for (u32 x : l)
                p.w[i++] = x;
            p.len = i;
            p.weight[0] = a;
            p.weight[1] = b;
            p.weight[2] = c;
            p.weight[3] = d;
            v.push_back(p);
        };
        SatPat w1{}, wi{}, w0{};
        w1.name = "walking-ones";
        wi.name = "walking-inv-ones";
        w0.name = "walking-zeros";
        w1.len = wi.len = w0.len = 64;
        for (int i = 0; i < 64; i++) {
            u32 x = i < 32 ? 1u << i : (i < 63 ? 1u << (62 - i) : 0u); // up 1..0x80000000, then back down to 0
            w1.w[i] = x;
            w0.w[i] = ~x;
            u32 u = 1u << (i / 2);
            wi.w[i] = (i & 1) ? ~u : u;
        }
        int a[4] = {1, 1, 2, 1}, b[4] = {2, 2, 5, 5};
        memcpy(w1.weight, a, sizeof a);
        memcpy(w0.weight, a, sizeof a);
        memcpy(wi.weight, b, sizeof b);
        v.push_back(w1);
        v.push_back(wi);
        v.push_back(w0);
        add("zero/one", {0x00000000u, 0xFFFFFFFFu}, 5, 5, 15, 5);
        add("zeros", {0x00000000u}, 2, 0, 0, 0);
        add("ones", {0xFFFFFFFFu}, 2, 0, 0, 0);
        add("5555", {0x55555555u}, 2, 0, 0, 0);
        add("AAAA", {0xAAAAAAAAu}, 2, 0, 0, 0);
        add("5/A", {0x55555555u, 0xAAAAAAAAu}, 1, 1, 1, 1);
        add("5AA5", {0x5AA5A55Au, 0xA55A5AA5u, 0xA55A5AA5u, 0x5AA5A55Au}, 1, 1, 1, 1);
        add("8b10b-long", {0x16161616u}, 2, 0, 0, 0);
        add("8b10b-short", {0xB5B5B5B5u}, 2, 0, 0, 0);
        add("8b10b-checker", {0xB5B5B5B5u, 0x4A4A4A4Au}, 1, 0, 0, 1);
        add("5557", {0x55555557u, 0x55575555u}, 0, 2, 0, 0);
        add("0002/FFFD", {0x00020002u, 0xFFFDFFFDu}, 0, 2, 0, 0);
        return v;
    }();
    return t;
}

static Beats beats_from_lanes(const u16* s) {
    Beats b{};
    for (int L = 0; L < 32; L++)
        for (int t = 0; t < 16; t++)
            if ((s[L] >> t) & 1)
                b[t] |= 1u << L;
    return b;
}
static u16 rotl16(u16 v, int r) {
    r &= 15;
    return (u16)((v << r) | (v >> ((16 - r) & 15)));
}

// DQ-lane stress styles. Lane = bit position inside the 32-bit beat; beat = position in the BL16 burst.
static void expand_style(const std::string& st, const Params& P, std::vector<std::pair<std::string, Beats>>& out,
                         const Logger& log) {
    u16 s[32];
    auto uni = [&](const std::string& n, u16 e, u16 o) {
        for (int L = 0; L < 32; L++)
            s[L] = (L & 1) ? o : e;
        out.push_back({n, beats_from_lanes(s)});
    };
    if (st == "sso")
        uni("sso(all lanes toggle in phase)", 0x5555, 0x5555);
    else if (st == "xtalk")
        uni("xtalk(neighbour lanes opposite phase)", 0x5555, 0xAAAA);
    else if (st == "half")
        uni("half-nyquist(0011)", 0x3333, 0xCCCC);
    else if (st == "quarter")
        uni("quarter(00001111)", 0x0F0F, 0xF0F0);
    else if (st == "low")
        uni("lowfreq(8 ones/8 zeros)", 0x00FF, 0xFF00);
    else if (st == "pulse") {
        for (int L = 0; L < 32; L++) {
            s[L] = (u16)(1u << (L % 16));
        }
        out.push_back({"pulse(single 1 in zeros)", beats_from_lanes(s)});
        for (int L = 0; L < 32; L++) {
            s[L] = (u16) ~(1u << (L % 16));
        }
        out.push_back({"pulse_inv(single 0 in ones)", beats_from_lanes(s)});
    } else if (st == "skew") {
        for (int L = 0; L < 32; L++) {
            s[L] = rotl16(0x0F0F, L);
        }
        out.push_back({"skew(quarter, phase=lane)", beats_from_lanes(s)});
        for (int L = 0; L < 32; L++) {
            s[L] = rotl16(0x00FF, L);
        }
        out.push_back({"skew(low, phase=lane)", beats_from_lanes(s)});
    } else if (st == "victim") {
        auto vl = P.nums("victims");
        std::vector<u64> victims = vl;
        if (victims.empty())
            for (u64 v = 0; v < 32; v++)
                victims.push_back(v);
        struct M {
            const char* n;
            u16 v, a;
        } modes[] = {{"toggle/aggr=0", 0x5555, 0x0000},        {"toggle/aggr=1", 0x5555, 0xFFFF},
                     {"toggle/aggr=opposite", 0x5555, 0xAAAA}, {"toggle/aggr=inphase", 0x5555, 0x5555},
                     {"static0/aggr=toggle", 0x0000, 0x5555},  {"static1/aggr=toggle", 0xFFFF, 0x5555}};
        for (u64 v : victims)
            for (auto& md : modes) {
                for (int L = 0; L < 32; L++)
                    s[L] = (L == (int)v) ? md.v : md.a;
                out.push_back({"victim lane " + std::to_string(v) + " " + md.n, beats_from_lanes(s)});
            }
    } else if (st == "walk1" || st == "walk0") {
        for (int k = 0; k < 32; k++) {
            Beats b;
            for (int t = 0; t < 16; t++)
                b[t] = 1u << ((t + k) % 32);
            if (st == "walk0")
                for (auto& x : b)
                    x = ~x;
            out.push_back({st + " k=" + std::to_string(k), b});
        }
    } else if (st == "prbs") { // per-lane PRBS7 windows, different phase per lane => pseudo-random ISI/crosstalk
        u8 seq[127];
        u32 r = 0x7F;
        for (int i = 0; i < 127; i++) {
            u32 bit = ((r >> 6) ^ (r >> 5)) & 1;
            seq[i] = (u8)(r & 1);
            r = ((r << 1) | bit) & 0x7F;
        }
        for (int k = 0; k < 8; k++) {
            for (int L = 0; L < 32; L++) {
                u16 v = 0;
                for (int t = 0; t < 16; t++)
                    v |= (u16)(seq[(k * 13 + L * 5 + t) % 127] << t);
                s[L] = v;
            }
            out.push_back({"prbs7 variant " + std::to_string(k), beats_from_lanes(s)});
        }
    } else
        log.emitf("unknown style '%s'\n", st.c_str());
}

std::vector<Var> build_vars(const Params& P, u64 salt, const Logger& log) {
    std::vector<Var> out;
    std::string data = lower(P.str("data", "pattern"));
    u64 seed = P.num("seed", 0);
    if (data == "sat") { // every stressapptest-style pattern at each requested bus-width repeat (combinations it never
                         // uses are skipped)
        for (auto& sp : sat_table())
            for (u64 wd : P.nums("sat_width", "32 64 128 256")) {
                int rep = wd >= 256 ? 3 : wd >= 128 ? 2 : wd >= 64 ? 1 : 0;
                if (!sp.weight[rep])
                    continue;
                Var v;
                v.g.kind = 3;
                v.g.sp = &sp;
                v.g.rep = rep;
                v.label = std::string("sat ") + sp.name + " x" + std::to_string(32u << rep);
                out.push_back(v);
            }
        return out;
    }
    if (data == "hash" || data == "addr") {
        u64 ns = P.num("seeds", 1);
        for (u64 i = 0; i < ns; i++) {
            Var v;
            v.g.kind = data == "hash" ? 1 : 2;
            v.g.seed = (i || salt) ? splitmix(seed + i + salt) : seed;
            v.label = data + " seed=" + std::to_string(i);
            out.push_back(v);
        }
        return out;
    }
    if (P.has("style")) {
        std::vector<std::pair<std::string, Beats>> sv;
        for (auto& st : P.words("style"))
            expand_style(lower(st), P, sv, log);
        for (auto& [n, b] : sv) {
            Var v;
            v.g.set_beats(b);
            v.label = n;
            out.push_back(v);
        }
    } else {
        for (auto& tok : P.words("pattern", "0x00000000")) {
            Beats b{};
            std::vector<u32> vals;
            std::string c;
            for (size_t i = 0; i <= tok.size(); i++) {
                if (i == tok.size() || tok[i] == ':') {
                    if (!c.empty())
                        vals.push_back((u32)pnum(c));
                    c.clear();
                } else
                    c += tok[i];
            }
            if (vals.empty())
                vals.push_back(0);
            for (int t = 0; t < 16; t++)
                b[t] = vals[t % vals.size()];
            Var v;
            v.g.set_beats(b);
            v.label = "pattern " + tok;
            out.push_back(v);
        }
    }
    if (P.has("altshift")) { // invert data when bit N of the line index is set; a list sweeps several block sizes
        auto al = P.nums("altshift");
        std::vector<Var> o2;
        for (auto& v : out)
            for (u64 a : al) {
                Var m = v;
                m.g.altshift = (int)a;
                if (al.size() > 1)
                    m.label += " altshift " + std::to_string(a);
                o2.push_back(m);
            }
        if (!o2.empty())
            out = o2;
    }
    if (P.has("modulo")) { // memtest-style modulo-N: for each N and each offset, every Nth line gets the inverse
        std::vector<Var> o2;
        for (auto& v : out)
            for (u64 n : P.nums("modulo"))
                for (u64 off = 0; n > 1 && off < n; off++) {
                    Var m = v;
                    m.g.modn = n;
                    m.g.modoff = off;
                    m.label += " modulo " + std::to_string(n) + " off " + std::to_string(off);
                    o2.push_back(m);
                }
        if (!o2.empty())
            out = o2;
    }
    return out;
}

} // namespace ddr5
