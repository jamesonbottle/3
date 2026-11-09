# DDR5 RAM Test

A single-file Windows memory tester for DDR5 systems. One native Win32 executable, no installer, no runtime, **no config files** - the whole test suite is compiled in.

The window follows the familiar two-tab layout (Testing / Advanced) used by popular RAM testers: system status, megabytes and threads, beep/stop on error, duration / coverage / error count, a big Start button, and Advanced options for CPU cache mode, RNG and an experimental FPU stress. There is no About page, no telemetry and no licence check.

## Use

1. Run `ddr5ramtest.exe`. Close other programs first.
2. Pick *Threads* (default: logical CPUs). *Megabytes* fills itself with ~90 % of free RAM, split evenly per thread, and follows the thread count; type a value to override it, or clear the box to go back to automatic.
3. Press **Start**. It runs until you press **Stop**. Coverage is how many times the tested region has been verified (100 % = one full pass; it keeps counting past that).
4. Any error is written to `ddr5ramtest.log` next to the exe, with address, expected/actual data, which DQ lane/beat and which 128-bit on-die-ECC word was hit, and whether a re-read shows it as transient, stable or unstable.

Advanced tab
- **CPU cache**: Default (each test decides), Bypass (clflush every access so reads come from DRAM), Cached, Non-temporal (streaming stores).
- **RNG**: generator used by the hash tests.
- **Test set**: Full (38 tests) or Quick (skips long pauses and row hammer). *This is an addition to the reference layout.*
- **Stress FPU**: AVX load on every thread - more power and heat, for testing marginal supply.

## What is tested

38 tests (87 consolidated into 35 by turning near-duplicates into list sweeps, plus 3 modelled on Google's stressapptest) built around how DDR5 behaves: BL16 64-byte bursts over two 32-bit subchannels, bank groups (tCCD_L/S, tRRD, tFAW), row open/close (tRCD/tRP/tRAS/tRC), bus turnaround (tWTR/tRTW), refresh and retention (tREFI/tRFC/tREFW pauses), row-hammer-style activation patterns, DQ lane crosstalk and pattern-sensitivity styles, address-decode tests, pointer-chase latency stress, and read/write mixes. Read `suite.cfg` for the full list; each block has a name.

Ideas taken from [stressapptest](https://github.com/stressapptest/stressapptest) (reimplemented from its public description, no code copied):
- **Pattern table**: walking ones / inverse-alternating ones / zeros, 0/1, 5/A, 8b10b-style, "busy" patterns, each held for 1/2/4/8 words (32 to 256-bit bus-width repeat) and inverted too.
- **Page shuffle**: all memory in 4 KB pages with random patterns; threads copy pages onto each other (verifying the source line by line), invert pages in place four times (up, down, down, up), refill and re-check them, mixing cache-hot and flushed accesses, then verify every page. Data keeps moving between cores and channels while staying predictable.
- **Cache coherency**: threads share a few cache lines, each owning one byte; sums are checked after every round. This tests the cores' caches and fabric, not DRAM cells.

## Honest limits

- User-mode software cannot read or set DDR5 timings, voltage or temperature, and cannot see physical addresses. These tests *stress access patterns that exercise those timings*; they do not verify a timing value.
- On-die ECC can silently correct single-bit errors, so a clean run does not prove perfect cells. Errors that do appear are the ones ODECC could not hide.
- Results were checked with a fault-injection build (injected bit flips are detected) and a Linux run, **not on real DDR5 hardware**. No claim is made that this finds errors faster than any other tester; compare on your own failing configurations.
- Closed-source testers' internals are not public, so nothing here is derived from them.

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
```
