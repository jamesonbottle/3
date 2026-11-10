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
- Keep `tools/unit_tests.cpp` compiling: it uses `Run R; R.T = 1; R.chunks.push_back(...)`, `Order`, `Gen`, `sat_table`, `ALGOS`, `parse_march_variants`,
  `evo_period/evo_make`, `parse_cfg_stream`, `suite_text`, `salt_for/g_salt/fresh_seed`, `g_rng/mix64`. When a step changes one of these, update the test in the same commit.

## Status

| Item | What | Status |
|------|------|--------|
| M1 | Formatting + lint config (was point 5) | done |
| M2 | Namespace `ddr5` (point 2) | todo |
| M3 | Generated `suite_data.h` cannot drift (point 8) | todo |
| M4 | Fault-injection hooks out of the algorithms (point 6) | todo |
| M5 | Remove global mutable state (point 1), sub-steps 5a-5c | todo |
| M6 | Split up `Run` (point 3) | todo |
| M7 | Typed, validated test parameters (point 4), sub-steps 7a-7j | todo |
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

- [ ] Wrap the contents of every `src/*.h` / `src/*.cpp` except `main_*.cpp` in `namespace ddr5 { ... }` (keep macros such as `CBAR` and `#include`s outside). `plat::` becomes `ddr5::plat`.
- [ ] `main_cli.cpp`, `main_gui.cpp`, `tools/unit_tests.cpp`: `using namespace ddr5;` after the includes (qualify instead where it is one or two names).
- [ ] Windows: `platform_win.cpp` and `main_gui.cpp` still compile (zig check above); `app.rc` is unaffected.

Done when: `nm -C build/libddr5engine.a | grep ' [TDBR] ' | grep -v 'ddr5::'` lists nothing the project defines (only compiler/std symbols); gate passes.

## M3 - Generated `suite_data.h` cannot drift

Why: it is generated from `suite.cfg` and committed; nothing notices if someone edits one and not the other.

- [ ] Add `--check` to `tools/gen_suite.py`: generate in memory, compare with `src/suite_data.h`, exit 1 with "run python tools/gen_suite.py" if different. Keep the default behaviour (write the file) unchanged.
- [ ] Run it from `tools/selftest.sh` (a check line `suite_data.h is up to date with suite.cfg`) and from the Windows CI job is NOT needed (Linux job covers it).
- [ ] A unit test already parses `suite_text()`; leave it.

Done when: editing `suite.cfg` without regenerating makes `tools/selftest.sh` fail; gate passes.

## M4 - Fault-injection hooks out of the algorithms

Why: `#ifdef DDR5TEST_INJECT` blocks sit in the middle of `algo_fill/evolve/pagemix/coherency`.

- [ ] Add `src/inject.h` with one macro per hook point, e.g. `DDR5_INJECT(after_fill, R, tid, ji, w)`. In a normal build it expands to nothing; with `DDR5TEST_INJECT` it calls `inject::after_fill(...)`. Hook points: fill (after first sync), evolve (last pass), pagemix (stop in fill, planted flip), coherency (extra increment).
- [ ] Add `src/inject.cpp` holding the five injection bodies, moved verbatim from the algos. In CMake: compiled only into `ddr5engine_inject`.
- [ ] The algos contain only the one-line hook calls, no `#ifdef`.

Done when: `grep -rn DDR5TEST_INJECT src/` finds only `inject.h`, `inject.cpp` and (if kept) the CMake file; the normal build's object files for the four algos contain no injection code; the self-test injection counts are unchanged (42 lines / 44 bits, 2 evolve lines, pagemix, coherency); gate passes.

## M5 - Remove global mutable state

Globals: `g_salt`, `g_rng` (seed), `g_emit`, `g_console`, `g_tty` (log). Do the three sub-steps as three separate commits.

- [ ] **5a Output sink.** Replace `g_emit/g_console/g_tty` + free `emit/emitf/print_clear` with a `Logger` object (sink function, console flag, tty flag). The front-ends create it; `Run` holds a reference; `parse_cfg_stream` and `build_vars` take a `Logger&` (or a diagnostics callback). Same output bytes.
- [ ] **5b Salt.** `g_salt` becomes `Run::salt` (set per loop in `run_suite`); `Order::init` and `build_vars` receive it as an argument instead of reading a global. Update the unit tests that set `g_salt`.
- [ ] **5c RNG choice.** `g_rng` becomes a field of `Gen` (and a parameter of `evo_make`), copied from `Settings` when generators are built. `mix64(x)` keeps its hot-path switch but reads the field, not a global. Check `Gen::make` codegen did not get slower (run a 30 s `Pointer chase`/`Solid and checker` test before/after; GB/s within noise).

Done when: `grep -rn "^extern\|\bg_salt\b\|\bg_rng\b\|\bg_emit\b\|\bg_console\b\|\bg_tty\b" src/ tools/` finds nothing; `--seed` reproduction checks in the self-test (2c) still pass; gate passes.

## M6 - Split up `Run`

Why: `Run` holds config, counters, barrier, log file, stop flag and coverage as public members; every algo touches them freely.

- [ ] **6a Group the fields** into small structs inside `Run` (`Progress`: done/vbytes/vtotal/coverage targets; `ErrorSink`: mutex, logf, logged, firstErr times; `Layout`: region/chunks/T). Pure renames of access paths, no behaviour change.
- [ ] **6b Narrow the algo-facing surface.** Algos use only `view/dview/sync/tick/vtick/report/set_label/sleep_ms/stop/window_lines/parallel/winctr`; make everything else private (front-ends and unit tests get explicit accessors or `friend`).
- [ ] **6c Make `Run` non-copyable and document ownership** (who sets `stop`, who owns `logf`).

Done when: no algo file reads or writes `R.cfg`, `R.logf`, `R.firstErr*`, `R.errTotal` directly; unit tests still build; gate passes.

## M7 - Typed, validated test parameters

Why: `Params` is `map<string,string>`; a misspelled key in `suite.cfg` is silently ignored.

- [ ] **7a Key validation (cheap, do first).** Give every algo a list of the keys it understands (`algo.h`: `const std::set<std::string>& algo_keys(name)`); `run_suite` and a new unit test report any key in `suite.cfg` that no algo uses. Fix any real typos found (record them in the Log; if one changes what a test does, stop and ask).
- [ ] **7b-7j Typed structs, one algo per commit**: fill, march, hammer, copy, chase, rwstorm, evolve, probe/pagemix/coherency. Each gets `struct XOpts { ... static XOpts from(const Params&); }` parsed once before `R.parallel`; the algo body uses fields only. Defaults must equal the current `P.num("key", default)` values.
- [ ] Remove the now-unused `Params` accessors only after the last algo is converted.

Done when: no `P.num/P.str/P.flag/P.words/P.nums` call remains inside an `algo_*.cpp` (only in the `from()` functions); a misspelled key fails `tools/selftest.sh`; gate passes.

## M8 - Test runner

Why: `tools/unit_tests.cpp` uses a homegrown `CHECK` macro and a shell script is the only runner.

- [ ] Register `unit_tests` and the self-test with CTest (`enable_testing()`, `add_test`), so `ctest --output-on-failure` works. `tools/selftest.sh` stays as the full gate.
- [ ] Optional, ask the owner first: vendor `doctest.h` (single header, MIT) under `tools/third_party/` and turn each `test_*` function into a `TEST_CASE`. If declined, record "declined" in the Log and mark this item done after CTest.

Done when: `ctest` runs both and passes; gate passes.

## Parking lot (found while working; not part of any step)

- clang-tidy (tools/tidy.sh) at M1: 4 minor findings, none from `bugprone-*`: `Order::K` could be `uint8_t` (order.h), `lower(std::string s)` passes by value (params.cpp, fine: it modifies a copy), `emplace_back` in a loop without `reserve` (run.h `parallel`), string concatenation temporaries (suite.cpp ~201).
- `Cfg` has fields that nothing reads (`memPercent`, `memMB`, `marginMB`, `cacheMB`): delete in a separate commit after M5.
- `main_gui.cpp` calls `GlobalMemoryStatusEx` directly in three places; `plat::avail_phys/total_phys` could be used.
- `algo_hammer` and `algo_probe` have no built-in test using them (kept on purpose, see `suite.cfg`).
- Not verified here: a native MSVC build (`build.bat`); CI covers it on push.

## Log

- 2026-11-10 plan written.
- 2026-11-10 M1 done (commit "modernize M1: add .clang-format ..." + follow-up with blame-ignore, CI format check, tidy). Engine objects byte-identical before/after reformat; gate green. Next: M2.
