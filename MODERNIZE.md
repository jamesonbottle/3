# Modernization plan (resumable)

Goal: make the engine easy to maintain **without changing what it does**. One small step at a time, each step its own commit,
each verified by the self-test, so work can stop after any step and be picked up by a different person or Claude session.

Baseline: branch `modular-engine`, commit `a484feb` (engine.h split into translation units). `tools/selftest.sh` passed there with
53 check lines PASS (`grep -c "^  PASS"`), 0 FAIL, and the engine built with no warnings under `-Wall -Wextra`.

## How to resume (read this first)

1. `git fetch origin && git checkout modernize && git pull`. Work only on branch `modernize` (stacked on `modular-engine`).
2. Open the **Status** table below. The next step is the first row that is not `done`. Read that item's section and nothing else.
3. Do exactly that step. Do not start the next one, do not "fix while you are there". Anything noticed on the side goes into **Parking lot**.
4. Run the **gate** (below). It must pass before you commit.
5. Commit with the message `modernize M<n>: <what>` (+ the usual co-author trailer), tick the boxes of the item, set its Status to `done`,
   add one line to the **Log**, commit that update in the same commit, and `git push origin modernize`.
6. If you must stop in the middle of an item: leave the working tree committed on a green gate, set Status to `partial`,
   and write in the Log exactly which sub-step is next. Never leave the branch red.

## Gate (every step must pass)

```
tools/selftest.sh                       # ~4 min, 2+ CPUs, g++ 11+ with AVX2. Must end with SELFTEST PASSED, 0 FAIL, `grep -c "^  PASS"` of its output not lower than before (53 at baseline; the final "SELFTEST PASSED" line is not counted)
grep -ciE "warning|error" "$OUT/build.log"   # selftest already fails on this; the engine must stay warning-free
```

Quick loop while working (seconds): `cmake -S . -B build/cmake && cmake --build build/cmake -j && build/unit_tests`.
Windows cannot be built in this environment. Windows-only files can be compile-checked with `pip install ziglang` then
`python3 -m ziglang c++ -target x86_64-windows-gnu -std=c++20 -mavx2 -Isrc -c src/platform_win.cpp -o /tmp/x.obj` (also `src/main_gui.cpp`).

## Rules for every step

- Behaviour must not change: same log text (selftest greps it), same exit codes, same `--seed` reproduction, same injected-fault counts.
- One concern per commit. A formatting-only or rename-only commit contains nothing else.
- Hot paths (`Order::at/pos`, `Gen::make`, `IO<>`, `walk`, `mix64`, `Run::vtick`) must stay inline and must not gain a branch or an indirect call.
- Keep `tools/unit_tests.cpp` compiling: it uses `Run R(test_log); R.T = 1; R.chunks.push_back(...)` (`Run` needs a `const Logger&`), `Order`, `Gen`, `sat_table`, `algos()`, `parse_march_variants`,
  `evo_period/evo_make`, `parse_cfg_stream`, `suite_text`, `salt_for/g_salt/fresh_seed`, `g_rng/mix64`. When a step changes one of these, update the test in the same commit.

## Status

| Item | What | Status |
|------|------|--------|
| M1 | Formatting + lint config (was point 5) | done |
| M2 | Namespace `ddr5` (point 2) | done |
| M3 | Generated `suite_data.h` cannot drift (point 8) | done |
| M4 | Fault-injection hooks out of the algorithms (point 6) | done |
| M5 | Remove global mutable state (point 1), sub-steps 5a-5c | done |
| M6 | Split up `Run` (point 3) | done |
| M7 | Typed, validated test parameters (point 4), sub-steps 7a-7j | partial (7a-7h done) |
| M8 | Test runner: CTest (+ optional framework) (point 7) | todo |

Order matters: M1 first so later diffs are in the final style; M2 before M5/M6 so new types land in the namespace; M5 before M6 and M7.

## M1 - Formatting + lint config

Why: lines of 150-250 characters with several statements are hard to review and diff.

- [x] Add `.clang-format` (based on LLVM, 4-space indent, `ColumnLimit: 120`, `AllowShortFunctionsOnASingleLine: Inline`, `AllowShortLambdasOnASingleLine: Inline`, `SortIncludes: Never`: include order is hand-kept because Windows headers depend on it, e.g. `windows.h` before `commctrl.h`).
- [x] Add `.clang-format-ignore` listing `src/suite_data.h` (generated).
- [x] Add `tools/format.sh` (`tools/format.sh` rewrites, `tools/format.sh --check` fails if anything would change).
- [x] Reformat `src/*.h src/*.cpp tools/unit_tests.cpp` in ONE commit that changes nothing else.
- [x] Add that commit's hash to `.git-blame-ignore-revs` (second commit) and note `git config blame.ignoreRevsFile .git-blame-ignore-revs` in the README Layout/Build area.
- [x] CI (`.github/workflows/build.yml`, linux job): run `tools/format.sh --check` before the self-test.
- [x] Add `.clang-tidy` with a small set (`bugprone-*`, `performance-*`, `readability-misleading-indentation`, minus noisy ones) and `tools/tidy.sh` that runs it over `src/` using `compile_commands.json` (`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`). Report-only: do NOT gate on it, and do not fix its findings in this item (put them in the Parking lot).

Done when: `tools/format.sh --check` passes; every engine object file is byte-identical to the one built before the reformat
(compile each `src/*.cpp` except main_gui with `-O2 -mavx2 -std=c++20 -c`, compare `sha256sum`; formatting must not change code);
gate passes.

## M2 - Namespace `ddr5`

Why: `lower`, `trim`, `emit`, `walk`, `Order`, `Run`, `Line` ... live in the global namespace.

- [x] Wrap the contents of every `src/*.h` / `src/*.cpp` except `main_*.cpp` in `namespace ddr5 { ... }` (keep macros such as `CBAR` and `#include`s outside). `plat::` becomes `ddr5::plat`.
- [x] `main_cli.cpp`, `main_gui.cpp`, `tools/unit_tests.cpp`: `using namespace ddr5;` after the includes (qualify instead where it is one or two names).
- [x] Windows: `platform_win.cpp` and `main_gui.cpp` still compile (zig check above); `app.rc` is unaffected.

Done when: `nm -C build/libddr5engine.a | grep ' [TDBR] ' | grep -v 'ddr5::'` lists nothing the project defines (only compiler/std symbols); gate passes.

## M3 - Generated `suite_data.h` cannot drift

Why: it is generated from `suite.cfg` and committed; nothing notices if someone edits one and not the other.

- [x] Add `--check` to `tools/gen_suite.py`: generate in memory, compare with `src/suite_data.h`, exit 1 with "run python tools/gen_suite.py" if different. Keep the default behaviour (write the file) unchanged.
- [x] Run it from `tools/selftest.sh` (a check line `suite_data.h is up to date with suite.cfg`) and from the Windows CI job is NOT needed (Linux job covers it).
- [x] A unit test already parses `suite_text()`; leave it.

Done when: editing `suite.cfg` without regenerating makes `tools/selftest.sh` fail; gate passes.

## M4 - Fault-injection hooks out of the algorithms

Why: `#ifdef DDR5TEST_INJECT` blocks sit in the middle of `algo_fill/evolve/pagemix/coherency`.

- [x] Add `src/inject.h` with one macro per hook point, e.g. `DDR5_INJECT(after_fill, R, tid, ji, w)`. In a normal build it expands to nothing; with `DDR5TEST_INJECT` it calls `inject::after_fill(...)`. Hook points: fill (after first sync), evolve (last pass), pagemix (stop in fill, planted flip), coherency (extra increment).
- [x] Add `src/inject.cpp` holding the five injection bodies, moved verbatim from the algos. In CMake: compiled only into `ddr5engine_inject`.
- [x] The algos contain only the one-line hook calls, no `#ifdef`.

Done when: `grep -rn DDR5TEST_INJECT src/` finds only `inject.h`, `inject.cpp` and (if kept) the CMake file; the normal build's object files for the four algos contain no injection code; the self-test injection counts are unchanged (42 lines / 44 bits, 2 evolve lines, pagemix, coherency); gate passes.

## M5 - Remove global mutable state

Globals: `g_salt`, `g_rng` (seed), `g_emit`, `g_console`, `g_tty` (log). Do the three sub-steps as three separate commits.

- [x] **5a Output sink.** Replace `g_emit/g_console/g_tty` + free `emit/emitf/print_clear` with a `Logger` object (sink function, console flag, tty flag). The front-ends create it; `Run` holds a reference; `parse_cfg_stream` and `build_vars` take a `Logger&` (or a diagnostics callback). Same output bytes.
- [x] **5b Salt.** `g_salt` becomes `Run::salt` (set per loop in `run_suite`); `Order::init` and `build_vars` receive it as an argument instead of reading a global. Update the unit tests that set `g_salt`.
- [x] **5c RNG choice.** `g_rng` becomes a field of `Gen` (and a parameter of `evo_make`), copied from `Settings` when generators are built. `mix64(x)` keeps its hot-path switch but reads the field, not a global. Check `Gen::make` codegen did not get slower (run a 30 s `Pointer chase`/`Solid and checker` test before/after; GB/s within noise).

Done when: `grep -rn "^extern\|\bg_salt\b\|\bg_rng\b\|\bg_emit\b\|\bg_console\b\|\bg_tty\b" src/ tools/` finds nothing; `--seed` reproduction checks in the self-test (2c) still pass; gate passes.

## M6 - Split up `Run`

Why: `Run` holds config, counters, barrier, log file, stop flag and coverage as public members; every algo touches them freely.

- [x] **6a Group the fields** into small structs inside `Run` (`Progress`: done/vbytes/vtotal/coverage targets; `ErrorSink`: mutex, logf, logged, firstErr times; `Layout`: region/chunks/T). Pure renames of access paths, no behaviour change.
- [x] **6b Narrow the algo-facing surface.** Algos use only `view/dview/sync/tick/vtick/report/set_label/sleep_ms/stop/window_lines/parallel/winctr`; make everything else private (front-ends and unit tests get explicit accessors or `friend`).
- [x] **6c Make `Run` non-copyable and document ownership** (who sets `stop`, who owns `logf`).

Done when: no algo file reads or writes `R.cfg`, `R.logf`, `R.firstErr*`, `R.errTotal` directly; unit tests still build; gate passes.

## M7 - Typed, validated test parameters

Why: `Params` is `map<string,string>`; a misspelled key in `suite.cfg` is silently ignored.

- [x] **7a Key validation (cheap, do first).** Give every algo a list of the keys it understands (`algo.h`: `const std::set<std::string>& algo_keys(name)`); `run_suite` and a new unit test report any key in `suite.cfg` that no algo uses. Fix any real typos found (record them in the Log; if one changes what a test does, stop and ask).
- [x] **7b `FillOpts`** (fill). Pattern for the rest: the struct sits in `algo.h`, `from(P, log)` and `keys()` sit in the algo's `.cpp`; call `from()` right after `build_vars` so "bad ..." messages keep their order; `algo_keys()` takes the algo's own keys from `XOpts::keys()`; a unit test checks the defaults, the clamps and that every key in `keys()` changes the result.
- [x] **7c `MarchOpts`** (march).
- [x] **7d `HammerOpts`** (hammer).
- [x] **7e `CopyOpts`** (copy).
- [x] **7f `ChaseOpts`** (chase).
- [x] **7g `RwstormOpts`** (rwstorm).
- [x] **7h `EvolveOpts`** (evolve).
- [ ] **7i-7j Typed structs, one algo per commit**: probe, pagemix, coherency. Each gets `struct XOpts { ... static XOpts from(const Params&); }` parsed once before `R.parallel`; the algo body uses fields only. Defaults must equal the current `P.num("key", default)` values.
- [ ] Remove the now-unused `Params` accessors only after the last algo is converted.

Done when: no `P.num/P.str/P.flag/P.words/P.nums` call remains inside an `algo_*.cpp` (only in the `from()` functions); a misspelled key fails `tools/selftest.sh`; gate passes.

## M8 - Test runner

Why: `tools/unit_tests.cpp` uses a homegrown `CHECK` macro and a shell script is the only runner.

- [ ] Register `unit_tests` and the self-test with CTest (`enable_testing()`, `add_test`), so `ctest --output-on-failure` works. `tools/selftest.sh` stays as the full gate.
- [ ] Optional, ask the owner first: vendor `doctest.h` (single header, MIT) under `tools/third_party/` and turn each `test_*` function into a `TEST_CASE`. If declined, record "declined" in the Log and mark this item done after CTest.

Done when: `ctest` runs both and passes; gate passes.

## Parking lot (found while working; not part of any step)

- clang-tidy (tools/tidy.sh) at M1: 4 minor findings, none from `bugprone-*`: `Order::K` could be `uint8_t` (order.h), `lower(std::string s)` passes by value (params.cpp, fine: it modifies a copy), `emplace_back` in a loop without `reserve` (run.h `parallel`), string concatenation temporaries (suite.cpp ~201).
- M7b: `algo_fill` still hands `P` to the shared helpers `build_vars`, `Run::window_lines` (and `from()` hands it to `stride_list`/`ospec`); those read the keys `data`, `window_mb`, `stride`, `order`. That is not a `P.num/P.str/...` call inside the algo, so the M7 done-when holds, but typed `DataOpts`/window/stride structs could replace them after 7j (decide then).
- `Run::running` is written by `run_suite` (true/false around a run) and never read by anything; delete it (or use it) in a separate commit. `parallel()` also has a local `running` that shadows it.
- `run_suite` silently skips a test whose `algo` is not in `algos()` (a typo there is not reported at run time; only the unit test checks the built-in suite's algo names). Report it next to the new unused-key message, e.g. in M7a's loop or in M7b.
- `Cfg` has fields that nothing reads (`memPercent`, `memMB`, `marginMB`, `cacheMB`): delete in a separate commit after M5.
- `main_gui.cpp` calls `GlobalMemoryStatusEx` directly in three places; `plat::avail_phys/total_phys` could be used.
- `algo_hammer` and `algo_probe` have no built-in test using them (kept on purpose, see `suite.cfg`).
- Not verified here: a native MSVC build (`build.bat`); CI covers it on push.

## Log

- 2026-11-10 plan written.
- 2026-11-10 M1 done (commit "modernize M1: add .clang-format ..." + follow-up with blame-ignore, CI format check, tidy). Engine objects byte-identical before/after reformat; gate green. 
- 2026-11-10 M2 done. Every project symbol in libddr5engine(.inject).a is in `ddr5::` (nm check, std/compiler symbols filtered); `plat` is now `ddr5::plat`; main_cli/main_gui/unit_tests use `using namespace ddr5;`. Macros (`CBAR`) stay global. Windows files compile for x86_64-windows-gnu. Gate green. Next: M3.
- 2026-11-10 M3 done. `tools/gen_suite.py --check` (exit 1 on drift, default write unchanged and byte-identical) runs as the first check of selftest section 1, which needs `python3`. Gate green with 54 PASS lines (53 + the new check; the new minimum is 54), 0 FAIL, 0 build warnings. Verified: appending a line to `suite.cfg` without regenerating makes `tools/selftest.sh` exit 1 (FAIL on the new line). Next: M4.
- 2026-11-10 M4 done. Five hooks (`after_fill`, `evolve_last_pass`, `pagemix_fill_stop`, `pagemix_flip`, `coherency_extra`) in `src/inject.h`/`inject.cpp`; the `R.sync()` that sat inside each `#ifdef` moved into its hook, so a normal build has no extra barrier. Evolve passes `&io::store` as a function pointer (injection build only). `inject.cpp` is only in `ddr5engine_inject` and `#error`s without `DDR5TEST_INJECT`. All 19 normal-build objects (`-O2 -mavx2 -std=c++20`) are byte-identical to before (sha256), no `inject` symbols in `libddr5engine.a`; `grep -rn DDR5TEST_INJECT src/` finds only `inject.h`/`inject.cpp`. Gate green: 54 PASS, 0 FAIL, injection counts unchanged (42 lines / 44 bits, 2 evolve lines, pagemix, coherency), `tools/format.sh --check` clean. Windows files not rebuilt (hooks are `((void)0)` there). Next: M5.
- 2026-11-10 M5a done. `struct Logger { sink, console, tty; emit(), emitf(), print_clear() }` in `log.h`; the free functions and `g_emit/g_console/g_tty` are gone. `Run` holds `const Logger& log` (constructor `Run(const Logger&)`, so `unit_tests.cpp` now does `Run R(test_log)`); `build_vars`, `ospec`, `parse_march_variants`, `parse_cfg_stream` take a `const Logger&` as their LAST argument. CLI owns `static Logger log` (console/tty set after the `--list` branch, as before); GUI owns `A.logger` (declared before `A.run`). Same output bytes. Gate green: 54 PASS, 0 FAIL, 0 warnings, `format.sh --check` clean; Windows files (`platform_win`, `main_gui`, `main_cli`, `suite`, `run`) compile for x86_64-windows-gnu. Next: 5b (`g_salt` -> `Run::salt`; `Order::init` and `build_vars` take it as an argument; update the unit tests that set `g_salt`).
- 2026-11-10 M5b done. `g_salt` is gone: `Run::salt` (set per loop in `run_suite`); `Order::init(..., seed, salt)` and `build_vars(P, salt, log)` take it as an argument; algos read `R.salt`. `unit_tests.cpp` passes `0` where it relied on the default global and a local `salt` in `test_seed`. Same `--seed` output: the qword data of the injected evolve test for seeds 777/778 is byte-identical to the builds before 5b (compared with `cmp`). Gate green: 54 PASS, 0 FAIL, 0 warnings, `format.sh --check` clean, Windows files compile. Next: 5c (`g_rng` -> field of `Gen` and parameter of `evo_make`, copied from `Settings` when generators are built; then check `Gen::make` speed before/after with a 30 s `Pointer chase` and `Solid and checker`). Also resolve the `extern ALGOS` note in the Parking lot at the end of M5.
- 2026-11-10 M5c done. `g_rng` is gone: `Run::rng` (set in `run_suite` from `Settings::rngMode`) -> `Gen::rng` (copied into every Gen by `build_vars(P, salt, rng, log)`, which wraps `build_vars_impl`); `mix64(x, rng)` keeps its switch; `evo_make(g, gen, seed, rng, line)` takes the mixer (the inject hook uses `R.rng`). Unit tests pass the mode directly. Behaviour: seeded fault data for seed 777 is byte-identical to the 5b build for `--rng splitmix/murmur/xorshift` on both `Evolving data` (evo_make) and `Random-order hash soak` (Gen::make), and the three modes still differ from each other. Speed: `Gen::make` microbenchmark (40 M lines, best of 7, 2 interleaved rounds) within +-2% for every kind and mixer (hash 14.3-14.8 / 14.1-14.4 / 17.5-17.6 ns per line before vs after); end-to-end 30 s runs (`-mb 512 -t 2`): Solid and checker 1.2 / 1.2 GB/s, hash soak 0.9 / 0.9 (3 rounds), Pointer chase 1.0 before vs 0.9-1.0 after, but the normalized disassembly of `algo_chase` is identical before/after, so that spread is noise. Gate green: 54 PASS, 0 FAIL, 0 warnings, format clean, Windows files compile. Remaining for M5: `extern ALGOS` still matches the done-when grep (see Parking lot).
- 2026-11-10 M5 done. The `^extern` in the done-when grep matched `extern const std::map<...> ALGOS` (const, not mutable state); rather than loosening the grep it is now an accessor, `algos()` (function-local static table, same contents; used by `suite.cpp` and `unit_tests.cpp`, once per test, not on a hot path). `grep -rn "^extern\|\bg_salt\b\|\bg_rng\b\|\bg_emit\b\|\bg_console\b\|\bg_tty\b" src/ tools/` finds nothing; `--seed` reproduction checks (2c) pass. Gate green: 54 PASS, 0 FAIL, 0 warnings, format clean, Windows files compile. Next: M6 (split up `Run`; 6a group the fields, 6b narrow the algo-facing surface, 6c non-copyable + ownership docs). Note for M6: `Run` now also holds `log` (const ref), `salt`, `rng`.
- 2026-11-11 M6a done. `Run` now holds `Layout layout` (region, regionBytes, T, chunks), `Progress progress` (done, vbytes, vtotal, tStart, tTestStart, stopCoverage, stopSeconds, stopReason) and `ErrorSink errors` (mu, logf, logged, firstErrRun, firstErrTest, errTotal); access paths renamed everywhere (`R.T` -> `R.layout.T`, `R.done` -> `R.progress.done`, `R.logf` -> `R.errors.logf`, ...), nothing else changed. `errors.mu` also guards `label` (documented in `run.h`; kept as one mutex so locking is unchanged). Gate green: 54 PASS, 0 FAIL, 0 warnings, format clean, Windows files (`platform_win`, `main_gui`, `main_cli`, `suite`, `run`) compile for x86_64-windows-gnu. Next: 6b. Note: the algo-facing list in 6b is incomplete: besides `view/dview/sync/tick/vtick/report/set_label/sleep_ms/stop/window_lines/parallel/winctr` the algos also read `salt`, `rng`, `log`, `testName`, `layout.{T,region,regionBytes,chunks}` and add to `progress.done` (12 places, via `fetch_add`); keep those public (read-only, plus a small `add_done()`), and make `cfg`, `errors`, `st`/`total`, `bar`, `label`, `running`, the rest of `progress` private. The done-when grep (no algo touches `R.cfg`, `R.logf`, `R.firstErr*`, `R.errTotal`) already holds.
- 2026-11-11 M6b done. `Run` is now: public algo-facing part (`log`, `salt`, `rng`, `layout`, `testName`, `winctr`, `stop`; `vtick/sync/set_label/tick/add_done/view/dview/window_lines/sleep_ms/parallel/report`), a public front-end part (`Run::Status` + `status()` for the GUI refresh, `error_count()`; the CLI only needs `&R.stop`), and a private part (`cfg`, `progress`, `errors`, `st`, `total`, `bar`, `label`, `running`, `vflush`) that only `run_suite` (friend) and `Run`'s own methods can touch. The 8 `R.progress.done.fetch_add(n, relaxed)` calls in the algos became `R.add_done(n)` (same operation, inline). `main_gui.cpp` reads one `Status` snapshot instead of five separate fields. Compiler-checked: an algo that touches `R.cfg/errors/progress/label` no longer compiles (tried it); `grep -nE "R\.(cfg|logf|firstErr\w*|errTotal)" src/algo*.cpp` is empty. Unit tests needed no change (`R.layout.T` / `R.layout.chunks` stay public). Gate green: 54 PASS, 0 FAIL, 0 warnings, format clean, Windows files compile. Next: 6c (non-copyable `Run`, ownership notes: who sets `stop`, who owns `errors.logf`).
- 2026-11-11 M6 done. 6c: `Run(const Run&)` and `operator=` are deleted explicitly (it was already implicitly non-copyable via its atomics, mutex and reference member) and a `static_assert` in `run.h` keeps it copy- and move-proof; the ownership rules are written above `struct Run` (one `Run` per `run_suite` call; the front-end owns Run and Logger; `run_suite` fills in the setup, clears `stop` once and opens/closes `errors.logf`; who may set `stop`; `report()` runs on the workers under `errors.mu`; `parallel()` owns `bar`), each checked against the code. Done-when holds: `grep -nE "R\.(cfg|logf|firstErr\w*|errTotal)" src/algo*.cpp` is empty and the compiler rejects such access; unit tests unchanged. Gate green: 54 PASS, 0 FAIL, 0 warnings, format clean, Windows files compile. Next: M7 (typed, validated test parameters; start with 7a key validation).
- 2026-11-11 M7a done. `algo_keys(algo)` (`algo.h`/`algo.cpp`) lists the keys each algo reads (its own `P.*` reads plus the helpers: `build_vars` data keys, `stride_list`, `window_lines`, `ospec`), and `unknown_keys(TestDef)` returns the keys a test would have ignored. To tell a typo in a test block from a harmless `[default]` key, `TestDef` gained `own` (the keys its `[test]` block set itself): an own key must be in its algo's list; an inherited one (`width`, `flush`) only has to be known to some algo. `run_suite` prints `test '<name>': key '<k>' is not used by algo '<a>' (ignored)` (or `... (from [default]) is not used by any algo`) before selecting tests; a clean suite prints nothing, so no existing log text changed. New unit test `test_suite_keys` fails on any unknown key in the built-in suite and checks the detector on a typo in `[default]`, a typo in a test block and a key of another algo. Real typos found: none (all 28 tests clean). Verified by mutation: renaming one `window_mb` to `windw_mb` in `suite.cfg` fails `unit_tests` (so `tools/selftest.sh`) and prints the message in the CLI; reverted. Gate green: 54 PASS (the new test runs inside the existing unit-test check line), 0 FAIL, 0 warnings, format clean, Windows files compile. Caveat: the key table is hand-kept until 7b-7j; each of those commits should move that algo's keys into its `XOpts::from()` and delete its row. Next: 7b (typed `FillOpts`).
- 2026-11-11 M7b done. `FillOpts` (`algo.h`) with `from(P, log)` and `keys()` (`algo_fill.cpp`): the 11 keys fill reads itself (`order verify_order both cross width nt flush reads dwell_ms disturb disturb_passes`, plus `stride` through `stride_list`) are parsed once before `R.parallel`; `algo_fill` has no `P.num/str/flag/words/nums` call left (`grep -nE "P\.(num|str|flag|words|nums)" src/algo_fill.cpp` is empty). Defaults, the `max(1, ...)` clamps on `reads`/`disturb_passes`, the `dwell_ms` -> {0} and `disturb` -> {-1} fallbacks and the `verify_order` = `order` default are the old ones; `build_vars` still runs first, so the log lines keep their order. `algo_keys("fill")` now takes the fill keys from `FillOpts::keys()` (its hand-kept row is gone). New unit test `test_fill_opts` (defaults, an all-keys override, the clamps/fallbacks, and that every key in `keys()` changes the result, so a stale entry cannot hide a typo). Gate green: 54 PASS, 0 FAIL, 0 build warnings, format clean; the injection counts (42 lines / 44 bits in "Solid and checker", the fill-based tests) are unchanged. Windows files not rebuilt (no Windows-specific file touched). Next: 7c (typed `MarchOpts`; same pattern, see 7b).
- 2026-11-11 M7c done. `MarchOpts` (`algo.h`, `from()`/`keys()` in `algo_march.cpp`): `march burst order width nt flush` (+ `stride` via `stride_list`) parsed once before `R.parallel`; no `P.num/str/flag/words/nums` left in `algo_march`. Ordering kept: when `march` has no elements `from()` returns right after the march parse, so a bad `order` is still not reported before "march: no elements". The typed-options section of `algo.h` moved to the end of the file (it needs `MVar`); `FillOpts` moved with it. `unit_tests.cpp`: the "every listed key changes the result" check is now one helper (`check_opts_keys`, with an optional base config) used by fill and march; new `test_march_opts`. Gate green: 54 PASS, 0 FAIL, 0 warnings, format clean. Next: 7d (`HammerOpts`).
- 2026-11-11 M7d done. `HammerOpts` (`rowstride aggressors acts rounds`; `from(P)` has no log because hammer reports nothing while parsing). No built-in test uses hammer, so besides the gate I built the old (HEAD) and new engine in scratch copies with an extra `[test] algo = hammer` block (`rowstride = 64K`, `aggressors = 2`, `acts = 2000`, `rounds = 2`, `window_mb = 16`) and ran both with `--seed 0x1234`: exit 0 and identical output apart from the free-RAM line. New `test_hammer_opts`. Gate green: 54 PASS, 0 FAIL, 0 warnings, format clean. Next: 7e (`CopyOpts`).
- 2026-11-11 M7e done. `CopyOpts` (`method block`; `from(P)`): method words keep their case (the label prints them, the job lowercases them as before), `block` clamps to >= 64. New `test_copy_opts`. Covered by the self-test ("Bulk copy" clean run, coverage-counter and seed-reproduction checks). Gate green: 54 PASS, 0 FAIL, 0 warnings, format clean. Next: 7f (`ChaseOpts`).
- 2026-11-11 M7f done. `ChaseOpts` (`mlp seeds`; `from(P)`): the empty-`mlp` fallback to {12} and the `seeds >= 1` clamp are in `from()`; only the setup code of `algo_chase` changed, the chain build/walk loops are untouched. New `test_chase_opts`. Covered by the self-test ("Pointer chase" clean run). Gate green: 54 PASS, 0 FAIL, 0 warnings, format clean. Next: 7g (`RwstormOpts`).
- 2026-11-11 M7g done. `RwstormOpts` (`order verify_order width nt flush passes`, + `stride` via `stride_list`): the writer order defaults to `random`, the reader order to `seq` (NOT to `order`, unlike fill); `passes >= 1`; `from(P, log)` runs right after `build_vars`, so "bad order" lines keep their position. New `test_rwstorm_opts`. Covered by the self-test (the two "Read/write storm" clean runs). Gate green: 54 PASS, 0 FAIL, 0 warnings, format clean. Next: 7h (`EvolveOpts`).
- 2026-11-11 M7h done. `EvolveOpts` (`order verify_order width nt flush cross passes period`, + `stride` via `stride_list`; defaults: writes `random`, reads `seq`, 16 passes, periods 1..8; `passes`/`period` clamp to >= 1). The `evolve_last_pass` hook now passes `opt.passes` / `opt.period` (same values; the hook signature is unchanged). New `test_evolve_opts`. Gate green incl. the injection build: 54 PASS, 0 FAIL, 0 warnings, injected evolve counts unchanged (2 lines), format clean. Next: 7i (`ProbeOpts`).
