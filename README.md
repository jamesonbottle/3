# DDR5 RAM Test

A memory tester for DDR5 systems, written in C++20 and built with CMake. The Windows build is one native Win32 executable (GUI) plus a console tool built from the same engine; the console tool also builds and runs on Linux. No installer, no runtime, **no config files** - the whole test suite is compiled in.

The window follows the familiar two-tab layout (Testing / Advanced) used by popular RAM testers: system status, megabytes and threads, beep/stop on error, duration / coverage / error count, a big Start button, and Advanced options for CPU cache mode, RNG and an experimental FPU stress. There is no About page, no telemetry and no licence check.

**Requirements:** a CPU with AVX2 (Intel Haswell / 4th generation Core, AMD Excavator / Ryzen or newer) and an OS that has the AVX register state enabled (Windows 7 SP1 or newer). The program checks this before it does anything else: on a machine without it the GUI shows a message box and the console tool prints the reason to stderr and exits 2, instead of crashing with an illegal instruction. The check is `src/cpu_check.cpp`, the one source file built without AVX2 (its own small library in `CMakeLists.txt`), because the compiler may use AVX2 in any file that has it enabled.

## Use

1. Run `ddr5ramtest.exe`. Close other programs first. Only one copy runs at a time (two tests would fight over the same free memory): starting a second copy brings the first window forward instead, or says so in a message box when the other copy is in another user session. The console tool is not covered by this.
2. Pick *Threads* (default: the logical CPUs the program may use: a start with a CPU restriction such as `start /affinity 3 ...`, or a job object that limits the processor groups, is followed, not the machine's CPU count; a job's CPU *rate* cap is not). Thread N is pinned to the N-th allowed CPU (across processor groups) and wraps round when there are more threads than CPUs, as on Linux. *Megabytes* fills itself with ~90 % of free RAM (at least 512 MB always stay free for Windows and the window itself), split evenly per thread, and follows the thread count; type a value to override it, or clear the box to go back to automatic.
3. Press **Start**. It runs until you press **Stop**. Coverage is how many times the tested region has been verified (100 % = one full pass; it keeps counting past that).
4. Any error is written to `ddr5ramtest.log` next to the exe (or to `%TEMP%` if that folder is not writable - the status line says which), with address (and the line's byte offset in the tested region, which stays the same between runs with the same seed and size), expected/actual data, which DQ lane/beat was hit, which x8-device on-die-ECC codeword that would be *if* the DIMM uses x8 chips and the controller does not swizzle lanes (a heuristic, see *Honest limits*), and whether a re-read shows it as transient, stable or unstable.

*Stop on error* is ticked by default, so the run ends at the first error; untick it to keep going and collect statistics (error counts per lane/beat/address bit). The console tool defaults to the opposite and reports its result in the exit code (0 = clean, 1 = errors found, 2 = bad option or option value / could not start, 3 = internal error; a typo such as `-mb abc` is rejected, not silently replaced by a default); the console tool appends errors to `ddr5test.log` in the current folder, or to the file named by `--log FILE`: if that file cannot be opened the tool prints the reason and exits 2 before allocating memory when `--log` was given, and only warns (errors then go to the console only) when it is the default name; Ctrl-C stops the console tool cleanly (current step ends, summary is printed) and a second Ctrl-C kills it at once, so a wedged run can always be ended; with `--test-seconds N` it caps the timed tests, and when its output is piped it prints a progress line every 30 s instead of the live line. The live line shows the error count of the whole run followed by `(this test N)`, so earlier failures stay visible in keep-going mode. Stopping a run (Ctrl-C, Stop, `--coverage`, the time limit) is a hard stop: it ends the current step at once, including the page shuffle's final verify of all pages, without verifying data that was not fully written, so a stop never reports errors on healthy memory (pages not yet compared are simply not checked). The start-up can be stopped too: locking the memory (the first touch of every page) is done in 64 MB slices with a stop check between them, and the first-touch zeroing after it likewise; the one exception is a large-page allocation on Windows, which is a single OS call that cannot be interrupted. In the GUI, neither Stop nor closing the window ever blocks the window: both only ask the run to stop, and closing the window waits for the run to end (the status line says "Stopping, then closing..."). If the stop is still not done after 3 seconds (for example during a large-page allocation), the Stop button turns into **Force quit** and a second click on the window's close button does the same: the program ends at once, like the console tool's second Ctrl-C (error lines are already in the log, they are flushed as they are written). If the run cannot even be created (no memory for it, or Windows refuses a new thread), the GUI shows the reason in a message box, writes a `run did not start` line to the log and stays usable: the window is only switched to "running" once the worker exists.

**Memory size.** The size is read from the free memory when the run starts: automatic = 90 % of it, always leaving at least 512 MB; a size you give (`-mb`, the *Megabytes* box) is capped at 97 % of the free memory, leaving at least 256 MB, and, on Windows, at what the system can still commit (RAM + page file) minus 256 MB; the log says when it was reduced. If the OS still refuses the allocation (another program took the memory in the meantime), the run does not end with "could not run": it logs `allocation of N MB failed; trying M MB` and retries at 90 %, 80 %, ... down to 20 % of the planned size (each rounded to whole 2 MB pieces per thread), then runs on the size that worked and says `testing X GiB instead of the Y GiB planned`; the log header and the coverage line use the real size. Only when every size is refused does it exit 2 (`allocation failed at every size ...`). Pressing Stop during the retries ends the run instead of trying smaller. A run on less memory than planned is a smaller test, not a failed one, so check the `testing ... instead of` line in a soak that must cover a given size.

**Seed.** Every run picks a random seed and prints it first thing (`run seed: 0x... (random)`); it is also in the log header and repeated in the failure summary. The seed decides every random choice - hash data, random access orders, disturb traffic, page-shuffle patterns, pointer-chase layout - and each loop of a long run derives fresh ones from it, so a soak never repeats itself. To repeat a run, give the console tool `--seed 0x...` (decimal also works) with the same `-mb` and `-t`, which decide the data layout. A seed reproduces the data and access order, **not** thread timing or where the OS puts physical pages, so the timed tests (page shuffle, cache coherency) and marginal hardware can still behave differently from run to run.

Advanced tab
- **CPU cache**: Default (each test decides), Bypass (clflush every access so reads come from DRAM), Cached, Non-temporal (streaming stores). Flushing uses CLFLUSHOPT when the CPU has it (several times faster than CLFLUSH, which serialises), otherwise CLFLUSH; the run header names the instruction. CLI: `--flush clflush` forces plain CLFLUSH (for comparison), `--flush clflushopt` / `auto` (default) use CLFLUSHOPT when available.
- **RNG**: generator used by the hash tests.
- **Test set**: Full (28 tests) or Quick (the 12 tests marked quick). *This is an addition to the reference layout.*
- **Stress FPU**: AVX load on every thread - more power and heat, for testing marginal supply.

## What is tested

28 tests (several of them list sweeps, so one entry covers many combinations; 3 are modelled on Google's stressapptest and 1 is the evolving-data test) built around how DDR5 behaves: BL16 64-byte bursts over two 32-bit subchannels, bank groups (tCCD_L/S, tRRD, tFAW), row open/close (tRCD/tRP/tRAS/tRC), bus turnaround (tWTR/tRTW), long idle gaps between write and verify (refresh keeps running during them, so this is an idle soak, not a retention-time test), DQ lane crosstalk and pattern-sensitivity styles, address-decode tests, pointer-chase latency stress, and read/write mixes. Read `src/suite_data.cpp` for the full list; each entry has a name.

Ideas taken from [stressapptest](https://github.com/stressapptest/stressapptest) (reimplemented from its public description, no code copied):
- **Pattern table**: walking ones / inverse-alternating ones / zeros, 0/1, 5/A, 8b10b-style, "busy" patterns, each held for 1/2/4/8 words (32 to 256-bit bus-width repeat) and inverted too.
- **Page shuffle**: all memory in 4 KB pages with random patterns; threads copy pages onto each other (verifying the source line by line), invert pages in place four times (up, down, down, up), refill and re-check them, mixing cache-hot and flushed accesses, then verify every page. Data keeps moving between cores and channels while staying predictable.
- **Cache coherency**: threads share a few cache lines, each owning one byte; sums are checked after every round. This tests the cores' caches and fabric, not DRAM cells.
- Closed-source testers' internals are not public, so nothing here is derived from them.

Not from stressapptest:
- **Evolving data**: every other data test writes a pattern once or rewrites identical data, so a write that never lands leaves exactly the bytes the test expects. Here each 64-byte line has its own update period (every 1 to 8 passes) and a new generation of hash data each time, so memory always holds a mix of generations. After every pass *every* line is checked against its exact expected generation: a rewritten line that still shows its previous data is a lost or misdirected write, and a line that was not rewritten but changed is a stray write or disturb. The expected data is computed from (line, generation, seed), nothing is stored on the side.

## Honest limits

- User-mode software cannot read or set DDR5 timings, voltage or temperature, and cannot see physical addresses. These tests *stress access patterns that exercise those timings*; they do not verify a timing value.
- On-die ECC can silently correct single-bit errors, so a clean run does not prove perfect cells. Errors that do appear are the ones ODECC could not hide.
- The lane / beat / codeword attribution assumes cache-line byte order equals burst order and (for the codeword) x8 DRAM chips. Memory-controller data scrambling and board lane swaps are invisible to user-mode code, so treat these as hints for comparing runs, not as a map of the hardware. The same scrambling means the "DQ lane" patterns are not guaranteed to appear on the wire as named.
- **Coverage** counts verified bytes (100 % = as many bytes verified as the tested region holds). The windowed tests rotate over windows that together cover the whole region, including its tail, but the counter itself does not track which addresses were touched.
- Results were checked with the self-test below (unit tests, clean runs, a fault-injection build whose planted bit flips must be found) on Linux, **not on real DDR5 hardware**. No claim is made that this finds errors faster than any other tester; compare on your own failing configurations.

## Self-test

```
tools/selftest.sh        # Linux, g++ 11+ with AVX2; takes a few minutes; exits non-zero on any failure
ctest --output-on-failure   # from the CMake build directory: runs unit_tests (seconds), then selftest.sh (label "slow"; ctest -LE slow skips it)
SAN=address,undefined tools/selftest.sh   # the same checks with everything built under AddressSanitizer + UBSan (any report fails the run)
SAN=thread tools/selftest.sh              # ... under ThreadSanitizer; the read/write storm tests are skipped, they race by design
```

It builds the engine twice (normal, and with `-DDDR5TEST_INJECT`, which plants known bit flips between write and verify), then checks that address orders are true permutations, that the rotating windows cover the whole region, that every hash mixer (splitmix, murmur, xorshift64*) avalanches, that one seed reproduces every random choice, that clean runs of 15 representative tests report nothing (each with a fresh random seed, which a failure prints so it can be repeated), that bad option values (including `--seed`) are rejected, that the coverage counter is exact on a tiny region, that the printed seed is accepted back and reproduces the data of a run, and that the AVX2 gate stops the tester with exit 2 (and starts nothing) when the CPU or OS is made to look like one without AVX2, and that the injected faults are found exactly (42 lines / 44 bits in the pattern test, a lost write plus a stray bit flip in the evolving-data test, plus the page-shuffle and coherency injections) without hanging in stop-on-error mode. More checks stop the page shuffle in the middle of its fill phase and in its final verify, and the bulk copy in the middle of its fill, and require that nothing false (and, after a hard stop, nothing from the skipped verify) is reported; a last group sends SIGINT at four moments to every one of the 28 tests and requires an exit within `STOPWAIT` seconds (default 5) with exit code 0 and no error. Every run is wrapped in `timeout` (default 120 s, `TMO=` to change), so a hang fails the self-test instead of stalling CI. It runs in CI on every push, also under the sanitizers. The portable part of the injection checks lives in `tools/inject_checks.sh` (`bash tools/inject_checks.sh PATH_TO_ddr5test_cli_inject`); the Windows CI job builds `unit_tests` and `ddr5test_cli_inject` with MSVC, runs `ctest -R unit_tests` and that script, so a planted fault the MSVC build fails to report turns CI red; it also starts the console tool with `start /affinity 2` and requires one thread and no pinning warning. It proves the detection logic; it cannot prove anything about a particular DIMM.

## Build

The engine is ordinary C++20 split into translation units under `src/`, built with CMake (3.20+). With MSVC (Visual Studio Build Tools) run in an *x64 Native Tools* prompt:

```
build.bat
```

Output in `build\`: `ddr5ramtest.exe` (GUI) and `ddr5test_cli.exe` (same engine, console, for headless runs: `ddr5test_cli --help`). MinGW and Linux (console tool only) use CMake directly:

```
cmake -S . -B build/cmake -G Ninja            # MinGW: add -DCMAKE_CXX_COMPILER=g++ ; the GUI target exists on Windows only
cmake --build build/cmake                     # executables are placed in build/
```

GitHub Actions builds both on `windows-latest` and uploads them as artifacts.

## Changing the suite

The tests are plain C++ data in `src/suite_data.cpp` (key documentation at the top of the file). Edit an entry and rebuild; there is no config file, no generator and no Python. `tools/selftest.sh` fails on a misspelled key.

## Layout

```
src/types.h        u8..u64, the 64-byte Line, bit helpers
src/platform.h     OS layer (plat::)      src/platform_win.cpp  src/platform_posix.cpp
src/log.h/.cpp     Logger (output sink)            src/params.h/.cpp     string helpers, Params, TestDef
src/seed.h/.cpp    run seed, hash mixer   src/order.h/.cpp      address orders
src/gen.h/.cpp     data generators        src/lineio.h          64-byte store/load/verify (templates)
src/run.h/.cpp     Run, walk, error report
src/algo.h/.cpp    shared algo helpers, typed per-algo options (FillOpts ...), the algos() registry;  src/algo_*.cpp  one file per test algorithm
src/suite.h/.cpp   run_suite() (steps in SuiteDriver), failure summary, FPU stress;  src/suite_data.cpp  the 28 built-in tests
src/inject.h/.cpp  fault-injection hooks (DDR5_INJECT) for the self-test; inject.cpp is only in the injection engine
src/main_gui.cpp   Win32 GUI              src/main_cli.cpp      console front-end
src/stop_gate.h    rules of the GUI's Stop button / window close (header only, unit-tested)
src/start_guard.h  the steps of the GUI's run start that can throw, run guarded (header only, unit-tested)
src/app.rc, app.manifest   visual styles + DPI awareness
CMakeLists.txt             engine static library (built twice: normal + fault injection), front-ends, unit tests
tools/unit_tests.cpp       unit tests of orders / windows / generators / hash mixers / seeds / evolving data / suite parsing
tools/selftest.sh          unit tests + clean runs + fault injection (see Self-test)
tools/inject_checks.sh     the portable fault-injection checks, shared by selftest.sh (Linux) and the Windows CI job (MSVC build, Git Bash)
tools/format.sh            clang-format all sources (--check in CI)
```
