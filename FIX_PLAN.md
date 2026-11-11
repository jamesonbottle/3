# Audit fix plan

Source: audit of the `cleanup` branch (maintainability/readability, performance/resource management, reliability/error handling). The audit was a static read; nothing below has been built or run yet, so every "verify" step is still to do.

## How to resume (read this first)

- Each section (S1..S12) is self-contained: goal, files, steps, done-when. Pick the first section whose status is not `DONE`.
- Update the **Status board** and the section's **Status** line as you go, and add one line to the **Progress log**. Commit the plan changes together with the code.
- One section = one commit (or PR). Do not mix sections.
- Gate for every section before marking DONE: `bash tools/format.sh --check`, `bash tools/selftest.sh` (Linux, g++ 11+ with AVX2, takes a few minutes). On Windows also run `build.bat`.
- If a section turns out to be wrong or already fixed, mark it `SKIPPED` and say why in the log.
- Line numbers below are from the audit and will drift; search by the quoted code instead.

### Suggested order

S3 -> S2 -> S1 -> S4 -> S5 -> S6 -> S7 -> S8 -> S9 -> S10 -> S11 -> S12

Dependencies: S9 (split `run_suite`) should come after S1 (RAII), because the guards make the split trivial. S9 should also come after S8 (enums), so the extracted functions use the new types. S12 (sanitizers) is best last, since it will flag S6 if S6 is not done yet.

## Status board

| ID  | Area                         | Category    | Priority | Status |
|-----|------------------------------|-------------|----------|--------|
| S1  | RAII + worker exceptions     | Reliability | High     | TODO   |
| S2  | CLI log open failure         | Reliability | High     | TODO   |
| S3  | Progress line error count    | Reliability | High     | TODO   |
| S4  | `report()` hardening         | Rel + Perf  | Medium   | TODO   |
| S5  | Input value validation       | Reliability | Medium   | TODO   |
| S6  | `status()` race + Ctrl-C     | Reliability | Medium   | TODO   |
| S7  | Platform robustness          | Reliability | Medium   | TODO   |
| S8  | Enums and named constants    | Readability | Medium   | TODO   |
| S9  | Split `run_suite`            | Readability | Medium   | TODO   |
| S10 | Remove duplication           | Readability | Low      | TODO   |
| S11 | Stale docs and comments      | Readability | Low      | TODO   |
| S12 | CI: sanitizers, `-Werror`    | Tooling     | Medium   | TODO   |

## Progress log

| Date | Section | Note |
|------|---------|------|
| -    | -       | Plan created from audit |

---

## S1. RAII and worker exception safety

**Status:** TODO

**Problem.** `run_suite`'s `catch` (end of `src/suite.cpp`) returns 3 without releasing the region, closing `errors.logf`, or joining the FPU threads. A joinable `std::thread` destroyed during unwinding calls `std::terminate`. Separately, the worker lambda in `Run::parallel` (`src/run.h`) has no try/catch, so any throw in a worker (e.g. `std::vector` allocation in `algo_chase`) terminates the process and bypasses the documented exit code 3.

**Files:** `src/suite.cpp`, `src/run.h`, possibly `src/platform.h`.

**Steps**
1. Add small guards in `suite.cpp` (or a new `src/raii.h`): `RegionGuard` (calls `plat::release` and nulls `R.layout.region`), `LogFileGuard` (closes `errors.logf`), `FpuGuard` (sets `fpuStop`, joins threads; `std::jthread` is an option).
2. Replace the manual cleanup at the end of `run_suite` with the guards, so the success path and the `catch` path share one cleanup.
3. In `Run::parallel`, wrap `fn(t)` in try/catch, store the first `std::exception_ptr` (guarded by a mutex or `std::atomic_flag`), set `stop`, still call `vflush()` and `arrive_and_drop()`, and rethrow on the calling thread after `join()`.
4. Make sure the final summary and `return` codes are unchanged for non-exception runs.

**Done when**
- A forced `throw` inside a worker (temporary test hook or unit test) yields exit code 3 and a logged `internal error`, with no `terminate`.
- Region is released on the exception path (check with a small test that `R.layout.region == nullptr` afterwards).
- `selftest.sh` passes.

---

## S2. CLI: log file open failure is silent

**Status:** TODO

**Problem.** `src/suite.cpp`: `R.errors.logf = R.log.sink ? nullptr : fopen(cfg.logFile.c_str(), "a");` and nothing happens when `fopen` returns null. `--log /bad/path` runs normally and the persistent error log never exists. The GUI already handles this (temp-folder fallback plus status message).

**Files:** `src/suite.cpp` (and optionally `src/main_cli.cpp`).

**Steps**
1. If `!R.log.sink` and `fopen` fails, emit a clear warning with the path and `strerror(errno)`.
2. Decide the policy and document it in the README: warn and continue (errors still print to the console), or exit 2 when the user passed `--log` explicitly and it cannot be opened. Recommended: exit 2 for an explicit `--log`, warn for the default file name.
3. Add a selftest case: `--log /nonexistent/dir/x.log` -> expected behavior per the chosen policy.

**Done when:** the new selftest case passes and the README mentions the behavior.

---

## S3. Progress line shows per-test error count

**Status:** TODO

**Problem.** `Run::parallel` in `src/run.h` prints `st.lines`, which `run_suite` resets every test (`R.st.clear()`), instead of the run total. In keep-going mode earlier failures vanish from the live line.

**Files:** `src/run.h`.

**Steps**
1. Print `errors.errTotal.load()` (run total) in the `errors:` field. Optionally also show the per-test count, e.g. `errors: 12 (this test 3)`.
2. Check `tools/selftest.sh` and `tools/unit_tests.cpp` for anything that greps this progress format and update it.

**Done when:** a keep-going run with injected faults keeps showing a non-zero error count in later tests; `selftest.sh` passes.

---

## S4. Harden `Run::report` (buffer math, lock contention, counters)

**Status:** TODO

**Problem**
- `o += snprintf(buf + o, sizeof buf - o, ...)` wraps if the output ever exceeds 1400 bytes (`snprintf` returns the would-be length). Safe today, fragile.
- `Logger::emitf` silently truncates at 2048 bytes.
- `report()` takes `errors.mu` twice per error, even after `cfg.maxLog` is reached, and holds it during log emit and `fflush`. In keep-going mode on a badly failing DIMM, threads serialize here.
- Shared `std::atomic` arrays (`lane`, `beat`, `abit`) are incremented from every thread, which causes false sharing during error storms.

**Files:** `src/run.cpp`, `src/run.h` (`Stats`, `ErrorSink`), `src/log.cpp`.

**Steps**
1. Replace the manual offset arithmetic with a clamped helper (e.g. `append(buf, o, fmt, ...)` that never lets `o` exceed `sizeof buf - 1`), or build the message in a `std::string`.
2. In `Logger::emitf`, detect truncation (`vsnprintf` return >= buffer size) and either grow the buffer or mark the output `...`.
3. Make the "first error" bookkeeping lock-free (e.g. `std::atomic<double>` or an `std::atomic_flag` that is set once), and return early when `errors.logged >= cfg.maxLog` before doing any formatting or locking beyond one atomic increment.
4. Optional: per-thread `Stats` merged when the test ends, or `alignas(64)` padding on the hot arrays. Only do this if it measurably helps.
5. Add a unit test that feeds a worst-case line (every qword differs, long label) and asserts the output is well formed and not truncated mid-line.

**Done when:** the unit test passes and `selftest.sh` fault-injection counts are unchanged (42 lines / 44 bits in the pattern test).

---

## S5. Validate values in test data, not only keys

**Status:** TODO

**Problem.** `pnum()` (`src/params.cpp`) uses `strtoull` with no error check: `"4KB"` becomes 4, junk becomes 0. `ospec()` (`src/algo.cpp`) logs `bad order` without the test name or the offending value, then continues with a default. The CLI is strict about this; the compiled-in suite is not.

**Files:** `src/params.cpp`, `src/params.h`, `src/algo.cpp`, `tools/unit_tests.cpp`, `tools/selftest.sh`.

**Steps**
1. Add a checked parser (e.g. `bool pnum_checked(const std::string&, u64&)`) that rejects trailing garbage, empty input and overflow. Keep `pnum()` as a thin wrapper if other callers depend on it.
2. Make `Params::num/nums` report bad values through the logger or a validation pass, and have the suite validation step (next to `unknown_keys`) fail the run (exit 2) or warn loudly on a bad value.
3. Include the test name and the offending value in the `ospec` message.
4. Unit tests: `"4KB"`, `"abc"`, `"99999999999999999999"`, `"0x"`, plus the valid forms `"4K"`, `"2M"`, `"0x10"`.

**Done when:** a deliberately bad value in `suite_data.cpp` is caught by the unit tests or selftest, and the valid forms still parse identically.

---

## S6. `Run::status()` data race and Ctrl-C behavior

**Status:** TODO

**Problem**
- The GUI thread reads `layout.regionBytes` and `progress.tStart` (plain fields) while `run_suite` writes them from the worker thread. Benign on x86, formally UB.
- The Ctrl-C handler only sets `stop`; a second Ctrl-C does nothing, so a wedged run can only be killed externally.

**Files:** `src/run.h`, `src/suite.cpp`, `src/platform_posix.cpp`, `src/platform_win.cpp`.

**Steps**
1. Make the fields read by `status()` atomic (`std::atomic<u64>`, `std::atomic<double>` or an equivalent), with release stores in `run_suite` and acquire loads in `status()`. Audit every other cross-thread read of plain `Run` fields while here.
2. In both platform handlers, count signals: first sets `stop`, second restores the default handler and re-raises (POSIX) or calls `ExitProcess(130)` (Windows). Keep the handler async-signal-safe (atomics only).
3. Document the behavior in `--help` and the README.

**Done when:** TSan (see S12) no longer reports the `status()` race; double Ctrl-C terminates a hung run on Linux (manual test with a long `--test-seconds`).

---

## S7. Platform robustness

**Status:** TODO

**Problem**
- POSIX `pin()` uses `cpu % CPU_SETSIZE` and ignores the return value; `ncpu()` uses `hardware_concurrency()`, ignoring cpusets and `taskset`.
- Windows: results of `SetProcessWorkingSetSizeEx` and `GlobalMemoryStatusEx` are ignored; `pin()` silently does nothing for `cpu >= 64` on single-group systems.

**Files:** `src/platform_posix.cpp`, `src/platform_win.cpp`, `src/platform.h`.

**Steps**
1. POSIX: build the allowed CPU list once with `sched_getaffinity`, make `ncpu()` return its size, and make `pin(i)` pin to the i-th allowed CPU (wrapping). Check and surface the `pthread_setaffinity_np` result (a one-time warning is enough).
2. Windows: check `GlobalMemoryStatusEx` (treat failure as "unknown", not 0 free) and log a warning if `SetProcessWorkingSetSizeEx` fails. Return a bool from `pin()` or log once when pinning is skipped.
3. Add a selftest case that runs under `taskset -c 0,1` and asserts a clean run.

**Done when:** the `taskset` run uses only the allowed CPUs and reports a sensible thread count.

---

## S8. Replace magic integers with enums and named constants

**Status:** TODO (do before S9)

**Problem.** `Settings` uses `cacheMode` 0..3, `rngMode` 0..3 and `suite` 0/1 compared by number. `R.rng = s.rngMode == 2 ? 1 : s.rngMode == 3 ? 2 : 0` is an off-by-one mapping between two integer schemes. Literals 64 (line size), 4096 (page), `0x10000` (tick interval) and 118 (progress width, duplicated in `src/log.cpp` and `src/run.h`) appear throughout.

**Files:** `src/suite.h`, `src/suite.cpp`, `src/main_cli.cpp`, `src/main_gui.cpp`, `src/types.h`, `src/run.h`, `src/log.cpp`, `src/seed.h/.cpp` (for `mix64`), algo files.

**Steps**
1. `enum class CacheMode { Default, Bypass, Cached, NonTemporal }`, `enum class Rng { Default, SplitMix, Murmur, XorShift }` (make `Run::rng` use the same enum or one clearly documented mapping), `enum class SuiteSet { Full, Quick }`. Update the CLI `choice()` mapping and the GUI combo-box code.
2. Add to `types.h`: `constexpr u64 kLineBytes = 64; kPageBytes = 4096; kTickLines = 0x10000;` and `kStatusWidth = 118` in `log.h`. Replace the literals mechanically, one file at a time.
3. Keep behavior byte-for-byte identical: the seeds, orders and data must not change.

**Done when:** `selftest.sh` still passes, including the "one seed reproduces every random choice" check and the printed-seed reproduction check; no remaining `== 2 ? 1 : ... == 3 ? 2` style mappings.

---

## S9. Split `run_suite`

**Status:** TODO (after S1 and S8)

**Problem.** `run_suite` is about 185 lines and does validation, test selection, memory sizing, allocation, FPU threads, the main loop, the failure report and cleanup in one function.

**Files:** `src/suite.cpp` (optionally a private header).

**Steps** (pure refactor, no behavior change)
1. `select_tests(const Settings&, std::vector<TestDef>&, Logger&)` -> filtered, adjusted list (the `enabled` / `quick` / filter / `seconds` / cache-mode logic).
2. `size_region(const Settings&, int threads, Logger&)` -> bytes per thread and total, or an error (the 90% / 97% logic).
3. `run_one_test(Run&, TestDef&, u64 loop, size_t idx, size_t count, u64 total)` -> per-test result struct (bad lines, bits, timings).
4. `print_failure_detail(...)` for the DQ lane / beat / codeword block, and `print_final_summary(...)`.
5. `run_suite` becomes a short orchestrator using the S1 guards.

**Done when:** `run_suite` is under about 60 lines, the output text is identical for a clean run and an injected-fault run (diff the logs with a fixed `--seed`), and `selftest.sh` passes.

---

## S10. Remove duplication

**Status:** TODO

**Problem.** (a) `lower(t.p.str("algo","fill"))` is repeated about five times across `src/suite.cpp` and `src/algo.cpp`. (b) `src/algo_pagemix.cpp` repeats `if (fl) f<1>(...) else f<0>(...)` six times. (c) The hand-synced key sets in `src/algo.cpp` ("must be kept in step by hand").

**Files:** `src/params.h`, `src/suite.cpp`, `src/algo.cpp`, `src/algo_pagemix.cpp`, `src/lineio.h`.

**Steps**
1. Add `TestDef::algo()` (or resolve once into a field when the suite is built) and use it everywhere.
2. Add `with_flush(bool fl, F&& f)` next to `dispatch()` in `lineio.h` (calls `f(IC<0>{})` or `f(IC<1>{})`) and use it in pagemix.
3. For (c), either generate the shared sets from the helper that reads them (e.g. `build_vars::keys()`, `stride_list` and `window_lines` each exposing their keys), or add a unit test that fails when a helper reads a key that is not in the set.

**Done when:** no behavior change (`selftest.sh` passes) and the repeated expressions are gone.

---

## S11. Stale docs and comments

**Status:** TODO

**Problem.** README says "single-file Windows memory tester" although the project is a multi-file CMake build; it also carries history trivia ("87 consolidated into 35..."). `src/algo.h` has a duplicated banner around the evolving-data section and a leftover "(M7)" milestone tag. Short variable names (`sg`, `nq`, `ow/ov`, `wo/vo`) lack a one-line gloss at their first use.

**Files:** `README.md`, `src/algo.h`, algo files.

**Steps**
1. Reword the README intro; keep the "Honest limits" section as is.
2. Keep README behavior notes in sync with S2 (log failure), S3 (progress line) and S6 (Ctrl-C).
3. Remove the duplicate banner and milestone tags in `algo.h`; grep for other `(M\d)` style tags.
4. Add short comments where a one- or two-letter name is introduced in a long function.

**Done when:** `bash tools/format.sh --check` passes; README claims match the code (re-check test counts and the layout list).

---

## S12. CI: sanitizers and warnings

**Status:** TODO (best done last)

**Problem.** The code is heavily multithreaded with lock-free flags and CI has no ASan/UBSan/TSan leg. `-Wall -Wextra` is on but not `-Werror`. The Windows job is only a 1-minute smoke test.

**Files:** `tools/selftest.sh`, `.github/workflows/build.yml`, `CMakeLists.txt`.

**Steps**
1. Add a `SAN=thread|address,undefined` option to `tools/selftest.sh` that passes `-fsanitize=...` (and `-O1 -g`) to the engine and test builds; keep the default run unchanged.
2. Add CI jobs: one with `SAN=address,undefined`, one with `SAN=thread` on a reduced set of tests (TSan is slow; the quick suite on a tiny region is enough).
3. Triage findings. Expect the `status()` race (S6). Do not blanket-suppress; fix or document each suppression.
4. Turn on `-Werror` in CI only (e.g. `-DDDR5_WERROR=ON`), after the tree is warning-clean.
5. Optional: extend the Windows smoke test to run `--list` and a short `--seed` reproducibility check.

**Done when:** the new CI jobs are green and each suppression (if any) has a comment explaining why.

---

## Notes for later sessions

- Out of scope for this plan: new test algorithms, GUI redesign, ARM support (the engine requires x86 AVX2 by design).
- Audit gaps: these files were not reviewed and may hold more findings: `algo_hammer`, `algo_march`, `algo_rwstorm`, `algo_evolve`, `algo_copy`, `algo_probe`, `algo_coherency`, `suite_data.cpp`, `inject.cpp`, `tools/selftest.sh`, `tools/unit_tests.cpp`, and most of `main_gui.cpp`. Add a section here if a pass over them finds something.
