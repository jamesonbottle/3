#!/usr/bin/env bash
# Reproducible self-test of the engine on Linux (needs g++ 11+ and an AVX2 CPU). Usage: tools/selftest.sh
#
#  1. unit tests       address orders are permutations, windows cover the whole chunk, the built-in suite is valid, every hash mixer avalanches,
#                      one seed reproduces every random choice, evolving-data generations follow their rules
#  2. clean runs       the normal build must find NO errors (no false positives), exit code 0. Every run uses a fresh random seed
#                      (printed as "run seed: 0x..."; a failure prints it so the run can be repeated with --seed); 2c checks the seed itself;
#                      2e runs under taskset: the thread count and the pinning must follow the allowed CPUs
#  3. fault injection  a build with -DDDR5TEST_INJECT plants known bit flips between write and verify (and, in the evolving-data test, a lost
#                      write); the tester must report exactly those (exit code 1), and must not hang with --stop-on-error
#
# Sanitizer legs: SAN=address,undefined tools/selftest.sh   or   SAN=thread tools/selftest.sh   rebuild everything with -fsanitize=$SAN (-O1 -g)
#  and run the same checks; any sanitizer report fails the run. SAN=thread runs a reduced set (TSan is 5-15x slower) and leaves out the
#  read/write storm tests, which race by design (see the comment at the clean-run list). Unset (the default) = the normal build, unchanged.
#
# Exits non-zero on the first failed check group. Real DDR5 hardware is NOT needed (or exercised): this proves the detection logic, not the DIMMs.
set -u
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
OUT=${OUT:-$(mktemp -d)}
mkdir -p "$OUT"
RUN="-mb 64 -t 2 --no-large-pages --test-seconds 3"
SAN=${SAN:-}
# every run of the tester is wrapped in timeout: a hang must fail the self-test (exit code 124), not stall CI; sanitized builds are much slower
if [ -n "$SAN" ]; then TMO=${TMO:-600}; else TMO=${TMO:-120}; fi
case "$SAN" in
  thread) export TSAN_OPTIONS="${TSAN_OPTIONS:-} exitcode=66 halt_on_error=0 history_size=4" ;;
  *address*) export ASAN_OPTIONS="${ASAN_OPTIONS:-} detect_leaks=1 abort_on_error=0 exitcode=66"
             export UBSAN_OPTIONS="${UBSAN_OPTIONS:-} print_stacktrace=1 halt_on_error=1" ;;
esac
fail=0
ok()   { printf '  PASS  %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

echo "== build ($CXX, CMake) -> $OUT"
# unit_tests, ddr5test_cli (normal engine) and ddr5test_cli_inject (same sources compiled with -DDDR5TEST_INJECT), built by CMake with -Wall -Wextra.
cmake -S . -B "$OUT/cmake" -DCMAKE_CXX_COMPILER="$CXX" -DDDR5_OUTPUT_DIR="$OUT" -DDDR5_SANITIZE="$SAN" > "$OUT/cmake.log" 2>&1 || { cat "$OUT/cmake.log"; exit 2; }
cmake --build "$OUT/cmake" --target unit_tests ddr5test_cli ddr5test_cli_inject -j "$(nproc 2>/dev/null || echo 2)" > "$OUT/build.log" 2>&1 || { cat "$OUT/build.log"; exit 2; }
grep -E "error|warning" "$OUT/build.log" && fail=1
mv -f "$OUT/ddr5test_cli" "$OUT/cli"; mv -f "$OUT/ddr5test_cli_inject" "$OUT/cli_inject"

echo "== 1. unit tests"
check "orders, windows, generators, built-in suite" 'timeout $TMO "$OUT/unit_tests" > "$OUT/unit.out" 2>&1 || { cat "$OUT/unit.out"; false; }'

echo "== 2. clean runs: must report no errors"
n=0
for f in "Address-in-address" "Solid and checker" "Stressapptest-style patterns" "Classic marches" "Bus turnaround" "Write/read adjacency" "Normal cached traffic" \
         "Cross-core verify" "Pointer chase" "Bulk copy" "Read/write storm, hash data" "Read/write storm, random on both" "Evolving data" "Cache coherency" "Page shuffle"; do
  n=$((n + 1)); log="$OUT/clean$n.log"; rm -f "$log"
  # TSan exclusion, on purpose and not a suppression: in the read/write storm the even threads rewrite IDENTICAL data while their partner
  # verifies it (algo_rwstorm.cpp), so TSan reports a write/read race on every line. The data cannot be wrong, the race is the test.
  if [ "$SAN" = thread ] && [ "${f#Read/write storm}" != "$f" ]; then echo "  SKIP  clean: $f (races by design, see the comment above)"; continue; fi
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

echo "== 2d. log file that cannot be opened"
timeout $TMO "$OUT/cli" $RUN -f "Bulk copy" --log /nonexistent/dir/x.log > "$OUT/logbad.out" 2>&1; rc=$?
check "explicit --log that cannot be opened: exit 2 with the path and reason, before any test runs" '[ $rc -eq 2 ] && grep -q "cannot open log file '"'"'/nonexistent/dir/x.log'"'"'" "$OUT/logbad.out" && ! grep -q "\[loop" "$OUT/logbad.out" || { cat "$OUT/logbad.out"; false; }'
mkdir -p "$OUT/rodir"; ( cd "$OUT/rodir" && mkdir ddr5test.log && timeout $TMO "$OUT/cli" $RUN -f "Bulk copy" > "$OUT/logdef.out" 2>&1; echo $? > "$OUT/logdef.rc" )   # a directory named like the default log: fopen fails even as root
check "default log name that cannot be opened: warns and still exits 0 on a clean run" '[ "$(cat "$OUT/logdef.rc")" = "0" ] && grep -q "cannot open log file '"'"'ddr5test.log'"'"'" "$OUT/logdef.out" && grep -q "continuing without it" "$OUT/logdef.out" || { cat "$OUT/logdef.out"; false; }'

echo "== 2e. CPU restriction: thread count and pinning follow taskset, not the machine's CPU count"
# the CPUs this script may use, one number per line (a taskset or cpuset limit on the script itself is respected)
allowed=$(taskset -pc $$ 2>/dev/null | sed 's/.*: //' | awk -F, '{for (i = 1; i <= NF; i++) { n = split($i, r, "-"); if (n == 2) for (j = r[1]; j <= r[2]; j++) print j; else print $i } }')
# start the tester on CPUs $1 and $2 with two workers; true once one worker sits on CPU $1 alone and one on CPU $2 alone
workers_pinned() {
  taskset -c "$1,$2" "$OUT/cli" -mb 64 -t 2 --no-large-pages --test-seconds 8 -f "Address-in-address" --log "$OUT/pin.log" > "$OUT/pin.out" 2>&1 &
  local p=$! i m rc=1 t
  for i in $(seq 1 40); do
    kill -0 $p 2>/dev/null || break
    m=$(for t in /proc/$p/task/*; do sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' "$t/status" 2>/dev/null; done)
    if echo "$m" | grep -qx "$1" && echo "$m" | grep -qx "$2"; then rc=0; break; fi
    sleep 0.2
  done
  kill $p 2>/dev/null; wait $p 2>/dev/null
  return $rc
}
if [ "$(echo "$allowed" | grep -c .)" -ge 2 ]; then
  two=$(echo "$allowed" | head -2 | paste -sd, -); one=$(echo "$allowed" | head -1)
  # no -t: the default thread count is what the process may use, so it must follow the restriction
  check "taskset -c $two: 2 threads by default, clean run, every thread pinned" 'taskset -c $two timeout $TMO "$OUT/cli" -mb 64 --no-large-pages --test-seconds 3 -f "Address-in-address" --log "$OUT/ts2.log" > "$OUT/ts2.out" 2>&1; rc=$?; [ $rc -eq 0 ] && grep -q "^2 threads," "$OUT/ts2.out" && ! grep -q "could not be pinned" "$OUT/ts2.out" && ! grep -q "^ERROR" "$OUT/ts2.log" 2>/dev/null || { echo "    rc=$rc"; head -3 "$OUT/ts2.out"; false; }'
  check "taskset -c $one: 1 thread by default, clean run" 'taskset -c $one timeout $TMO "$OUT/cli" -mb 64 --no-large-pages --test-seconds 3 -f "Address-in-address" --log "$OUT/ts1.log" > "$OUT/ts1.out" 2>&1; rc=$?; [ $rc -eq 0 ] && grep -q "^1 threads," "$OUT/ts1.out" && ! grep -q "^ERROR" "$OUT/ts1.log" 2>/dev/null || { echo "    rc=$rc"; head -3 "$OUT/ts1.out"; false; }'
  # the two highest allowed CPUs: on a machine with more than two CPUs these are not CPUs 0 and 1
  top1=$(echo "$allowed" | tail -2 | head -1); top2=$(echo "$allowed" | tail -1)
  check "workers are pinned one per allowed CPU ($top1 and $top2)" 'workers_pinned $top1 $top2 || { echo "    thread CPU lists never showed one worker each on $top1 and $top2"; head -3 "$OUT/pin.out"; false; }'
else
  echo "  SKIP  taskset not available, or fewer than 2 usable CPUs"
fi

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
log="$OUT/inj_evostop.log"; rm -f "$log"
DDR5TEST_STOP_IN_EVOLVE=1 timeout $TMO "$OUT/cli_inject" $RUN -f "Evolving data" --log "$log" > "$OUT/inj_evostop.out" 2>&1; rc=$?
check "evolve: stopped mid-rewrite reports no errors (lines the cut-short pass never reached are not verified)" '[ $rc -eq 0 ] && [ "$(grep -c "^ERROR" "$log" 2>/dev/null)" = "0" ] || { echo "    rc=$rc ERROR lines: $(grep -c "^ERROR" "$log" 2>/dev/null)"; grep "run seed" "$OUT/inj_evostop.out"; false; }'
log="$OUT/inj_throw.log"; rm -f "$log"
DDR5TEST_THROW_IN_WORKER=1 timeout $TMO "$OUT/cli_inject" $RUN -f "Solid and checker" --fpu --log "$log" > "$OUT/inj_throw.out" 2>&1; rc=$?
check "worker exception: exit 3 and 'internal error' logged (no terminate, no hang; --fpu threads joined)" '[ $rc -eq 3 ] && grep -q "internal error: injected worker failure" "$OUT/inj_throw.out" || { echo "    rc=$rc"; tail -5 "$OUT/inj_throw.out"; false; }'
log="$OUT/inj_stop.log"; rm -f "$log"
timeout $TMO "$OUT/cli_inject" $RUN -f "Solid and checker" --stop-on-error --log "$log" > "$OUT/inj_stop.out" 2>&1; rc=$?
check "stop-on-error: exit 1 and no hang (barrier shutdown)" '[ $rc -eq 1 ]'

echo
if [ $fail -eq 0 ]; then echo "SELFTEST PASSED"; else echo "SELFTEST FAILED (outputs in $OUT)"; fi
exit $fail
