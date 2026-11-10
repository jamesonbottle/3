#!/usr/bin/env bash
# Reproducible self-test of the engine on Linux (needs g++ 11+ and an AVX2 CPU). Usage: tools/selftest.sh
#
#  1. unit tests       address orders are permutations, windows cover the whole chunk, suite.cfg parses, every hash mixer avalanches,
#                      one seed reproduces every random choice, evolving-data generations follow their rules
#  2. clean runs       the normal build must find NO errors (no false positives), exit code 0. Every run uses a fresh random seed
#                      (printed as "run seed: 0x..."; a failure prints it so the run can be repeated with --seed); 2c checks the seed itself
#  3. fault injection  a build with -DDDR5TEST_INJECT plants known bit flips between write and verify (and, in the evolving-data test, a lost
#                      write); the tester must report exactly those (exit code 1), and must not hang with --stop-on-error
#
# Exits non-zero on the first failed check group. Real DDR5 hardware is NOT needed (or exercised): this proves the detection logic, not the DIMMs.
set -u
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
OUT=${OUT:-$(mktemp -d)}
mkdir -p "$OUT"
RUN="-mb 64 -t 2 --no-large-pages --test-seconds 3"
TMO=${TMO:-120}   # every run of the tester is wrapped in timeout: a hang must fail the self-test (exit code 124), not stall CI
fail=0
ok()   { printf '  PASS  %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

echo "== build ($CXX, CMake) -> $OUT"
# unit_tests, ddr5test_cli (normal engine) and ddr5test_cli_inject (same sources compiled with -DDDR5TEST_INJECT), built by CMake with -Wall -Wextra.
cmake -S . -B "$OUT/cmake" -DCMAKE_CXX_COMPILER="$CXX" -DDDR5_OUTPUT_DIR="$OUT" > "$OUT/cmake.log" 2>&1 || { cat "$OUT/cmake.log"; exit 2; }
cmake --build "$OUT/cmake" --target unit_tests ddr5test_cli ddr5test_cli_inject -j "$(nproc 2>/dev/null || echo 2)" > "$OUT/build.log" 2>&1 || { cat "$OUT/build.log"; exit 2; }
grep -E "error|warning" "$OUT/build.log" && fail=1
mv -f "$OUT/ddr5test_cli" "$OUT/cli"; mv -f "$OUT/ddr5test_cli_inject" "$OUT/cli_inject"

echo "== 1. unit tests"
check "orders, windows, generators, suite parsing" 'timeout $TMO "$OUT/unit_tests" > "$OUT/unit.out" 2>&1 || { cat "$OUT/unit.out"; false; }'

echo "== 2. clean runs: must report no errors"
n=0
for f in "Address-in-address" "Solid and checker" "Stressapptest-style patterns" "Classic marches" "Bus turnaround" "Write/read adjacency" "Normal cached traffic" \
         "Cross-core verify" "Pointer chase" "Bulk copy" "Read/write storm, hash data" "Read/write storm, random on both" "Evolving data" "Cache coherency" "Page shuffle"; do
  n=$((n + 1)); log="$OUT/clean$n.log"; rm -f "$log"
  # every run uses a fresh random seed, so a failure here must say which one to repeat with --seed
  check "clean: $f" 'timeout $TMO "$OUT/cli" $RUN -f "$f" --log "$log" > "$OUT/clean$n.out" 2>&1; rc=$?; [ $rc -eq 0 ] && ! grep -q "^ERROR" "$log" 2>/dev/null || { grep "run seed" "$OUT/clean$n.out"; tail -5 "$OUT/clean$n.out"; false; }'
done

echo "== 2b. option checking and coverage counter"
for bad in "-mb abc" "-t abc" "-l x" "--minutes abc" "--coverage -5" "--cache bogus" "--rng bogus" "-mb" "--seed abc" "--seed -1" "--seed +5" "--seed 0x" "--seed 0xZZ" "--seed 18446744073709551616" "--seed"; do
  check "bad option rejected with exit 2: $bad" 'timeout $TMO "$OUT/cli" $bad --list > /dev/null 2>&1; [ $? -eq 2 ]'
done
check "valid option values still accepted" 'timeout $TMO "$OUT/cli" -mb 64 -t 2 --cache bypass --rng murmur --minutes 0.5 --coverage 100 --list > /dev/null 2>&1'
check "seed accepted as decimal, hex, 0 and the 64-bit maximum" 'for sv in 12345 0xDEADBEEF 0 18446744073709551615 0xFFFFFFFFFFFFFFFF; do timeout $TMO "$OUT/cli" --seed $sv --list > /dev/null 2>&1 || exit 1; done'
check "xorshift RNG option runs clean (hash tests with the new final mix)" 'timeout $TMO "$OUT/cli" $RUN --rng xorshift -f "Random-order hash soak" --log "$OUT/xs.log" > "$OUT/xs.out" 2>&1; rc=$?; [ $rc -eq 0 ] && ! grep -q "^ERROR" "$OUT/xs.log" 2>/dev/null || { grep "run seed" "$OUT/xs.out"; false; }'
log="$OUT/cov.log"; rm -f "$log"
timeout $TMO "$OUT/cli" -mb 2 -t 1 --no-large-pages -f "Bulk copy" --log "$log" > "$OUT/cov.out" 2>&1
check "coverage counter exact on a 2 MB region (3 full verifies = 3.0x)" 'grep -q "coverage this test 3.0x" "$OUT/cov.out" || { grep coverage "$OUT/cov.out"; false; }'

echo "== 2c. run seed: printed, logged, accepted back, and it reproduces the run"
rs() { grep -o "run seed: 0x[0-9A-F]*" "$1" | head -1 | sed 's/run seed: //'; }   # the seed a run printed
S1="$OUT/seed1"; S2="$OUT/seed2"; rm -f "$S1".* "$S2".*
timeout $TMO "$OUT/cli" -mb 2 -t 1 --no-large-pages -f "Bulk copy" --log "$S1.log" > "$S1.out" 2>&1
timeout $TMO "$OUT/cli" -mb 2 -t 1 --no-large-pages -f "Bulk copy" --log "$S2.log" > "$S2.out" 2>&1
check "each run prints a seed, and two runs get different ones" '[ -n "$(rs "$S1.out")" ] && [ -n "$(rs "$S2.out")" ] && [ "$(rs "$S1.out")" != "$(rs "$S2.out")" ]'
check "the seed is also in the log file header" 'grep -q "seed $(rs "$S1.out")" "$S1.log"'
check "random seeds are labelled as random" 'grep -q "run seed: 0x[0-9A-F]* (random)" "$S1.out"'
timeout $TMO "$OUT/cli" -mb 2 -t 1 --no-large-pages -f "Bulk copy" --seed 777 --log "$S2.log" > "$S2.out" 2>&1
check "--seed 777 is echoed as 0x0000000000000309 (given)" 'grep -q "run seed: 0x0000000000000309 (given)" "$S2.out"'
SEEDP="$(rs "$S1.out")"; timeout $TMO "$OUT/cli" -mb 2 -t 1 --no-large-pages -f "Bulk copy" --seed "$SEEDP" --log "$S2.log" > "$S2.out" 2>&1
check "the printed seed is accepted back unchanged" '[ "$(rs "$S2.out")" = "$SEEDP" ] && grep -q "(given)" "$S2.out"'
# End to end: the injected faults of the evolving-data test print hash data derived from the seed, so the data of the two errors (not their
# timestamps or addresses) must be identical for the same seed and differ for another seed.
for tag in A B C; do sv=777; [ $tag = C ] && sv=778
  timeout $TMO "$OUT/cli_inject" $RUN -f "Evolving data" --seed $sv --log "$OUT/rep$tag.log" > "$OUT/rep$tag.out" 2>&1
  grep "qword" "$OUT/rep$tag.log" | sort > "$OUT/rep$tag.data"
done
check "same seed -> identical data and fault lines (run reproduced)" '[ -s "$OUT/repA.data" ] && cmp -s "$OUT/repA.data" "$OUT/repB.data"'
check "different seed -> different data" '! cmp -s "$OUT/repA.data" "$OUT/repC.data"'

echo "== 3. fault injection: planted faults must be found"
log="$OUT/inj_fill.log"; rm -f "$log"
timeout $TMO "$OUT/cli_inject" $RUN -f "Solid and checker" --log "$log" > "$OUT/inj_fill.out" 2>&1; rc=$?
check "fill: exit code 1"                       '[ $rc -eq 1 ]'
check "fill: exactly the 42 planted lines found" '[ "$(grep -c "^ERROR" "$log")" = "42" ]'
check "fill: 44 flipped bits counted"            'grep -q "42 bad lines, 44 bits" "$OUT/inj_fill.out"'
check "fill: single-bit and 3-bit faults both logged" 'grep -q "lanes=0x00000001 bits=1" "$log" && grep -Eq "bits=3 " "$log"'
log="$OUT/inj_page.log"; rm -f "$log"
timeout $TMO "$OUT/cli_inject" $RUN -f "Page shuffle" --log "$log" > "$OUT/inj_page.out" 2>&1; rc=$?
check "page shuffle: exit 1, flip caught in final verify" '[ $rc -eq 1 ] && grep -q "pagemix-final" "$log"'
log="$OUT/inj_pagestop.log"; rm -f "$log"
DDR5TEST_STOP_IN_FILL=1 timeout $TMO "$OUT/cli_inject" $RUN -f "Page shuffle" --log "$log" > "$OUT/inj_pagestop.out" 2>&1; rc=$?
check "page shuffle: stopped mid-fill reports only the planted flip (pages never filled are not verified)" '[ $rc -eq 1 ] && [ "$(grep -c "^ERROR" "$log")" = "1" ] || { echo "    ERROR lines: $(grep -c "^ERROR" "$log")"; false; }'
log="$OUT/inj_coh.log"; rm -f "$log"
timeout $TMO "$OUT/cli_inject" $RUN -f "Cache coherency" --log "$log" > "$OUT/inj_coh.out" 2>&1; rc=$?
check "coherency: exit 1, lost increment caught"          '[ $rc -eq 1 ] && grep -q "coherency" "$log"'
log="$OUT/inj_evo.log"; rm -f "$log"
timeout $TMO "$OUT/cli_inject" $RUN -f "Evolving data" --log "$log" > "$OUT/inj_evo.out" 2>&1; rc=$?
check "evolve: exit 1" '[ $rc -eq 1 ]'
check "evolve: exactly the lost write and the stray flip are found (2 lines)" '[ "$(grep -c "^ERROR" "$log")" = "2" ] && [ "$(grep -c "evolve-verify" "$log")" = "2" ] || { grep "run seed" "$OUT/inj_evo.out"; false; }'
check "evolve: lost write shows as a whole stale line, the stray change as a single bit" 'grep -Eq "bits=1 " "$log" && grep -Eq "bits=[0-9]{3} " "$log"'
log="$OUT/inj_stop.log"; rm -f "$log"
timeout $TMO "$OUT/cli_inject" $RUN -f "Solid and checker" --stop-on-error --log "$log" > "$OUT/inj_stop.out" 2>&1; rc=$?
check "stop-on-error: exit 1 and no hang (barrier shutdown)" '[ $rc -eq 1 ]'

echo
if [ $fail -eq 0 ]; then echo "SELFTEST PASSED"; else echo "SELFTEST FAILED (outputs in $OUT)"; fi
exit $fail
