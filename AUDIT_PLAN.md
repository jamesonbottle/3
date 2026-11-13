# DDR5 RAM Test: audit validation and work plan

Resume file for Claude sessions. Repo `jamesonbottle/3`, branch `jamesonbottle-patch-1`, validated at commit `f108410ebc4be95e7050bc9b4816b3c24424591c` on 2026-11-13.
Drop this file in the repo root (e.g. as `AUDIT_PLAN.md`) so every session sees it.

## How to resume (read this first)

1. Read this file, then pick the first unchecked item in the order given under **Suggested order**.
2. Before touching a hot path run the safety net: `bash tools/selftest.sh` (needs g++ 11+, AVX2, a few minutes) or at least `unit_tests` plus the injection checks.
3. When an item is done: tick its box, add one line to **Session log**, note any new finding. Do not delete items; mark dropped ones `[~]` with a reason.
4. Items marked **WIN** can only be verified on Windows/MSVC (CI or a real machine). A Linux sandbox cannot run them.

Paste-able prompt for a new session:
> Read AUDIT_PLAN.md in the repo root. Continue with the first unchecked item in "Suggested order". Run the baseline checks first, make the change, run tools/selftest.sh, then update the checkboxes and the session log.

Legend: `[ ]` open, `[x]` done, `[~]` dropped. Priority P0 (do first), P1, P2. Verdicts: CONFIRMED, CONFIRMED+MEASURED, PARTLY, REFUTED, UNMEASURED (mechanism confirmed in code, impact needs hardware).

## Validation method and limits

- Read the code of every file the audit cites (all of `src/`, `tools/selftest.sh`, CI, CMake, README) against the audit's claims. Windows API behaviour was checked against Microsoft's docs.
- Built the Linux engine (g++ 13.3, 0 warnings). `unit_tests` pass (1.9 s). The full `selftest.sh` was NOT run end to end; only targeted runs (see Evidence).
- Measurements are from a 2-vCPU KVM guest (Intel Xeon @ 2.1 GHz, 8 GB), not a DDR5 machine. Treat them as relative numbers. Nothing was run on Windows/MSVC.

## Validation summary

| Audit claim | Verdict | Note |
|---|---|---|
| Windows binary detection never verified | CONFIRMED | CI Windows job only builds + exit-0 smoke runs; unit tests, ctest and injection are Linux/GCC only. Injection hooks exist only for fill, pagemix, evolve, coherency. |
| Stop polls missing | CONFIRMED | pagemix final verify, copy fill/verify, hammer fill, startup memset, allocation/VirtualLock. GUI default has "Stop on error" ticked, so the pagemix wait after the first error is the default experience. |
| GUI Stop disabled until join; WM_CLOSE joins on UI thread; no GUI force-exit | CONFIRMED | `main_gui.cpp:287-295`, `:504-510`; only the CLI has the second-Ctrl-C exit (`platform_win.cpp:23-29`). |
| No sleep inhibition | CONFIRMED | No `SetThreadExecutionState` anywhere. MS docs: CPU activity is not detected as activity; the request is per calling thread. |
| Log write failures ignored | CONFIRMED | `run.cpp:99-102`, `main_gui.cpp:255-260`. CLI still prints errors to the console, so details are only lost in the GUI. |
| Errors show virtual address only | CONFIRMED | `run.cpp:85-87`; the region offset is computed at `run.cpp:49-54` but not printed. |
| Coverage is not address coverage | CONFIRMED (by design) | Documented in README. Matters only for `--coverage N` and the GUI "Coverage %" label. Address-in-address does verify each line 3x per job. Bitmap size checks out (64 GiB / 4 KiB = 16 Mi bits = 2 MiB). |
| `--cache` ignored for evolve | CONFIRMED | `suite.cpp:107` hard-codes fill/march/rwstorm; only evolve is missing (it is the only other algo with `flush`/`nt`). |
| No AVX2 runtime check | CONFIRMED | Whole build is `/arch:AVX2` (`CMakeLists.txt:31`), so the check itself must sit in a TU compiled without AVX2. |
| Low: single instance, `start_run` throw, `ncpu`, `pin` | CONFIRMED | `pin()` not wrapping is documented in `platform.h`; it only yields a warning. |
| `g_sink` shared atomic | CONFIRMED mechanism, UNMEASURED | Scope is narrower than stated: only the test "Disturb traffic between write and verify" (modes r, rw) uses disturb. No built-in march uses `X(...)`. Needs >=8 threads to show. |
| `sat_word` runtime `%` | CONFIRMED+MEASURED | 2.3 -> 4.9 GB/s with a mask at `--cache cached`; invisible at default flush=1 (see D1). |
| Evolve divisions | CONFIRMED, UNMEASURED | Also 8 `mix64` per line (`algo.h:85-89`), likely a bigger cost than the divisions. |
| `g % modn`, coherency `x % L` | CONFIRMED, low value | `modulo` is unused by the built-in suite; coherency is coherence-latency bound. |
| Chase does not reach its MLP | CONFIRMED+MEASURED | Flat ~120-190 ns/hop for K=2..32. Main cause is per-hop CLFLUSH, not only compute (see D4). |
| FPU stress saturates | CONFIRMED | Multiplier is exactly 1+2^-23; a lane reaches +inf after ~8.0e8 iterations (~1.5-2 s at ~8 cycles/iter; time is an estimate). |
| Memory sizing / headroom / no retry | CONFIRMED | 90% of `ullAvailPhys` (MS docs: includes standby+free+zero lists), capped at 97%; failure returns exit 2 / "Could not run". |
| Low: memcpy dst not flushed | CONFIRMED | Only the cache-sized tail of dst is affected. |
| Low: `probe` reads never-written memory | REFUTED | `prepare_region` memsets every chunk before any test (`suite.cpp:303`). `probe` is unused by the built-in suite anyway. |
| Barrier protocol implicit | CONFIRMED | Spot-checked fill, march, evolve, chase, copy, hammer, rwstorm, coherency, pagemix: no deadlock or false-positive path found. |
| Skeleton repeated; run.h mixed; aliasing; silent bad values; dead code | CONFIRMED | chase also hand-rolls its loops (audit listed pagemix/copy/hammer). Bad values only matter to developers editing `suite_data.cpp` (no config file). |
| Build hygiene (version, /WX, build.bat, .gitignore) | CONFIRMED | The /WX comment is aspirational ("once /W4 is clean"); CI does not enable it. `.gitignore` has `ddr5ramtest.log` but not the CLI default `ddr5test.log`. |

### New findings from this validation (not in the audit)

- **N1: CLFLUSH serialization dominates every default test (P0, measured on a VM only).** All tests inherit `flush=1` (`suite_data.cpp:54`) and flush every line with `_mm_clflush`. On this VM "Solid and checker" ran at 0.8 GB/s with CLFLUSH and 5.7 GB/s with CLFLUSHOPT. At flush=1 this also hides the audit's compute findings (pattern table sat at 0.8 GB/s, same as a plain pattern). See D1.
- **N2: the audit's page-template count is off.** Pagemix needs 34 (pattern x width) combinations with nonzero weight, x2 for inverted = 68 templates (~272 KB), not ~136 x 4 KB. Harmless either way.
- **N3 (found in A2, FIXED 2026-11-13): chase stopped a chain at its first bad line.** `bad[k] = 1` in `algo_chase.cpp` skipped the rest of that chain for the job, so a multi-fault chain reported one error and chase error counts were a lower bound. Now a bad line is reported and the chain carries on from the order's own next line (`pos[k]`), while the normal path still follows the loaded pointer (dependent load). The chase injection plants two faults in one chain (one of them a pointer flip) plus one in another chain, and requires all 3. Mutation check: restoring the old stop-at-first-error behaviour gives 2 of 3 and fails the check.
- **N4 (found in A2, FIXED 2026-11-13, one limit remains): the rwstorm concurrent verify had no deterministic injection.** The writer heals any planted memory fault and the partner's concurrent read finds it only by chance, so a memory fault cannot be planted for that path. New hook `rwstorm_read_fault` flips one bit of the line thread 1 *expects* (once, at line 1234 in the first pass), which is what a read error looks like to the compare; the check requires it as exactly one `rwstorm-read thr=1` error next to the two final-check errors on thr 0. Mutation check: verification off on thread 1 only gives 2 of 3 and fails. Remaining limit: the rwstorm injection check is still skipped under `SAN=thread`, because the writer and its verifying partner race by design (same as the clean runs); that cannot be removed without changing the test.

## Plan by category

### A. Detection proof and CI

- [ ] **A1 (P0, WIN)** Run `unit_tests` and the fault-injection checks on the MSVC build in CI.
  - Files: `build.bat` (builds only `ddr5ramtest` and `ddr5test_cli`), `.github/workflows/build.yml` (windows job), `CMakeLists.txt` (targets `unit_tests`, `ddr5test_cli_inject` and `add_test(unit_tests)` already exist on all platforms).
  - Do: build both extra targets on windows-latest, run `ctest -R unit_tests`, port selftest section 3 (fill 42 lines / 44 bits, page shuffle, coherency, evolve 2 lines, march 3 lines / 5 bits, chase 3 lines / 3 bits, copy 3 lines / 4 bits, rwstorm 3 lines / 4 bits, stop-on-error exit 1) to a script the runner can run (Git Bash is normally on the runner; otherwise a PowerShell port). The injection hooks read env vars via `getenv`.
  - Done when: CI goes red if a planted fault is not reported by the MSVC build.
- [x] **A2 (P0)** Extend fault injection to march, chase, copy and rwstorm. (Done 2026-11-13, see Session log and N3/N4.)
  - Files: `src/inject.cpp/.h` (existing hooks: `after_fill`, `evolve_last_pass`, `after_fill_throw`, `evolve_stop_in_write`, `pagemix_fill_stop`, `pagemix_flip`, `coherency_extra`), the four `algo_*.cpp`, `tools/selftest.sh` section 3.
  - Do: add a hook between write and verify in each algo (chase: flip a byte of a pointer line; copy: corrupt dst after the copy), add checks with exact expected counts.
  - Done when: each of the four has a "planted fault found" check, and temporarily disabling that algo's verify makes the check fail (mutation check). Land this before the D items so perf rewrites are protected.

### B. Stop and shutdown responsiveness

- [ ] **B1 (P0)** (**Non-GUI parts done 2026-11-13; stays open for the Windows allocation / VirtualLock slicing**, see Session log.) Make every long loop stoppable, with explicit semantics for the pagemix final verify.
  - Gaps: pagemix final verify (`algo_pagemix.cpp:204-211`), copy fill (`algo_copy.cpp:57-64`) and verify (`:85-95`), hammer fill (`algo_hammer.cpp:60-67`) and its `acts` loop (`:82-88`), startup memset (`suite.cpp:303`), allocation/VirtualLock (`platform_win.cpp:97-125`; one call cannot be interrupted, so slice the first touch instead).
  - Design decision needed: the pagemix final verify deliberately runs after Stop. Skipping checks is always false-positive safe, but the selftest "stopped mid-fill reports only the planted flip" relies on it running. Suggested: two levels. Soft stop completes the verify with visible progress ("Verifying... N%"); hard abort (second Stop click, window close, second Ctrl-C) interrupts it. Update the selftest expectations accordingly.
  - Done when: a new selftest sends SIGINT at several moments in every test and requires exit within a few seconds (no 124 timeouts).
- [ ] **B2 (P0, WIN)** GUI close and stop.
  - Files: `main_gui.cpp:504-510` (WM_CLOSE joins on the UI thread), `:287-295` (Stop button disabled until join).
  - Do: set stop and wait for `WM_RUN_DONE` before destroying; keep the window responsive; add a force-exit path (second click or timeout) like the CLI's second Ctrl-C.
  - Done when (manual checklist): closing during allocation, mid-test and during the final verify never shows "Not responding".

### C. Soak reliability and resources

- [ ] **C1 (P0, WIN)** Sleep inhibition. Add `plat::keep_awake(bool)`: Windows `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)`, POSIX no-op. Call it from the long-lived run thread (the state is per calling thread) at the start of `SuiteDriver::run` with a `ScopeExit` that clears it. Decide whether the display may sleep. Log one line when active. Done when: a run survives a short power-plan sleep timeout, and normal idle behaviour returns afterwards.
- [x] **C2 (P0)** (Done 2026-11-13, CLI tested, GUI part untested; see Session log.) Log write failures. Check `fputs`/`fflush` results (`run.cpp:99-102`, `main_gui.cpp:255-260`), set an atomic "log failed" flag, show it once in the console/status line, in the final summary and in the GUI end text (`main_gui.cpp:313-317` currently keys off the open-time `logWhere` only). Keep counting errors. Done when: a test with a full disk (or `/dev/full`) shows the warning and still exits 1.
- [ ] **C3 (P1, WIN)** Memory headroom and allocation retry. Today: auto size = 90% of available (`suite.cpp:244`, `main_gui.cpp:183`), cap 97%, working-set min = size + 256 MB (`platform_win.cpp:113`), failure exits 2. Do: keep a fixed floor for OS/GUI, retry at smaller sizes with a log line, consider the commit limit. Done when: constrained-memory runs degrade gracefully instead of "Could not run".
- [x] **C4 (P1)** (Done 2026-11-13, see Session log.) Print the region offset next to the virtual address in error lines (`run.cpp:85-87`; the offset is already computed at `:50`). Extend the selftest seed-reproduction check to compare offsets for the same seed.
- [ ] **C5 (P2, WIN)** Single-instance guard (named mutex in `WinMain`; second copy activates the first or shows a message).

### D. Stressor throughput (find the real limiter first)

Order matters: D1 first, because at flush=1 it masks everything below.

- [ ] **D1 (P0, measure-first)** CLFLUSH serialization (finding N1). Hot-path flushes: `lineio.h:41-42` and `:66-67`, `algo_chase.cpp` chain loop, `algo.cpp:44`, `algo_pagemix.cpp:79`, hammer.
  - Do: detect CLFLUSHOPT at runtime (cpuid leaf 7, EBX bit 23), make the `FL=1` path use it when present with CLFLUSH as fallback, keep an option to force plain CLFLUSH for comparison. CLFLUSHOPT has weaker ordering than CLFLUSH: the code already issues `_mm_sfence()` before barriers; re-check every phase boundary and the re-read in `Run::report` (`run.cpp:56-61`).
  - Evidence so far: see Evidence table. Planted-fault check still found 42 lines / 44 bits with CLFLUSHOPT.
  - Open risk: CLFLUSH cost is microarchitecture- and VM-dependent. Benchmark both variants on the real DDR5 targets (Zen 4/5, Raptor Lake) before making it the default. Optional self-check: time a post-flush read (like `algo_probe`) to prove the next read really goes to DRAM.
- [ ] **D2 (P1)** `sat_word` runtime `%` (`gen.h:27-29`): all table lengths are 1, 2, 4 or 64, so use `& (len-1)` (assert power of two in `sat_table()`, `gen.cpp:12-24`) or precomputed page templates. Pagemix lines depend only on the in-page line index (68 templates, see N2); fill with `data=sat` repeats every <=32 lines. Measured 2.1x at `--cache cached`.
- [ ] **D3 (P1, after D1)** Evolve: `evo_period` does `% maxp` (`algo.h:82-84`), the loops do `p % per` / `p / per` (`algo_evolve.cpp:87-109`), plus 8 `mix64` per line. Cache the per-line period (1 byte/line) or precompute per pass; measure before and after.
- [ ] **D4 (P1, after D1)** Pointer chase. Batching alone (all K loads, then flush, then verify, as the audit proposes) did NOT help with CLFLUSH. Batching plus CLFLUSHOPT scaled ns/hop from 167 to ~38 (K=1 to 32), leaving a compute floor of ~38 ns/hop (6 splitmix + memcmp per hop). Do D1 first, then restructure the loop; optional second step: cheaper link check without losing full-line coverage.
- [x] **D5 (P1)** (Done 2026-11-13; the benefit is still UNMEASURED, needs >=8 threads, see Session log.) `g_sink` (`algo.cpp:34,45`): per-thread accumulator, publish once per call. Trivial change; measure on >=8 threads with the disturb test.
- [ ] **D6 (P1)** FPU stress (`suite.cpp:45-59`): lanes overflow to +inf in ~1.5-2 s. Renormalise (e.g. re-seed lanes every few thousand outer loops, or alternate `m` and `1/m`). Threads are below-normal priority and unpinned and compete with T pinned workers (`:162`): decide priority/pinning. Done when: a unit test runs the kernel long enough and asserts values stay finite and keep changing.
- [ ] **D7 (P2)** Low-value: coherency `x % L` (`algo_coherency.cpp:58`), `g % modn` (`gen.h:52`, no built-in test uses `modulo`). Only if profiling shows it.

### E. Option semantics, metrics, startup checks

- [x] **E1 (P1)** `--cache` / GUI "CPU cache" for evolve: replace the hard-coded list at `suite.cpp:107` with `algo_keys(algo).count("flush")` / `"nt"`. Add a unit test that every algo with those keys receives the override. (Done 2026-11-13: `select_tests` now keys off `algo_keys()` and is exposed in `suite.h`; new `test_cache_override` in `tools/unit_tests.cpp`.)
- [ ] **E2 (P1)** Coverage semantics. Decide: relabel "Coverage" in the GUI as passes, or make `--coverage N` mean unique address coverage (per-page bitmap, 2 MiB per 64 GiB, or mark whole windows at job end since windows are deterministic via `Run::view`). Done when: `--coverage 100` on a region larger than the biggest window stops only after every page was verified once.
- [ ] **E3 (P1, WIN)** AVX2 runtime check before any AVX2 code runs: `IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)` or cpuid leaf 7 + `xgetbv` for OS YMM state. Put it in a TU compiled without `/arch:AVX2` (per-source compile options in CMake). Message box (GUI) / stderr (CLI), exit 2. Linux analogue: `__builtin_cpu_supports("avx2")`.
- [ ] **E4 (P2)** Validate currently silent bad values and add them to `bad_values()` (`algo.cpp:137-178`) with unit tests: copy `method` typo falls through to non-temporal (`algo_copy.cpp:70-80`); `width` outside 8/16/32 becomes 32 (`lineio.h:97-102`); unknown march direction becomes "up" (`algo_march.cpp:205`); `victims >= 32` never matches (`gen.cpp:123-128`); `altshift >= 64` is an undefined shift (`gen.h:50`). Developer-facing only.

### F. Windows platform parity

- [ ] **F1 (P2, WIN)** `ncpu()` should honour process affinity and job limits (`platform_win.cpp:34-37`, contract in `platform.h:12`); decide whether `pin()` should wrap like Linux (`:40-61`) or keep the documented false + warning.
- [ ] **F2 (P2, WIN)** `start_run` exception safety (`main_gui.cpp:205-285`): `A.busy` and the UI are set before the thread is created (`:277-281`); wrap thread/`make_unique` creation in try/catch and restore state.

### G. Maintainability and hygiene

- [x] **G1 (P1)** (Done 2026-11-13, see Session log.) Document the barrier invariants and add a debug assertion: every thread runs the same `R.sync()` sequence per job; early exit only via `arrive_and_drop` (`run.h:272`); `walk()` polls every 65,536 iterations (`run.h:346-355`); "no false errors after stop" relies on stop being re-read after each barrier. Debug-only per-thread sync counter compared at the end of `parallel()`; enable in the sanitizer legs.
- [ ] **G2 (P2)** Share the job skeleton (jobs list, `winctr`, `parallel`, stop checks, sync) and move pagemix, copy, hammer and chase onto `walk`.
- [ ] **G3 (P2)** Split `run.h` (data model / orchestration / status printing) and break `Run::report` (`run.cpp:8-103`) into classify, format, write.
- [x] **G4 (P2)** `Run::report` aliasing: replace `(const u32*)&e` (`run.cpp:12-13`) with `memcpy` into `u32[16]`. (Done 2026-11-13, with a `static_assert(sizeof(Line) == kLineBytes)`.)
- [ ] **G5 (P2)** `hammer` and `probe` are unused by the built-in suite: either keep them as documented extras with tests or remove them.
- [ ] **G6 (P2)** Build hygiene: (a) version string via CMake + `git describe`, shown in `VERSIONINFO`, the CLI banner and both log headers (`suite.cpp:305`, `main_gui.cpp:248`); (b) enable `-DDDR5_WARNINGS_AS_ERRORS=ON` in CI once clean, or fix the comment at `CMakeLists.txt:17`; (c) `build.bat:6` swallows the first configure error: only fall back when Ninja is missing, and also build `unit_tests`; (d) add `ddr5test.log` (or `*.log`) to `.gitignore` (**d done 2026-11-13**: `ddr5test.log` added; a-c still open, so G6 stays unchecked).
- [ ] **G7 (P2)** memcpy destination in `algo_copy.cpp:70-71` is not flushed before verify; flush it after the copy (affects only the cache-sized tail).

## Suggested order

1. **Session 1 (Linux, fully testable): safety net.** A2, E1, G4, G6d, C4. Everything later is protected by A2's new injection checks.
2. **Session 2 (Linux): stop/log/soak logic.** B1 (non-GUI parts), C2, G1, D5, D6.
3. **Session 3 (Linux, measurement-driven): throughput.** D1 -> D2 -> D3 -> D4. Re-run selftest after each step; get target-hardware numbers for D1 before changing the default.
4. **Session 4 (needs Windows / CI): platform items.** A1, B2, C1, C3, C5, E3, F1, F2, G6a-c.
5. **Later (P2):** E2 decision, E4, G2, G3, G5, G7, D7.

## Open decisions

1. ~~Pagemix final verify after Stop: soft stop with progress vs hard abort (B1).~~ **Decided 2026-11-13: hard stop** (Stop, Ctrl-C, window close and the time/coverage limits end the verify at the next page; unverified pages are skipped, which cannot produce a false error). No second level.
2. Coverage: relabel or implement address coverage (E2).
3. CLFLUSHOPT as default or as an option (D1); needs real-hardware numbers.
4. Sleep inhibition: allow the display to sleep? (C1)

## Dropped / downgraded

- `[~]` Audit Low item "probe reads never-written memory": refuted (see summary).
- Downgraded: `g_sink` scope (one built-in test), `g % modn` (unused), coherency `%` (not DRAM bound), silent bad values (developer-facing only).

## Evidence (reproduce with these)

Environment: KVM guest, Intel Xeon @ 2.1 GHz, 2 vCPU, 8 GB, g++ 13.3, CMake 3.28, Ninja. All runs `-t 2 --no-large-pages`.

```
cmake -S . -B $B/cmake -G Ninja -DDDR5_OUTPUT_DIR=$B/out
cmake --build $B/cmake --target unit_tests ddr5test_cli ddr5test_cli_inject
$B/out/unit_tests                                   # baseline: all checks passed
$B/out/ddr5test_cli -mb 128 -t 2 --no-large-pages -f "Solid and checker"   # add --cache cached for the no-flush runs
```

| Run | Result |
|---|---|
| "Solid and checker", default (flush=1, CLFLUSH) | 0.8 GB/s, 5.5 s |
| "Stressapptest-style patterns", default | 0.8 GB/s, 24.3 s |
| "Solid and checker", `--cache cached` | 8.6 GB/s, 0.5 s |
| "Stressapptest-style patterns", `--cache cached` | 2.3 GB/s, 8.0 s |
| same, `sat_word` mask patch (`gen.h:28`: `& (u64)(p.len - 1)`) | 4.9 GB/s, 3.8 s |
| "Solid and checker", default, CLFLUSHOPT build | 5.7 GB/s, 0.8 s |
| Planted-fault run (`ddr5test_cli_inject`, "Solid and checker"), CLFLUSHOPT build | exit 1, 42 lines / 44 bits (expected) |
| FPU kernel, `x=x*1.0000001f+1e-7f` | `m` = 1.0000001192; lane overflows after ~8.02e8 iterations (~40,100 outer loops of 20,000) |

Chase, `-mb 1024`, ns per hop (thread 0), K = 1, 2, 4, 8, 16, 32:

| Variant | ns/hop |
|---|---|
| Original loop | ~167, 134-141, 140, 135-139, 122-134, 131-136 (two seeds) |
| Loads batched first, CLFLUSH | ~173, 143-147, 180-192, 148-175, 144-153, 134-149 |
| Loads batched first, CLFLUSHOPT | 167, 105, 76, 50, 39, 38 |

Temporary patches used (scratch copy only, not committed): per-job timer `tc0` plus a `[probe]` print around the chain loop in `algo_chase.cpp`; for CLFLUSHOPT replace `_mm_clflush(` with `_mm_clflushopt(` in `lineio.h` and `algo_chase.cpp`, cast `(void*)a` in `IO::verify`, configure with `-DCMAKE_CXX_FLAGS=-mclflushopt`.

## Session log

| Date | Session | What was done |
|---|---|---|
| 2026-11-13 | Validation | Validated every audit claim against `f108410`, measured items D1/D2/D4/D6 on a Linux VM, wrote this plan. No source changes made. |
| 2026-11-13 | Session 1: A2 | Baseline green (`unit_tests` pass, build 0 warnings). Added hooks `march_after_element`, `chase_after_fill`, `copy_after_copy`, `rwstorm_before_final` (`inject.cpp/.h`, four `algo_*.cpp`) and four exact-count checks in `selftest.sh` section 3 (march 3 lines / 5 bits, chase 2 lines / 2 bits, copy 3 lines / 4 bits, rwstorm 2 lines / 3 bits). Full `tools/selftest.sh`: 64 PASS, 0 FAIL, 0 SKIP (Linux VM, g++ 13). Mutation check on a scratch copy: disabling each algo's verify made its check fail (march, chase, copy, rwstorm; copy also with only the src or only the dst side off). New findings N3, N4. Next in Session 1: E1, G4, G6d, C4. |
| 2026-11-13 | Session 1: A2 follow-up | Fixed N3 and N4. Chase now continues past a bad line via the order's next line (check: 3 lines / 3 bits, two in one chain). New hook `rwstorm_read_fault` covers the concurrent verify (check: 3 lines / 4 bits, 2 on thr 0 + 1 on thr 1). Full `selftest.sh`: 64 PASS, 0 FAIL, 0 SKIP; `format.sh --check` clean. Mutants caught: chase verify off, chase old stop-at-first-error (2 of 3), rwstorm verify off, rwstorm thread-1-only verify off (2 of 3). rwstorm injection still skipped under `SAN=thread` (by design). |
| 2026-11-13 | Session 1: E1 | Baseline green (`unit_tests` pass, 0 build warnings). `--cache` now sets `flush` / `nt` for every algo that lists the key in `algo_keys()`, so evolve gets it (before: only fill, march, rwstorm). `select_tests` made non-static and declared in `suite.h` for testing. New unit check `test_cache_override`: for bypass / cached / nt every built-in test of fill, march, rwstorm and evolve gets the right values, algos without `nt` do not gain it, and `Default` leaves all 28 tests' parameters unchanged. Mutation check: excluding evolve from the override (old behaviour) makes the check fail. Full `tools/selftest.sh`: 64 PASS, 0 FAIL, 0 SKIP; `format.sh --check` clean. Note: the GUI "CPU cache" list uses the same `Settings::cacheMode`, so it is fixed too; the GUI itself was not built (needs Windows). Next in Session 1: G4, G6d, C4. |
| 2026-11-13 | Session 1: G4 | Baseline green at the E1 commit. `Run::report` now copies both lines into `u32[16]` with `memcpy` (plus `static_assert(sizeof(Line) == kLineBytes)`) instead of casting `Line*` to `u32*`; beat / lane / bit counts are unchanged (same little-endian word order). Build 0 warnings, `unit_tests` pass, `format.sh --check` clean, full `tools/selftest.sh`: 64 PASS, 0 FAIL, 0 SKIP (the injection checks assert exact line / bit counts, which go through this function). No dedicated unit test: the change is behaviour-preserving and the injection checks cover it. Next in Session 1: G6d, C4. |
| 2026-11-13 | Session 1: G6d | Baseline green at the G4 commit (`unit_tests` pass, 0 build warnings). Added `ddr5test.log` (the CLI/GUI default `logFile` in `params.h:86` / `suite.h:34`) to `.gitignore`; `git check-ignore` confirms it matches. Config-only change, no source touched. Full `tools/selftest.sh`: 64 PASS, 0 FAIL, 0 SKIP. G6 as a whole stays open (a-c). Next in Session 1: C4. |
| 2026-11-13 | Session 1: C4 | Baseline green at the G6d commit (`unit_tests` pass, 0 build warnings). Error lines now read `addr=0x... off=0x...`: `Run::report` hoists the region offset (already computed for the address-bit stats) and prints it; README updated. Selftest seed-reproduction block now also checks that every ERROR line has an `off=`, that offsets are 64-byte aligned and inside the 64 MiB region, and that two runs with the same seed print identical offsets. Full `tools/selftest.sh`: 67 PASS, 0 FAIL, 0 SKIP (was 64); `format.sh --check` clean. Mutation check on a scratch copy (`off` = raw virtual address): the range and same-seed checks fail. Session 1 is complete; next is Session 2 (B1 non-GUI parts, C2, G1, D5, D6). |
| 2026-11-13 | Session 2: C2 | Baseline green at the C4 commit (`unit_tests` pass, 0 build warnings). New `Run::write_error_log` (used by `report()` and the run header in `prepare_region`) checks `fputs`/`fflush`, sets `ErrorSink::logFailed` and prints ONE console warning with the errno text; `print_final_summary` repeats it. Errors keep being counted, exit code stays 1. GUI (`main_gui.cpp`, NOT built here, needs Windows): the log sink and the header write set `A.logFailed`; `on_run_done` shows 'Errors found - the log file could not be written completely (disk full?)'. Not done for the GUI: no warning while the run is still going (only the end text). Selftest: 4 new checks (no warning on a healthy log; `/dev/full` + injected fill fault: exit 1, 42 lines / 44 bits, exactly one warning plus the summary line; clean run on `/dev/full`: one warning, exit 0; SKIP if `/dev/full` is not writable). Full `tools/selftest.sh`: 71 PASS, 0 FAIL, 0 SKIP; `format.sh --check` clean. Mutation check (ignore write failures): the two warning checks fail. Next in Session 2: B1 (needs the pagemix decision), G1, D5, D6. |
| 2026-11-13 | Session 2: B1 (non-GUI) | Baseline green at the C2 commit. Decision: hard stop (see Open decisions). Stop checks added: pagemix final verify (per page), copy fill and verify (every 65,536 lines), hammer fill and its `acts` loop (every 16,384 iterations), startup memset (64 MB slices, `suite.cpp`); the `filled[]` guard in the pagemix final verify is now only defensive. README updated. New hooks `pagemix_final_stop` (late flip in the last page, optional stop) and `copy_fill_stop`. Selftest: the old 'stopped mid-fill reports only the planted flip' became 'exit 0, nothing reported' (the final verify is skipped on stop); +control (late flip found when the verify runs to the end, `off=0x3FFF...`) and +stop-in-final (late flip not found, nothing false); +copy stopped mid-fill (exit 0, STOP line); new section 4: SIGINT at 0.1 / 0.5 / 1.2 / 2.5 s in each of the 28 tests alone on 128 MiB, requiring exit within `STOPWAIT` (5 s, x3 under ASan) with rc 0 and no ERROR line (skipped under TSan). Full `tools/selftest.sh`: 103 PASS, 0 FAIL, 0 SKIP; `format.sh --check` clean. Mutation checks on scratch copies: pagemix final verify without its stop check -> the late flip is reported, stop-in-final check fails; copy verify/copy not guarded by stop -> rc 1 with 2 errors, copy-stop check fails. **Limits:** (1) the SIGINT sweep proves the end-to-end behaviour (prompt exit, rc 0, no false error) but cannot show a single missing check in a loop that takes <1 s at this size: loops are bounded by the test window, so only the region-sized loops (pagemix final verify, startup memset) matter and the first is covered by the hook checks; the memset slicing itself is untested (needs a multi-GB region). (2) `hammer` is not in the built-in suite, so its new checks have no test (G5). (3) Not done: allocation / `VirtualLock` first-touch slicing in `platform_win.cpp` (Windows only), so B1 stays open. Next in Session 2: G1, D5, D6. |
| 2026-11-13 | Session 2: G1 | Baseline green at the B1 commit (`unit_tests` pass, 0 build warnings). Barrier protocol written up in `run.h` (four rules: same sync sequence while not stopped, leaving only by return + `arrive_and_drop`, re-read `stop` after the barrier that follows a cut-short walk, polling intervals). Debug check `DDR5_CHECK_SYNC`: each worker counts its `sync()` calls (`t_syncs`), `parallel()` compares the counts after the join and prints the worker numbers + `std::abort()`s on a mismatch. **The check applies only if `stop` is still false when `parallel()` ends and no worker failed**: after a Stop the top-of-job `if (R.stop) break;` is read by each worker alone, so workers may legitimately leave at different points and make different numbers of calls (rule 2); `stop` is never cleared, so "false at the end" means "false throughout". On when asserts are on (no `NDEBUG`) or via the new CMake option `DDR5_CHECK_SYNC` (off by default; `selftest.sh` turns it on in every leg, so the ASan/UBSan and TSan CI legs have it). New hook `extra_sync` (`DDR5TEST_EXTRA_SYNC`: worker 0 makes one extra `sync()` at the end of the fill job; cannot hang, the others have already left). Tests: unit `test_sync_protocol` (the mismatch helper; `parallel()` with equal counts; `parallel()` with stop-divergent counts must not be reported) and one selftest check (extra sync: dies by SIGABRT with the message). Full `tools/selftest.sh`: 104 PASS, 0 FAIL, 0 SKIP (was 103); `format.sh --check` clean. No existing algo needed a change: all 28 clean runs and the SIGINT sweep pass with the check on. Mutation checks on a scratch copy: removing the `abort()` makes the new check fail (my first version of the check only looked for rc != 0 and the message, and the mutant passed it because the planted fill faults give exit 1 as well; now it requires rc >= 128). **Limits:** (1) the check proves equal counts, not that the sync points line up: a worker that makes a different *kind* of call sequence with the same count is not caught. (2) Counts are only compared at the end of a `parallel()`, so a mismatch shows up there and not at the sync where it happened. (3) The full `SAN=` selftest legs were not run here (they take much longer); only `unit_tests` was built and run with `DDR5_SANITIZE=address,undefined` and with `thread` (both with the check on): all checks passed, no report. Next in Session 2: D5, D6. |
| 2026-11-13 | Session 2: D5 | Baseline green at the G1 commit (`unit_tests` pass, 0 build warnings). `disturb_traffic` (`algo.cpp`) now xors what it reads into a local `acc` and publishes it once per call with `g_sink.fetch_xor(acc, relaxed)`, instead of a relaxed load + store of the shared atomic for every line read (so no shared write per line; the loads cannot be optimised away because `acc` still reaches the atomic, and the xor is present in the binary). Semantics of the sink are unchanged (it is only a dummy consumer). Measured on the 2-vCPU VM, `-mb 256 -t 2`, test "Disturb traffic...": 1.1 GB/s before and after (3 runs each, identical to the printed precision), which is expected: at flush=1 CLFLUSH dominates (D1) and 2 threads do not contend. The >=8-thread effect this item is about could NOT be measured here, so the gain is unverified. Full `tools/selftest.sh`: 104 PASS, 0 FAIL, 0 SKIP; `format.sh --check` clean. No new test: the change is behaviour-preserving and the disturb test (modes r, rw) runs in the stop sweep and clean runs. No mutation check (nothing to detect). Next in Session 2: D6. |
