# Audit fix plan

Source: audit of the `cleanup` branch (maintainability/readability, performance/resource management, reliability/error handling), then a second pass that checked every claim below against the code and a real build (g++ 13, clean build, unit tests pass). Items marked **verified by running** were reproduced with the CLI; the rest were verified by reading the code. The Windows/GUI paths were read only, never run.

## How to resume (read this first)

- Each section (S1..S13) is self-contained: goal, files, steps, done-when. Pick the first section in the suggested order whose status is not `DONE`.
- Update the **Status board** and the section's **Status** line as you go, and add one line to the **Progress log**. Commit the plan changes together with the code.
- One section = one commit (or PR). Do not mix sections.
- Gate for every section before marking DONE: `bash tools/format.sh --check`, `bash tools/selftest.sh` (Linux, g++ 11+ with AVX2, takes a few minutes). On Windows also run `build.bat`.
- If a section turns out to be wrong or already fixed, mark it `SKIPPED` and say why in the log.
- Line numbers below are from the audit and will drift; search by the quoted code instead.

### Order

S13 -> S3 -> S2 -> S5 -> S6 -> S1 -> S4 -> S7 -> S8 -> S10 -> S9 -> S11 -> S12

Dependencies: S9 (split `run_suite`) comes after S1 (RAII) and S8 (enums). Both S1 and S9 need access to `Run`'s private members (see their notes). S12 (sanitizers) comes last.

## Status board

| ID  | Area                          | Category    | Priority | Status |
|-----|-------------------------------|-------------|----------|--------|
| S13 | Evolve: false errors on stop  | Correctness | High     | DONE   |
| S1  | RAII + worker exceptions      | Reliability | Medium   | DONE   |
| S2  | CLI log open failure          | Reliability | High     | DONE   |
| S3  | Progress line error count     | Reliability | High     | DONE   |
| S4  | `report()` hardening          | Rel + Perf  | Low      | DONE   |
| S5  | Input value validation        | Reliability | Medium   | DONE   |
| S6  | `status()` race + Ctrl-C      | Reliability | Medium   | DONE   |
| S7  | Platform robustness           | Reliability | Medium   | DONE   |
| S8  | Enums and named constants     | Readability | Low      | DONE   |
| S9  | Split `run_suite`             | Readability | Low      | DONE   |
| S10 | Remove duplication            | Readability | Low      | DONE   |
| S11 | Stale docs and comments       | Readability | Low      | DONE   |
| S12 | CI: sanitizers, warnings      | Tooling     | Low      | TODO   |

## Progress log

| Date       | Section | Note |
|------------|---------|------|
| -          | -       | Plan created from audit |
| 2026-11-11 | all     | Plan checked against the code and a real build; S13 added, corrections folded into S1-S12 |
| 2026-11-12 | S13     | DONE: `R.stop` check before the evolve verify walk; new inject hook `evolve_stop_in_write` (env `DDR5TEST_STOP_IN_EVOLVE`) + selftest case. Without the fix: 3 of 3 runs report 200 false errors (log cap), exit 1; with it: 0 errors, exit 0. Stop paths of the other algos re-audited, no same-shaped bug found |
| 2026-11-12 | S3      | DONE: progress line prints the run total plus `(this test N)` (`src/run.h`) |
| 2026-11-12 | S2      | DONE: `Settings::logFileSet`; log opened before the allocation; explicit `--log` that cannot be opened -> exit 2, default name -> warning; README + `--help` updated; selftest group 2d |
| 2026-11-12 | S5      | DONE: strict `pnum_checked`, bad values recorded and reported by `bad_values()`, `run_suite` refuses to start (exit 2); unit tests for 16 bad / 10 good number forms, flags, the whole suite and 10 planted mistakes |
| 2026-11-12 | S6      | DONE: atomics for the fields `status()` reads, no `R.stop = false`, double Ctrl-C (POSIX `SA_RESETHAND`, Windows `ExitProcess(130)`), README and `--help` updated, unit test for the status poll and for an early Stop |
| 2026-11-12 | S1      | DONE: `ScopeExit` guards in `run_suite`, exception-safe `Run::parallel` (rethrow after join, barrier slots dropped, `aborted` flag), `catch (...)`; unit tests + inject selftest for exit code 3 |
| 2026-11-12 | S4      | DONE: `appendf` (log.h) grows a `std::string` instead of clamping a fixed buffer, used by `report()` and `Logger::emitf`; first-error times and `errors.logged` atomic, `report()` returns before `errors.mu` once `maxLog` blocks are written; `Counter` (one cache line each) for `lane`/`beat`/`abit`; `test_report` in `tools/unit_tests.cpp` |
| 2026-11-12 | S7      | DONE on Linux, Windows part unbuilt: `ncpu()`/`pin()` follow `sched_getaffinity` (taskset), `pin()` returns bool and `Run::parallel` warns once, `avail_phys()` from `MemAvailable` with 0 = unknown handled in `run_suite`, `plat::alloc` returns a `note` (why memory is not locked), Windows API results checked; selftest group 2e |
| 2026-11-12 | S8      | DONE on Linux, GUI edits unbuilt: `Rng`/`CacheMode`/`SuiteSet` enums, `kLineBytes`/`kPageBytes`/`kLinesPerPage`/`kTickLines`/`kStatusWidth`/`kWindowGranuleLines`; fixed-seed error output identical to the pre-change build for all four `--rng` values; `format.sh --check` and `selftest.sh` pass |
| 2026-11-12 | S10     | DONE: `TestDef::algo()`, `with_flush` (pagemix), `Params::reads` recorder + `test_shared_key_sets` (planted missing key is caught); no behavior change, `format.sh --check` and `selftest.sh` pass |
| 2026-11-12 | S9      | DONE: `SuiteDriver` (friend of `Run`) with one member per step, `run_suite` is a wrapper, `run()` 41 lines; console output and log files identical to the previous build for 9 fixed-seed scenarios (clean, injected, stop-on-error, error paths) + `--fpu`; `selftest.sh` passes |
| 2026-11-12 | S11     | DONE: README intro (multi-file CMake build, Linux console tool) and test-count history trivia reworded, README notes the `(this test N)` progress field and the stop behaviour; `suite_data.cpp` key header rewritten by hand under `clang-format off` (grouped by data / access / per-algo keys, adds `pagemix` and `coherency`, drops the bogus `inverted` key); duplicate banner and `(M7)` removed in `algo.h`; stale `[default]` / `[test]` config-file wording replaced in `params.h` / `algo.cpp`; one-line glosses for `wo/vo`, `ow/ov/of`, `sq/sg`, `nq/ng`; `format.sh --check` and `selftest.sh` pass |

---

## S13. Evolving data reports false errors when a run is stopped

**Status:** DONE (2026-11-12). Step 3 audit result: `algo_fill` and `algo_march`/`copy`/`hammer`/`chase`/`pagemix` guard their verify with `!R.stop`; `algo_rwstorm` rewrites identical data, and its unguarded final check walks the same order as the initial fill with the same poll interval, so it never reaches a line the fill did not. Remaining theoretical window: a stop raised between two threads' `R.stop` checks (one thread verifies while its neighbour has left); not reproduced, narrow, and the same shape as the pre-existing loop guards.

**Problem (verified by running).** Stopping a run during the "Evolving data" test can report thousands of bad lines and exit with code 1 on healthy memory. The stop can come from Ctrl-C, the GUI Stop button, `--coverage` (reproduced) or `--minutes`/the time limit (same flag). In 16 SIGINT runs at staggered times, 6 ended with exit 1 (up to 63,486 "bad lines"); `--coverage 150` reproduced it in 2 of 9 runs. All 28 tests were interrupted with SIGINT and only this one misbehaved.

Cause: in `src/algo_evolve.cpp` the verify walk runs right after the rewrite pass with no `R.stop` check in between. `walk()` only polls `R.stop` every 65,536 lines, so a rewrite pass cut short by stop leaves some lines at the previous generation, and the verify walk then checks up to 65,536 lines against the new one. Every logged error is `evolve-verify` showing STABLE stale data. The same flag is shared with the other algos, which are safe because their verify loops are guarded by `!R.stop`.

**Files:** `src/algo_evolve.cpp`, `src/inject.h`, `src/inject.cpp`, `tools/selftest.sh`.

**Steps**
1. In the pass loop, after the `DDR5_INJECT(evolve_last_pass, ...)` line and before the verify `walk`, add:
   ```cpp
   if (R.stop)
       break; // the rewrite walk above may have been cut short: lines it never reached still hold the
              // previous generation and must not be verified against this one
   ```
   (Prototyped on a copy: 0 of 16 SIGINT runs failed afterwards, and the fault-injection test still finds exactly its 2 planted faults. The break is safe for the barrier: a thread that leaves early is dropped by `arrive_and_drop`.)
2. Add a deterministic regression test. The failure is intermittent, so do NOT rely on a plain `--coverage` or SIGINT selftest case. Mirror the existing `DDR5TEST_STOP_IN_FILL` hook for pagemix: an inject hook (e.g. `evolve_stop_in_write`, enabled by an env var) that sets `R.stop` partway through a rewrite pass, then check that the run reports no `ERROR` lines.
3. Audit the other algos' stop paths once more for the same shape (a verify walk that is not preceded by an `!R.stop` check, after a walk that can be cut short). None were found in this pass.

**Done when:** the new inject test passes with the fix and fails without it; `selftest.sh` passes.

---

## S1. RAII and worker exception safety

**Status:** DONE (2026-11-12). Access question settled without any `friend`/public change: lambdas defined inside the friend `run_suite` have its access, so the guards are `ScopeExit` lambdas there (`logGuard`, `regionGuard`, `fpuGuard`; destroyed in reverse order on both the success and exception path; `catch (...)` added). `Run::parallel` keeps the first exception (worker, thread creation or monitor loop), sets `stop` and a new `aborted` flag (`report()` ignores mismatches after it), always leaves the barrier, joins, and rethrows on the calling thread; unstarted workers have their barrier slots dropped. Tests: `test_exceptions` (worker throw, partner waiting at the barrier, exception inside `run_suite` with `--fpu` and the region allocated: exit 3, logged, `R.layout.region == nullptr`) abort with `terminate` on the old code and pass now, clean under TSan; selftest case with a new inject hook (`DDR5TEST_THROW_IN_WORKER`) checks exit 3 end to end. Not covered by a test: the thread-creation-failure branch (needs an OS thread limit) and the log file closing on the exception path (the guard is the same pattern as the region guard; the unit test runs with a log sink, where no file is opened).

**Problem.** `run_suite`'s `catch` (end of `src/suite.cpp`) returns 3 without releasing the region, closing `errors.logf`, or joining the FPU threads. A joinable `std::thread` destroyed during unwinding calls `std::terminate`: with `--fpu`, any exception thrown on the calling thread terminates the process during unwinding (before the catch runs), so the exit is an abort, not 3. Separately, the worker lambda in `Run::parallel` (`src/run.h`) has no try/catch, so any throw in a worker terminates the process. `Run::parallel` also terminates if `th.emplace_back` throws (thread limit), leaving started threads joinable and the barrier expecting `layout.T` participants.

**Realistic triggers (audit correction).** The plan's original example (`std::vector` in `algo_chase`) is hard to hit: its size comes from suite data. A realistic one is the pagemix metadata (`std::vector<PagePat>(np)`, `new std::atomic<u8>[np]`, `filled`) allocated on the calling thread while 97% of free memory is already committed.

**Files:** `src/suite.cpp`, `src/run.h`, possibly `src/platform.h`.

**Access note (audit correction).** `Run`'s `errors`, `progress`, `st`, `total` are private and only `run_suite` is a friend. A `LogFileGuard` that closes `errors.logf` (or any helper outside `run_suite`) cannot compile unless you add a `friend` declaration or small public methods on `Run`. Decide this before writing the guards.

**Steps**
1. Add small guards in `suite.cpp`: `RegionGuard` (calls `plat::release` and nulls `R.layout.region`), `LogFileGuard` (closes `errors.logf`), `FpuGuard` (sets `fpuStop`, joins threads).
2. Replace the manual cleanup at the end of `run_suite` with the guards, so the success path and the `catch` path share one cleanup. Add `catch (...)` next to `catch (const std::exception&)`.
3. In `Run::parallel`, wrap `fn(t)` in try/catch, store the first `std::exception_ptr` (guarded by a mutex or `std::atomic_flag`), set `stop`, still call `vflush()` and `arrive_and_drop()`, and rethrow on the calling thread after `join()`. Also make thread creation exception-safe (join what was started, drop the barrier slots of threads that never started).
4. Make sure the final summary and `return` codes are unchanged for non-exception runs.

**Done when**
- A forced `throw` inside a worker (temporary test hook or unit test) yields exit code 3 and a logged `internal error`, with no `terminate`. There is currently no test for exit code 3 at all.
- Region is released on the exception path (check with a small test that `R.layout.region == nullptr` afterwards).
- `selftest.sh` passes.

---

## S2. CLI: log file open failure is silent

**Status:** DONE (2026-11-12). Note: if S1 adds a `LogFileGuard`, it can replace the `close_log` lambda in `run_suite`, which currently closes the log on the two early returns after the open.

**Problem (verified by running).** `src/suite.cpp`: `R.errors.logf = R.log.sink ? nullptr : fopen(cfg.logFile.c_str(), "a");` and nothing happens when `fopen` returns null. `--log /nonexistent/dir/x.log` exits 0 with no message and the persistent error log never exists. The GUI already handles this (temp-folder fallback plus status message, `main_gui.cpp`).

**Files:** `src/suite.cpp`, `src/suite.h`, `src/main_cli.cpp`.

**Steps**
1. If `!R.log.sink` and `fopen` fails, emit a clear warning with the path and `strerror(errno)`.
2. Policy: exit 2 when the user passed `--log` explicitly and it cannot be opened; warn and continue (errors still print to the console) for the default file name. Document it in the README. `Settings::logFile` defaults to `"ddr5test.log"` and nothing records whether `--log` was given, so add a `bool logFileSet` to `Settings`, set by `main_cli.cpp`.
3. Move the open before the memory allocation. Today it happens after `plat::alloc` and the zeroing `memset` of GBs, so the exit-2 policy would only fire after the slow part. Put it next to the other early validation.
4. Add selftest cases: `--log /nonexistent/dir/x.log` exits 2; the default log name that cannot be opened warns and still exits 0 on a clean run.

**Done when:** the new selftest case passes and the README mentions the behavior.

---

## S3. Progress line shows per-test error count

**Status:** DONE (2026-11-12). Verified on a pty with `--keep-going` and injected faults: later tests show e.g. `errors: 294 (this test 0)`; longest line 116 columns.

**Problem.** `Run::parallel` in `src/run.h` prints `st.lines` (line ~252), which `run_suite` resets every test (`R.st.clear()`, `suite.cpp` ~169), instead of the run total. In keep-going mode earlier failures vanish from the live line. (Confirmed by reading; `errors.errTotal` is the right run-total field.)

**Files:** `src/run.h`.

**Steps**
1. Print `errors.errTotal.load()` (run total) in the `errors:` field, followed by the per-test count, e.g. `errors: 12 (this test 3)`.
2. Keep the line within the 118-column TTY width (see S8). Nothing in `tools/selftest.sh` or `tools/unit_tests.cpp` depends on this format, so nothing else needs updating.

**Done when:** a keep-going run with injected faults keeps showing a non-zero error count in later tests; `selftest.sh` passes.

---

## S4. Harden `Run::report` (buffer math, lock contention, counters)

**Status:** DONE (2026-11-12). Deviation from step 1/2: instead of a clamped `append` on a fixed buffer, one `appendf(std::string&, fmt, ...)` in `log.h/.cpp` that grows the string to fit. It removes the wrap and also the truncation (a long test name no longer costs the qword lines after it), and `Logger::emitf` uses the same helper, so the 2048-byte cut is gone too; the string is only built for the first `maxLog` (default 200) errors, so the allocation does not matter. Step 3: `errors.firstErrRun/firstErrTest` are `std::atomic<double>` set with compare-exchange (`run_suite` still resets `firstErrTest` between tests, with no worker running), `errors.logged` is atomic, and `report()` checks it before taking `errors.mu`, then again under the lock; the lock is now taken only to write a block. Step 4: `Counter` (`alignas(64)`, derives from `std::atomic<u64>`, so call sites are unchanged) for `lane`, `beat` and `abit`. Step 5: `test_report` (worst-case block with a 3000-character test name, exact text of all 9 lines; `appendf`/`emitf` with 5000 characters; 4 threads x 100 errors give 400 counted and exactly `maxLog` well-formed blocks). Checked by planting faults: a truncating `appendf` fails 11 checks, removing the `maxLog` limit fails the block count. `selftest.sh` passes with the fault-injection counts unchanged (42 lines / 44 bits), the unit tests are clean under TSan (0 warnings). Not measured: the contention gain itself (no benchmark of an error storm).

**Problem**
- `o += snprintf(buf + o, sizeof buf - o, ...)` can wrap if the output ever exceeds 1400 bytes: `snprintf` returns the would-be length, and `sizeof buf - o` then underflows to a huge value, which turns truncation into an overflow. Safe today (the longest message is ~1.1 KB), fragile if the label or fields grow.
- `Logger::emitf` silently truncates at 2048 bytes. No current caller gets near it.
- `report()` takes `errors.mu` twice per error, even after `cfg.maxLog` is reached, and holds it during log emit and `fflush`. In keep-going mode on a badly failing DIMM, threads serialize here.
- Shared `std::atomic` arrays (`lane`, `beat`, `abit`) are incremented from every thread, which causes false sharing during error storms.

**Files:** `src/run.cpp`, `src/run.h` (`Stats`, `ErrorSink`), `src/log.cpp`.

**Steps**
1. Replace the manual offset arithmetic with a clamped `append(buf, o, fmt, ...)` helper that never lets `o` exceed `sizeof buf - 1`.
2. In `Logger::emitf`, detect truncation (`vsnprintf` return >= buffer size) and grow the buffer to fit.
3. Make the "first error" bookkeeping lock-free (compare-exchange on `std::atomic<double>`), make `errors.logged` atomic, and return early when it has reached `cfg.maxLog` before doing any formatting or locking. Counting in `st` / `errTotal` must still happen first.
4. Add `alignas(64)` padding to the hot `Stats` arrays (`lane`, `beat`, `abit`) to avoid false sharing during error storms.
5. Add a unit test that feeds a worst-case line (every qword differs, long label) and asserts the output is well formed and not truncated mid-line.

**Done when:** the unit test passes and `selftest.sh` fault-injection counts are unchanged (42 lines / 44 bits in the pattern test).

---

## S5. Validate values in test data, not only keys

**Status:** DONE (2026-11-12). Design: `pnum_checked()` is strict and `pnum()` wraps it; `Params::num/nums/flag/parse_num` record bad values in `Params::bad` (still returning 0/false) and `bad_values(TestDef)` in `algo.cpp` runs each algo's own `XOpts::from` (+ `build_vars`, `stride_list`, `window_mb`) on a copy with the messages captured, so there is no second table of which key is numeric. `run_suite` checks every test in the built-in suite before allocating and exits 2 on any bad value. March `D()`/`X()` values and `ospec` messages now name the test and the value. Checked by planting `"reads": "2x"` in `suite_data.cpp`: unit test fails, CLI exits 2.

**Problem (verified by running).** `pnum()` (`src/params.cpp`) uses `strtoull` with no error check and silently accepts junk:

| input | result |
|---|---|
| `"4KB"` | 4 |
| `"abc"` | 0 |
| `"1.5G"` | 1 GiB (decimals truncated) |
| `"17179869184G"` | 0 (multiplication overflow wraps) |
| `"99999999999999999999"` | 2^64-1 (saturated) |
| `"-1"` | 2^64-1 |

`Params::flag()` also treats any unrecognized value as false (`"ture"` -> false). `ospec()` (`src/algo.cpp`) logs `bad order` without the test name or the offending value, then continues with a default. The CLI is strict about this; the compiled-in suite is not. Since the suite is compiled in, a bad value can only come from a developer edit, so the unit test is the right place to catch it.

**Files:** `src/params.cpp`, `src/params.h`, `src/algo.cpp`, `src/gen.cpp`, `src/algo_march.cpp` (both also call `pnum`), `tools/unit_tests.cpp`, `tools/selftest.sh`.

**Steps**
1. Add a checked parser (e.g. `bool pnum_checked(const std::string&, u64&)`) that rejects trailing garbage, empty input, a leading sign, decimals, and overflow (both of `strtoull` and of the K/M/G multiply). Keep `pnum()` as a thin wrapper over it for the existing callers.
2. Make `Params::num/nums/flag` report bad values through the logger or a validation pass, and have the suite validation step (next to `unknown_keys`) fail the run (exit 2) or warn loudly on a bad value.
3. Include the test name and the offending value in the `ospec` message.
4. Unit tests: `"4KB"`, `"abc"`, `"99999999999999999999"`, `"0x"`, `"1.5G"`, `"17179869184G"`, `"-1"`, plus the valid forms `"4K"`, `"2M"`, `"0x10"`.

**Done when:** a deliberately bad value in `suite_data.cpp` is caught by the unit tests or selftest, and the valid forms still parse identically.

---

## S6. `Run::status()` data race, lost Stop, and Ctrl-C behavior

**Status:** DONE (2026-11-12). `layout.regionBytes` and `progress.tStart` are atomics (release store in `run_suite`, acquire load in `status()`); `R.stop = false` removed; POSIX uses `sigaction` + `SA_RESETHAND` (second Ctrl-C kills with 130), Windows counts hits and calls `ExitProcess(130)` (read only, not compiled here). `test_run_status` in `tools/unit_tests.cpp` polls `status()` during `run_suite` and checks a pre-set Stop is honored; it is clean under TSan (0 warnings) and fails if `R.stop = false` is put back. S12 can reuse it for the TSan leg.

**Problem**
- The GUI thread reads `layout.regionBytes` and `progress.tStart` (plain fields) through `Run::status()` while `run_suite` writes them from the worker thread. Benign on x86, formally UB. These are the only cross-thread plain reads: `main_gui.cpp` touches the `Run` only via `status()`, `error_count()` and `stop`.
- `run_suite` does `R.stop = false` (`suite.cpp` ~108). A Stop or Ctrl-C that arrives before that line is swallowed. A `Run` serves exactly one `run_suite` call (see the comment on `Run`), so the line is unnecessary: delete it.
- The Ctrl-C handler only sets `stop`; a second Ctrl-C does nothing, so a wedged run can only be killed externally. (Measured: with the first Ctrl-C a run stops within 0.4 s in all 28 tests, so responsiveness is fine when nothing is wedged.)

**Files:** `src/run.h`, `src/suite.cpp`, `src/platform_posix.cpp`, `src/platform_win.cpp`.

**Steps**
1. Delete `R.stop = false` in `run_suite`.
2. Make the fields read by `status()` atomic (`std::atomic<u64>` and `std::atomic<double>`), with release stores in `run_suite` and acquire loads in `status()`.
3. In both platform handlers, count signals: first sets `stop`, second restores the default handler and re-raises (POSIX, prefer `sigaction`) or calls `ExitProcess(130)` (Windows). Keep the handler async-signal-safe (atomics only; the handler will need its own counter next to `g_stop`).
4. Document the behavior in `--help` and the README.

**Done when:** a unit test that runs `run_suite` on one thread and polls `status()` from another is clean under TSan (see S12; the CLI never calls `status()`, so a plain CLI run under TSan will not show this race); double Ctrl-C terminates a hung run on Linux (manual test with a long `--test-seconds`).

---

## S7. Platform robustness

**Status:** DONE on Linux (2026-11-12); the Windows half is written but NOT compiled or run (no Windows toolchain in the session). POSIX: `allowed_cpus()` reads `sched_getaffinity` once (falls back to `hardware_concurrency()` only if that fails), `ncpu()` is its size, `pin(i)` pins to the i-th allowed CPU (wrapping) and returns whether it worked; `avail_phys()` reads `MemAvailable` and falls back to `sysconf`, where a `-1` now gives 0. `0` means "unknown" (documented in `platform.h`): `run_suite` then asks for `-mb` instead of reading it as "no memory free" (with `-mb` it runs, saying the size is unchecked). `Run::parallel` counts the workers whose `pin()` failed and warns once per run after they have joined. Deviation: `plat::alloc` got a `std::string& note` parameter so the platform layer can say why memory could not be locked (POSIX: the `mlock` errno, Windows: the `SetProcessWorkingSetSizeEx` / `VirtualLock` error code) - it has no logger of its own. Windows: `GlobalMemoryStatusEx` failure gives 0 (unknown), `pin()` returns false for `cpu >= 64` on a single group and for a failed call. Selftest group 2e (`taskset` on the first two / first allowed CPUs: 2 threads / 1 thread by default, clean run, no pin warning; two workers each pinned to one of the top two allowed CPUs). Checked: the previous build fails the 1-thread case (starts 2 threads under `taskset -c 0`); forcing `pin()` to fail gives exactly one warning and a valid run; forcing `avail_phys()` to 0 gives the `-mb` message / unchecked run. Limits: on this 2-CPU machine the pinning check cannot tell old from new (it only does with more than 2 CPUs); a failing `pthread_setaffinity_np` cannot be provoked naturally, so that path was only checked by forcing it; containers' cgroup memory limits are still not considered (`MemAvailable` is the host's); `main_gui.cpp` still calls `GlobalMemoryStatusEx` itself and ignores its result.

**Problem**
- POSIX `pin()` uses `cpu % CPU_SETSIZE` and ignores the return value; `ncpu()` uses `hardware_concurrency()`, which ignores `taskset`/cpusets (**verified by running**: under `taskset -c 0`, `nproc` says 1 but `hardware_concurrency()` says 2 and the CLI still starts 2 threads).
- POSIX `avail_phys()` uses `sysconf(_SC_AVPHYS_PAGES)` (free pages, not `MemAvailable`) and does not check for a `-1` error return, which would become a huge `u64`.
- Windows: results of `SetProcessWorkingSetSizeEx` and `GlobalMemoryStatusEx` are ignored; `pin()` silently does nothing for `cpu >= 64` on single-group systems.

**Files:** `src/platform_posix.cpp`, `src/platform_win.cpp`, `src/platform.h`.

**Steps**
1. POSIX: build the allowed CPU list once with `sched_getaffinity`, make `ncpu()` return its size, and make `pin(i)` pin to the i-th allowed CPU (wrapping). Check and surface the `pthread_setaffinity_np` result (a one-time warning is enough). Read `MemAvailable` from `/proc/meminfo` for `avail_phys()` and fall back to `sysconf` only if that fails; treat a failed `sysconf` as "unknown".
2. Windows: check `GlobalMemoryStatusEx` (treat failure as "unknown", not 0 free) and log a warning if `SetProcessWorkingSetSizeEx` fails. Return a bool from `pin()` or log once when pinning is skipped.
3. Add a selftest case that runs under `taskset -c 0,1` and asserts a clean run and a thread count of 2.

**Done when:** the `taskset` run uses only the allowed CPUs and reports a sensible thread count.

---

## S8. Replace magic integers with enums and named constants

**Status:** DONE on Linux (2026-11-12); the GUI edits (`main_gui.cpp`) are written but NOT compiled (no Windows toolchain in the session). One enum, no mapping: `Rng { Default, SplitMix, Murmur, XorShift }` in `seed.h` is used by `Settings::rngMode`, `Run::rng`, `Gen::rng`, `build_vars`, `evo_make` and `mix64` (Default and SplitMix are the same function, so the old `== 2 ? 1 : == 3 ? 2 : 0` mapping is gone and no seed's data changes). `CacheMode` and `SuiteSet` are in `suite.h`; the CLI `choice()` index and the GUI `CB_GETCURSEL` index convert straight to the enums (list order = enum order, commented at both places; the GUI log header still prints the numbers via `(int)`). Constants: `kLineBytes`, `kPageBytes`, `kLinesPerPage`, `kTickLines` in `types.h`, `kStatusWidth` in `log.h`, and `kWindowGranuleLines` (4096 LINES = 256 KiB, the `window_mb` rounding in `Run::window_lines`; it only looks like the page size). The progress line uses `%-*.*s` with `kStatusWidth`. Checked: old (HEAD) and new builds give identical qword-level error output for the injected "Evolving data" test with `--seed 7` under all four `--rng` values (and murmur differs from xorshift, so the check is sensitive to the mixer); unit tests and `selftest.sh` pass. Left as literals on purpose: `inject.cpp` (test hooks with hand-picked offsets), `main_gui.cpp` widget sizes, the `1..64` lines-per-round cap and thread cap in `algo_coherency.cpp`, bit widths in `gen.cpp`, and `64 * 1024` (KiB) in `algo_copy.cpp`.

**Problem.** `Settings` uses `cacheMode` 0..3, `rngMode` 0..3 and `suite` 0/1 compared by number. `R.rng = s.rngMode == 2 ? 1 : s.rngMode == 3 ? 2 : 0` is an off-by-one mapping between two integer schemes (default and SplitMix both map to 0). Literals 64 (line size), 4096 (page), `0x10000` (tick interval) and 118 (progress width) appear throughout.

**Files:** `src/suite.h`, `src/suite.cpp`, `src/main_cli.cpp`, `src/main_gui.cpp`, `src/types.h`, `src/run.h`, `src/log.cpp`, `src/seed.h/.cpp` (for `mix64`), algo files.

**Steps**
1. `enum class CacheMode { Default, Bypass, Cached, NonTemporal }`, `enum class Rng { Default, SplitMix, Murmur, XorShift }` (make `Run::rng` use the same enum or one clearly documented mapping), `enum class SuiteSet { Full, Quick }`. Update the CLI `choice()` mapping and the GUI combo-box code (the GUI converts `CB_GETCURSEL` indices directly, so the conversion belongs at that boundary).
2. Add to `types.h`: `constexpr u64 kLineBytes = 64; kPageBytes = 4096; kTickLines = 0x10000;` and `kStatusWidth = 118` in `log.h`. Replace the literals mechanically, one file at a time. **Careful:** the literal `118` appears in `run.h` (`%-118.118s`) and `log.cpp` (progress width), but ALSO in `main_gui.cpp` as unrelated widget sizes and a layout offset - leave those alone. `printf` needs `%-*.*s` to take the width from a constant.
3. Keep behavior byte-for-byte identical: the seeds, orders and data must not change.

**Done when:** `selftest.sh` still passes, including the "one seed reproduces every random choice" check and the printed-seed reproduction check; no remaining `== 2 ? 1 : ... == 3 ? 2` style mappings.

---

## S9. Split `run_suite`

**Status:** DONE (2026-11-12). Access settled with one `friend struct SuiteDriver;` in `run.h` (replaces `friend int run_suite`); `run_suite` is now a 2-line wrapper and `SuiteDriver` (in `suite.cpp`) has one member per step: `suite_is_valid` (unknown keys, bad values), `read_settings`, free `select_tests(Settings, tests)` (pure: no Run, no log), `open_error_log` / `close_error_log`, `configure_run`, `size_region` (the 90% / 97% logic, says why on failure), `announce`, `allocate` / `release_region`, `prepare_region`, `run_tests`, `run_one_test`, `print_failure_detail`, `print_final_summary`; the FPU threads moved into a small `FpuThreads` struct. `run()` is 41 lines (was about 188): each acquiring step is followed at once by its `ScopeExit` guard (the S1 guards, same order). Deviations from the steps above: `select_tests` takes no Logger (it logs nothing) and returns the list, an empty one is reported by `run()`; there is no per-test result struct, `run_one_test` returns the bad-line count and the totals stay on `Run` (`R.st` / `R.total`). Verified against a baseline binary built from the previous commit, fixed `--seed`, timing / GB/s / ASLR addresses normalised: clean run, injected fill / evolve, stop-on-error, `--cache bypass`, `--fpu`, no tests selected, bad `--log`, too little memory and `--list` give identical console output and log files; unit tests, `format.sh --check` and `selftest.sh` pass. Page shuffle with injection is time-driven (its output differs between two runs of the *old* binary) so it is not in the comparison.

**Problem.** `run_suite` is about 188 lines and does validation, test selection, memory sizing, allocation, FPU threads, the main loop, the failure report and cleanup in one function.

**Files:** `src/suite.cpp`, plus `src/run.h` for access.

**Access note (audit correction).** The extracted helpers (`run_one_test`, `print_failure_detail`) touch `R.st`, `R.progress`, `R.errors`, `R.total`, which are private; only `run_suite` is a friend. Add a friend (e.g. one `struct SuiteDriver` that owns the helpers) or public methods before splitting.

**Steps** (pure refactor, no behavior change)
1. `select_tests(const Settings&, std::vector<TestDef>&, Logger&)` -> filtered, adjusted list (the `enabled` / `quick` / filter / `seconds` / cache-mode logic).
2. `size_region(const Settings&, int threads, Logger&)` -> bytes per thread and total, or an error (the 90% / 97% logic).
3. `run_one_test(Run&, TestDef&, u64 loop, size_t idx, size_t count, u64 total)` -> per-test result struct (bad lines, bits, timings).
4. `print_failure_detail(...)` for the DQ lane / beat / codeword block, and `print_final_summary(...)`.
5. `run_suite` becomes a short orchestrator using the S1 guards.

**Done when:** `run_suite` is under about 60 lines, the output text is identical for a clean run and an injected-fault run (diff the logs with a fixed `--seed`), and `selftest.sh` passes.

---

## S10. Remove duplication

**Status:** DONE (2026-11-12). `TestDef::algo()` (`params.h`) is the one place that lower-cases the `algo` key (default "fill"); `algo.cpp` (2), `suite.cpp` (3), `main_cli.cpp` (`--list`, which was the un-lowered copy) and `tools/unit_tests.cpp` (2 reads) use it; the two places that *set* the key (`t.p.m["algo"] = ...`) are untouched. `with_flush(bool, F)` sits next to `dispatch()` in `lineio.h` and replaces the four `if (fl) f<1>(...) else f<0>(...)` pairs in `algo_pagemix.cpp` (the final-verify loop calls `pm_check<1>` directly on purpose and stays as is). Step 3 deviation: instead of a hand-written list of which keys each helper reads (a second table), `Params` got an optional `reads` recorder (null in real runs, `touch()` in `has/str/num/flag`, so also `words/nums`); `test_shared_key_sets` records the keys `build_vars` (all 5 data branches), `stride_list` and `Run::window_lines` really look up and requires every algo that calls a helper to list them in `algo_keys()`. Which algo calls which helper is the one hand-kept part, in the test. Checked by planting a mistake: dropping `modulo` from the data keys in `algo.cpp` gives 6 failures, exit 1. `format.sh --check` and `selftest.sh` pass, fault-injection counts unchanged.

**Problem.** (a) `lower(t.p.str("algo","fill"))` is repeated 4 times in `src` (`algo.cpp` once, `suite.cpp` three times), twice more in `tools/unit_tests.cpp`, and once un-lowered in `main_cli.cpp` (`--list`). (b) `src/algo_pagemix.cpp` repeats `if (fl) f<1>(...) else f<0>(...)` 4 times (copy, fill, invert, check). (c) The hand-synced key sets in `src/algo.cpp` ("must be kept in step by hand").

**Files:** `src/params.h`, `src/suite.cpp`, `src/algo.cpp`, `src/algo_pagemix.cpp`, `src/lineio.h`, `src/main_cli.cpp`, `tools/unit_tests.cpp`.

**Steps**
1. Add `TestDef::algo()` (or resolve once into a field when the suite is built) and use it everywhere.
2. Add `with_flush(bool fl, F&& f)` next to `dispatch()` in `lineio.h` (calls `f(IC<0>{})` or `f(IC<1>{})`) and use it in pagemix.
3. For (c), add a unit test that fails when a shared helper (`build_vars`, `stride_list`, `window_lines`) reads a key that is not in the hand-kept set in `algo.cpp`.

**Done when:** no behavior change (`selftest.sh` passes) and the repeated expressions are gone.

---

## S11. Stale docs and comments

**Status:** DONE (2026-11-12). Not touched: the algo banners that clang-format reflowed into a run-on (for example `algo_coherency.cpp`, `algo_hammer.cpp`); they read acceptably and fixing them needs `clang-format off` around each.

**Problem.** README says "single-file Windows memory tester" although the project is a multi-file CMake build; it also carries history trivia ("87 consolidated into 35..."). `src/algo.h` has a duplicated banner around the evolving-data section and a leftover "(M7)" milestone tag (the only `(M<n>)` tag in the tree). Short variable names (`sg`, `nq`, `ow/ov`, `wo/vo`) lack a one-line gloss at their first use. The README test counts (28 tests, 12 quick) are correct (asserted by the unit tests).

New (audit): the key-documentation header at the top of `src/suite_data.cpp` - which the README points readers to - was reflowed by clang-format into a run-on paragraph (the `cross = 1 verify another thread's chunk window_mb limits ...` run), and its `algo` list omits `pagemix` and `coherency`.

**Files:** `README.md`, `src/algo.h`, `src/suite_data.cpp`, `.clang-format`, algo files.

**Steps**
1. Reword the README intro; keep the "Honest limits" section as is.
2. Keep README behavior notes in sync with S2 (log failure), S3 (progress line), S6 (Ctrl-C) and S13 (stop behavior).
3. Remove the duplicate banner and the `(M7)` tag in `algo.h`.
4. Restore the `suite_data.cpp` header: wrap it in `// clang-format off` / `// clang-format on`, re-break it by hand, and add `pagemix` and `coherency` to the `algo` list.
5. Add short comments where a one- or two-letter name is introduced in a long function.

**Done when:** `bash tools/format.sh --check` passes; README claims match the code (re-check test counts and the layout list).

---

## S12. CI: sanitizers and warnings

**Status:** TODO

**Problem.** The code is heavily multithreaded with lock-free flags and CI has no ASan/UBSan/TSan leg. `-Wall -Wextra` is on and `selftest.sh` already fails on any `warning|error` in the build log, so Linux/g++ already enforces a warning-free build. The gap is MSVC: its flags are only `/O2 /arch:AVX2 /EHsc /utf-8` with no warning level. The Windows job is only a 1-minute smoke test.

**What a TSan leg will actually report (verified by running a TSan build)** - the plan's earlier "expect the `status()` race" was wrong for CLI runs, since only the GUI calls `status()`:
- `rwstorm` is flagged BY DESIGN: even threads rewrite identical data while their partner verifies it (writer store at `algo_rwstorm.cpp` ~74 vs reader load ~82). TSan exits with code 66, which would fail `selftest.sh`. This needs a deliberate, commented exclusion (skip that test in the TSan leg), not a blanket suppression.
- `fpu_stress` has a real race on `g_fpu_sink` (`suite.cpp` ~54; several FPU threads write one global). Fix: make it a `std::atomic<float>` updated with relaxed ops. The same pattern exists in `g_sink` (`algo.cpp`, disturb traffic).
- The `status()` race (S6) only shows up through a unit test that polls it from another thread.

**Files:** `tools/selftest.sh`, `tools/unit_tests.cpp`, `.github/workflows/build.yml`, `CMakeLists.txt`, `src/suite.cpp`.

**Steps**
1. Fix `g_fpu_sink` (and `g_sink`) so they are not races.
2. Add a `SAN=thread|address,undefined` option to `tools/selftest.sh` that passes `-fsanitize=...` (and `-O1 -g`) to the engine and test builds; keep the default run unchanged. Set `TSAN_OPTIONS` so only intended exclusions remain.
3. Add CI jobs: one with `SAN=address,undefined`, one with `SAN=thread` on a reduced set of tests (TSan is slow; the quick suite on a tiny region is enough), excluding `rwstorm` with a comment saying why.
4. Add the unit test that runs `run_suite` on one thread and polls `status()` from another (see S6).
5. MSVC: add `/W4`, fix what it reports, then add `/WX` in CI only.
6. Extend the Windows smoke test to run `--list` and a short `--seed` reproducibility check.

**Done when:** the new CI jobs are green and each exclusion has a comment explaining why.

---

## Notes for later sessions

- Out of scope for this plan: new test algorithms, GUI redesign, ARM support (the engine requires x86 AVX2 by design).
- Audit coverage: the second pass also read `algo_hammer`, `algo_march`, `algo_rwstorm`, `algo_evolve`, `algo_copy`, `algo_probe`, `algo_coherency`, `algo_fill`, `algo_chase`, `inject.cpp`, `tools/selftest.sh` and the relevant parts of `main_gui.cpp`. That pass found S13, the TSan findings and the corrections above. Not yet reviewed in depth: `gen.cpp` beyond the first 80 lines, `suite_data.cpp` test contents, most of `tools/unit_tests.cpp`, and the Windows paths (never run).
- Not run in the second pass: the full `selftest.sh`, anything on Windows, and TSan beyond four tests (rwstorm, coherency, disturb, `--fpu`).
