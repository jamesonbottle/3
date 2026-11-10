#!/usr/bin/env bash
# Reproducible self-test of the engine on Linux (needs g++ 11+ and an AVX2 CPU). Usage: tools/selftest.sh
#
#  1. unit tests       address orders are permutations, windows cover the whole chunk, suite.cfg parses
#  2. clean runs       the normal build must find NO errors (no false positives), exit code 0
#  3. fault injection  a build with -DDDR5TEST_INJECT plants known bit flips between write and verify;
#                      the tester must report exactly those (exit code 1), and must not hang with --stop-on-error
#
# Exits non-zero on the first failed check group. Real DDR5 hardware is NOT needed (or exercised): this proves the detection logic, not the DIMMs.
set -u
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
OUT=${OUT:-$(mktemp -d)}
mkdir -p "$OUT"
FLAGS="-std=c++20 -O2 -mavx2 -pthread -Wall -Wextra -Isrc"
RUN="-mb 64 -t 2 --no-large-pages --test-seconds 3"
TMO=${TMO:-120}   # every run of the tester is wrapped in timeout: a hang must fail the self-test (exit code 124), not stall CI
fail=0
ok()   { printf '  PASS  %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

echo "== build ($CXX) -> $OUT"
$CXX $FLAGS tools/unit_tests.cpp -o "$OUT/unit_tests" 2>&1 | grep -v "run_suite.*defined but not used" | grep -E "error|warning" && fail=1
$CXX $FLAGS src/main_cli.cpp -o "$OUT/cli" || exit 2
$CXX $FLAGS -DDDR5TEST_INJECT src/main_cli.cpp -o "$OUT/cli_inject" || exit 2

echo "== 1. unit tests"
check "orders, windows, generators, suite parsing" 'timeout $TMO "$OUT/unit_tests" > "$OUT/unit.out" 2>&1 || { cat "$OUT/unit.out"; false; }'

echo "== 2. clean runs: must report no errors"
n=0
for f in "Address-in-address" "Solid and checker" "Stressapptest-style patterns" "Classic marches" "Bus turnaround" "Write/read adjacency" "Normal cached traffic" \
         "Cross-core verify" "Pointer chase" "Bulk copy" "Read/write storm, hash data" "Read/write storm, random on both" "Cache coherency" "Page shuffle"; do
  n=$((n + 1)); log="$OUT/clean$n.log"; rm -f "$log"
  check "clean: $f" 'timeout $TMO "$OUT/cli" $RUN -f "$f" --log "$log" > "$OUT/clean$n.out" 2>&1; rc=$?; [ $rc -eq 0 ] && ! grep -q "^ERROR" "$log" 2>/dev/null || { tail -5 "$OUT/clean$n.out"; false; }'
done

echo "== 2b. option checking and coverage counter"
for bad in "-mb abc" "-t abc" "-l x" "--minutes abc" "--coverage -5" "--cache bogus" "--rng bogus" "-mb"; do
  check "bad option rejected with exit 2: $bad" 'timeout $TMO "$OUT/cli" $bad --list > /dev/null 2>&1; [ $? -eq 2 ]'
done
check "valid option values still accepted" 'timeout $TMO "$OUT/cli" -mb 64 -t 2 --cache bypass --rng murmur --minutes 0.5 --coverage 100 --list > /dev/null 2>&1'
log="$OUT/cov.log"; rm -f "$log"
timeout $TMO "$OUT/cli" -mb 2 -t 1 --no-large-pages -f "Bulk copy" --log "$log" > "$OUT/cov.out" 2>&1
check "coverage counter exact on a 2 MB region (3 full verifies = 3.0x)" 'grep -q "coverage this test 3.0x" "$OUT/cov.out" || { grep coverage "$OUT/cov.out"; false; }'

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
log="$OUT/inj_stop.log"; rm -f "$log"
timeout $TMO "$OUT/cli_inject" $RUN -f "Solid and checker" --stop-on-error --log "$log" > "$OUT/inj_stop.out" 2>&1; rc=$?
check "stop-on-error: exit 1 and no hang (barrier shutdown)" '[ $rc -eq 1 ]'

echo
if [ $fail -eq 0 ]; then echo "SELFTEST PASSED"; else echo "SELFTEST FAILED (outputs in $OUT)"; fi
exit $fail
