// run.cpp - Run::report.
#include "run.h"
#include <cstring>
#include "lineio.h"

namespace ddr5 {

void Run::report(int tid, const u8* addr, const Line& e, const Line& a, const char* phase, bool reread) {
    const u32* eb = (const u32*)&e;
    const u32* ab = (const u32*)&a;
    int perblk[4] = {0, 0, 0, 0};
    u64 bits = 0;
    u32 lanemask = 0, beatmask = 0;
    for (int b = 0; b < 16; b++) {
        u32 d = eb[b] ^ ab[b];
        if (!d)
            continue;
        st.beat[b]++;
        beatmask |= 1u << b;
        lanemask |= d;
        for (int l = 0; l < 32; l++)
            if ((d >> l) & 1)
                st.lane[l]++;
        bits += pc32(d);
        for (int g = 0; g < 4; g++)
            perblk[g] += pc32((d >> (8 * g)) & 0xFFu);
    }
    // On-die ECC (128 data bits + 8 check bits per codeword) corrects 1 bit invisibly, so what reaches us is
    // mostly >=2 bits inside one codeword (or a miscorrection). On x8 DRAMs one codeword is ONE device's 8 DQ lanes
    // over all 16 beats, i.e. byte lane g of every 32-bit beat (perblk[g]). This ASSUMES x8 devices and an unswizzled
    // lane order: the memory controller's scrambling and board lane swaps cannot be seen from user mode, so treat the
    // grouping as a heuristic, not ground truth.
    for (int k = 0; k < 4; k++)
        if (perblk[k])
            st.blk[perblk[k] == 1 ? 0 : perblk[k] == 2 ? 1 : perblk[k] <= 4 ? 2 : 3]++;
    u64 nl = st.lines.fetch_add(1) + 1;
    st.bits += bits;
    errTotal++;
    double tnow = plat::now();
    {
        std::lock_guard<std::mutex> l2(mu);
        if (firstErrTest < 0)
            firstErrTest = tnow - tTestStart;
        if (firstErrRun < 0)
            firstErrRun = tnow - tStart;
    }
    {
        u64 off = (u64)(addr - region);
        for (int b = 6; b < 48; b++)
            if ((off >> b) & 1)
                st.abit[b]++;
    }
    const char* cls = reread ? "n/a" : "(logical check, not a memory line)";
    if (reread && nl < 20000) { // re-read from DRAM: separates read-path/signalling errors from stored errors
        _mm_clflush(addr);
        _mm_mfence();
        Line r;
        IO<32, 0, 0>::load(addr, r);
        bool eq_e = !memcmp(&r, &e, 64), eq_a = !memcmp(&r, &a, 64);
        if (eq_e) {
            st.trans++;
            cls = "TRANSIENT(re-read ok: read path/signalling)";
        } else if (eq_a) {
            st.stable++;
            cls = "STABLE(cell/write path/addressing)";
        } else {
            st.unstable++;
            cls = "UNSTABLE(changes every read)";
        }
    }
    if (cfg.stopOnErr)
        stop = true;
    std::lock_guard<std::mutex> l(mu);
    if (logged >= cfg.maxLog)
        return;
    logged++;
    char buf[1400];
    int o = 0;
    o += snprintf(buf + o, sizeof buf - o,
                  "ERROR t=%.1fs test=\"%s\" %s thr=%d addr=%p beats=0x%04X lanes=0x%08X bits=%llu "
                  "x8-codewords[1b/2b/3-4b/5+b]=%d/%d/%d/%d %s\n",
                  tnow - tStart, label.c_str(), phase, tid, (const void*)addr, beatmask, lanemask,
                  (unsigned long long)bits,
                  (int)(perblk[0] == 1) + (perblk[1] == 1) + (perblk[2] == 1) + (perblk[3] == 1),
                  (int)(perblk[0] == 2) + (perblk[1] == 2) + (perblk[2] == 2) + (perblk[3] == 2),
                  (int)(perblk[0] >= 3 && perblk[0] <= 4) + (perblk[1] >= 3 && perblk[1] <= 4) +
                      (perblk[2] >= 3 && perblk[2] <= 4) + (perblk[3] >= 3 && perblk[3] <= 4),
                  (int)(perblk[0] >= 5) + (perblk[1] >= 5) + (perblk[2] >= 5) + (perblk[3] >= 5), cls);
    for (int j = 0; j < 8; j++)
        if (e.w[j] != a.w[j])
            o +=
                snprintf(buf + o, sizeof buf - o, "      qword %d: exp=%016llX act=%016llX xor=%016llX\n", j,
                         (unsigned long long)e.w[j], (unsigned long long)a.w[j], (unsigned long long)(e.w[j] ^ a.w[j]));
    print_clear();
    emit(buf);
    if (logf) {
        fputs(buf, logf);
        fflush(logf);
    }
}

} // namespace ddr5
