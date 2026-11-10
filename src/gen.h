#pragma once
// gen.h - data generators: stressapptest-style pattern table, Gen, Var, build_vars.
#include <array>
#include <string>
#include <vector>
#include "log.h"
#include "params.h"
#include "seed.h"
#include "types.h"

namespace ddr5 {

// ------------------------------------------------------------------ data generators (pure functions of line index)
using Beats = std::array<u32, 16>; // 16 beats x 32 DQ = one BL16 burst on one DDR5 subchannel
// Data patterns in the style of Google's stressapptest table (the exact table is reimplemented from its public
// description, not copied): walking ones / inverse-alternating ones / zeros, 0/1, 5/A, 8b10b-style, and a few "busy"
// patterns that toggle many lanes per beat. Each 32-bit pattern word is held for 1/2/4/8 consecutive words ("bus width"
// 32/64/128/256 bit repeat). weight[w] = how often stressapptest-style random page filling picks this pattern at width
// w (0 = not used at that width).
struct SatPat {
    const char* name;
    std::array<u32, 64> w;
    int len;
    int weight[4];
};
const std::vector<SatPat>& sat_table();
static inline u32 sat_word(const SatPat& p, u64 idx, int rep) {
    return p.w[(idx >> rep) % (u64)p.len];
}

struct Gen {
    int kind = 0; // 0 pattern, 1 hash, 2 address-in-address, 3 stressapptest-style pattern (sp, rep)
    const SatPat* sp = nullptr;
    int rep = 0;
    Line pat{}, inv{};
    u64 seed = 0;
    int altshift = -1;
    u64 modn = 0, modoff = 0; // modulo-N: every Nth line (offset) is inverted
    void set_beats(const Beats& b) {
        for (int j = 0; j < 8; j++) {
            pat.w[j] = (u64)b[2 * j] | ((u64)b[2 * j + 1] << 32);
            inv.w[j] = ~pat.w[j];
        }
    }
    inline void make(u64 g, const void* addr, bool invert, Line& o) const {
        if (kind == 0) {
            bool s = invert;
            if (altshift >= 0)
                s ^= (bool)((g >> altshift) & 1);
            if (modn)
                s ^= (g % modn) == modoff;
            o = s ? inv : pat;
        } else if (kind == 3) {
            for (int j = 0; j < 8; j++) {
                u64 v = (u64)sat_word(*sp, g * 16 + 2 * j, rep) | ((u64)sat_word(*sp, g * 16 + 2 * j + 1, rep) << 32);
                o.w[j] = invert ? ~v : v;
            }
        } else if (kind == 1) {
            for (int j = 0; j < 8; j++) {
                u64 v = mix64(((g << 3) + j) ^ seed);
                o.w[j] = invert ? ~v : v;
            }
        } else {
            for (int j = 0; j < 8; j++) {
                u64 v = ((u64)(uintptr_t)addr + 8 * j) ^ seed;
                o.w[j] = invert ? ~v : v;
            }
        }
    }
};
struct Var {
    std::string label;
    Gen g;
};

std::vector<Var> build_vars(const Params& P, u64 salt, const Logger& log); // salt: Run::salt

} // namespace ddr5
