# DDR5 RAM Test

A single-file Windows memory tester for DDR5 systems. One native Win32 executable, no installer, no runtime, **no config files** - the whole test suite is compiled in.

The window follows the familiar two-tab layout (Testing / Advanced) used by popular RAM testers: system status, megabytes and threads, beep/stop on error, duration / coverage / error count, a big Start button, and Advanced options for CPU cache mode, RNG and an experimental FPU stress. There is no About page, no telemetry and no licence check.

## Use

1. Run `ddr5ramtest.exe`. Close other programs first.
2. Pick *Threads* (default: logical CPUs). *Megabytes* fills itself with ~90 % of free RAM, split evenly per thread, and follows the thread count; type a value to override it, or clear the box to go back to automatic.
3. Press **Start**. It runs until you press **Stop**. Coverage is how many times the tested region has been verified (100 % = one full pass; it keeps counting past that).
4. Any error is written to `ddr5ramtest.log` next to the exe (or to `%TEMP%` if that folder is not writable - the status line says which), with address, expected/actual data, which DQ lane/beat was hit, which x8-device on-die-ECC codeword that would be *if* the DIMM uses x8 chips and the controller does not swizzle lanes (a heuristic, see *Honest limits*), and whether a re-read shows it as transient, stable or unstable.

*Stop on error* is ticked by default, so the run ends at the first error; untick it to keep going and collect statistics (error counts per lane/beat/address bit). The console tool defaults to the opposite and reports its result in the exit code (0 = clean, 1 = errors found, 2 = bad option or option value / could not start, 3 = internal error; a typo such as `-mb abc` is rejected, not silently replaced by a default); with `--test-seconds N` it caps the timed tests, and when its output is piped it prints a progress line every 30 s instead of the live line.

**Seed.** Every run picks a random seed and prints it first thing (`run seed: 0x... (random)`); it is also in the log header and repeated in the failure summary. The seed decides every random choice - hash data, random access orders, disturb traffic, page-shuffle patterns, pointer-chase layout - and each loop of a long run derives fresh ones from it, so a soak never repeats itself. To repeat a run, give the console tool `--seed 0x...` (decimal also works) with the same `-mb` and `-t`, which decide the data layout. A seed reproduces the data and access order, **not** thread timing or where the OS puts physical pages, so the timed tests (page shuffle, cache coherency) and marginal hardware can still behave differently from run to run.

Advanced tab
- **CPU cache**: Default (each test decides), Bypass (clflush every access so reads come from DRAM), Cached, Non-temporal (streaming stores).
- **RNG**: generator used by the hash tests.
- **Test set**: Full (28 tests) or Quick (the 12 tests marked quick). *This is an addition to the reference layout.*
- **Stress FPU**: AVX load on every thread - more power and heat, for testing marginal supply.

## What is tested

28 tests (87 consolidated into 35 by turning near-duplicates into list sweeps, audited for usefulness and trimmed, plus 3 modelled on Google's stressapptest and 1 evolving-data test) built around how DDR5 behaves: BL16 64-byte bursts over two 32-bit subchannels, bank groups (tCCD_L/S, tRRD, tFAW), row open/close (tRCD/tRP/tRAS/tRC), bus turnaround (tWTR/tRTW), long idle gaps between write and verify (refresh keeps running during them, so this is an idle soak, not a retention-time test), DQ lane crosstalk and pattern-sensitivity styles, address-decode tests, pointer-chase latency stress, and read/write mixes. Read `suite.cfg` for the full list; each block has a name.

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
```

It builds the engine twice (normal, and with `-DDDR5TEST_INJECT`, which plants known bit flips between write and verify), then checks that address orders are true permutations, that the rotating windows cover the whole region, that every hash mixer (splitmix, murmur, xorshift64*) avalanches, that one seed reproduces every random choice, that clean runs of 15 representative tests report nothing (each with a fresh random seed, which a failure prints so it can be repeated), that bad option values (including `--seed`) are rejected, that the coverage counter is exact on a tiny region, that the printed seed is accepted back and reproduces the data of a run, and that the injected faults are found exactly (42 lines / 44 bits in the pattern test, a lost write plus a stray bit flip in the evolving-data test, plus the page-shuffle and coherency injections) without hanging in stop-on-error mode. One more check stops the page shuffle in the middle of its fill phase and requires that only the planted flip is reported (pages that were never filled must not be verified). Every run is wrapped in `timeout` (default 120 s, `TMO=` to change), so a hang fails the self-test instead of stalling CI. It runs in CI on every push. It proves the detection logic; it cannot prove anything about a particular DIMM.

## Build

Needs MSVC (Visual Studio Build Tools) - run in an *x64 Native Tools* prompt:

```
build.bat
```

Output in `build\`: `ddr5ramtest.exe` (GUI) and `ddr5test_cli.exe` (same engine, console, for headless runs: `ddr5test_cli --help`). MinGW also works:

```
windres src/app.rc -O coff -o app.res.o
g++ -std=c++20 -O2 -mavx2 -mwindows -static -pthread -Isrc src/main_gui.cpp app.res.o -o ddr5ramtest.exe -lcomctl32 -lgdi32 -luser32 -ladvapi32
```

GitHub Actions builds both on `windows-latest` and uploads them as artifacts.

## Changing the suite

Edit `suite.cfg` (format documented at the top), run `python tools/gen_suite.py`, rebuild. The generated `src/suite_data.h` is committed so a normal build needs no Python.

## Layout

```
src/engine.h       test engine (header-only)
src/main_gui.cpp   Win32 GUI          src/main_cli.cpp   console front-end
src/suite_data.h   generated from suite.cfg
src/app.rc, app.manifest   visual styles + DPI awareness
tools/gen_suite.py         suite.cfg -> suite_data.h
tools/unit_tests.cpp       unit tests of orders / windows / generators / hash mixers / seeds / evolving data / suite parsing
tools/selftest.sh          unit tests + clean runs + fault injection (see Self-test)
```
