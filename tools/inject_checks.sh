#!/usr/bin/env bash
# Portable fault-injection checks: run the fault-injection console build (ddr5test_cli_inject, compiled with -DDDR5TEST_INJECT) and require that
# every planted fault is reported with the exact line / bit counts. The hooks plant known bit flips between write and verify; see src/inject.cpp.
# These checks need nothing but bash, grep and timeout, so the same file runs
#   - from tools/selftest.sh on Linux (sourced; it provides check/ok/bad, $TMO, $RUN and $SAN), and
#   - on the Windows CI runner against the MSVC build (Git Bash): bash tools/inject_checks.sh build/ddr5test_cli_inject.exe
# Standalone usage: tools/inject_checks.sh PATH_TO_ddr5test_cli_inject [OUTDIR]     (OUTDIR default: build/inject_checks, kept for the logs)
# Give OUTDIR as a relative path when the tester is a native Windows program: it does not understand Git Bash's /tmp/... paths.
# Exits 1 if any check fails, 2 if the tester does not exist. A build WITHOUT the injection hooks fails the first check (exit 0, no errors).
#
# Checks that need Linux (SIGINT, SIGABRT exit codes, /dev/full, CPU flags) or env-var hooks stay in tools/selftest.sh.

# inject_checks_portable INJ OUT: INJ is the fault-injection tester, OUT the directory for its logs and outputs (outputs keep the names
# inj_<what>.log / .out so the caller can look at them afterwards). Uses the caller's check(), $TMO (seconds) and $RUN (options of one run).
inject_checks_portable() {
  local INJ=$1 OUT=$2 log rc
  log="$OUT/inj_fill.log"; rm -f "$log"
  timeout $TMO "$INJ" $RUN -f "Solid and checker" --log "$log" > "$OUT/inj_fill.out" 2>&1; rc=$?
  check "fill: exit code 1"                       '[ $rc -eq 1 ]'
  check "fill: exactly the 42 planted lines found" '[ "$(grep -c "^ERROR" "$log")" = "42" ]'
  check "fill: 44 flipped bits counted"            'grep -q "42 bad lines, 44 bits" "$OUT/inj_fill.out"'
  check "fill: single-bit and 3-bit faults both logged" 'grep -q "lanes=0x00000001 bits=1" "$log" && grep -Eq "bits=3 " "$log"'
  log="$OUT/inj_page.log"; rm -f "$log"
  timeout $TMO "$INJ" $RUN -f "Page shuffle" --log "$log" > "$OUT/inj_page.out" 2>&1; rc=$?
  check "page shuffle: exit 1, flip caught in final verify" '[ $rc -eq 1 ] && grep -q "pagemix-final" "$log"'
  log="$OUT/inj_coh.log"; rm -f "$log"
  timeout $TMO "$INJ" $RUN -f "Cache coherency" --log "$log" > "$OUT/inj_coh.out" 2>&1; rc=$?
  check "coherency: exit 1, lost increment caught"          '[ $rc -eq 1 ] && grep -q "coherency" "$log"'
  log="$OUT/inj_evo.log"; rm -f "$log"
  timeout $TMO "$INJ" $RUN -f "Evolving data" --log "$log" > "$OUT/inj_evo.out" 2>&1; rc=$?
  check "evolve: exit 1" '[ $rc -eq 1 ]'
  check "evolve: exactly the lost write and the stray flip are found (2 lines)" '[ "$(grep -c "^ERROR" "$log")" = "2" ] && [ "$(grep -c "evolve-verify" "$log")" = "2" ] || { grep "run seed" "$OUT/inj_evo.out"; false; }'
  check "evolve: lost write shows as a whole stale line, the stray change as a single bit" 'grep -Eq "bits=1 " "$log" && grep -Eq "bits=[0-9]{3} " "$log"'
  log="$OUT/inj_march.log"; rm -f "$log"
  timeout $TMO "$INJ" $RUN -f "Classic marches" --log "$log" > "$OUT/inj_march.out" 2>&1; rc=$?
  check "march: exit 1, exactly the 3 planted lines (5 bits) found by the read right after the write element" '[ $rc -eq 1 ] && [ "$(grep -c "^ERROR" "$log")" = "3" ] && [ "$(grep -c "march el1 r0" "$log")" = "3" ] && grep -q "3 bad lines, 5 bits" "$OUT/inj_march.out" || { echo "    rc=$rc ERROR lines: $(grep -c "^ERROR" "$log" 2>/dev/null)"; grep "run seed" "$OUT/inj_march.out"; false; }'
  log="$OUT/inj_chase.log"; rm -f "$log"
  timeout $TMO "$INJ" $RUN -f "Pointer chase" --log "$log" > "$OUT/inj_chase.out" 2>&1; rc=$?
  check "chase: exit 1, all 3 planted lines found (2 in one chain, one of them a pointer flip, plus 1 in another chain; 3 bits)" '[ $rc -eq 1 ] && [ "$(grep -c "^ERROR" "$log")" = "3" ] && [ "$(grep -c " chase thr=0 " "$log")" = "3" ] && grep -q "3 bad lines, 3 bits" "$OUT/inj_chase.out" || { echo "    rc=$rc ERROR lines: $(grep -c "^ERROR" "$log" 2>/dev/null)"; grep "run seed" "$OUT/inj_chase.out"; false; }'
  log="$OUT/inj_copy.log"; rm -f "$log"
  timeout $TMO "$INJ" $RUN -f "Bulk copy" --log "$log" > "$OUT/inj_copy.out" 2>&1; rc=$?
  check "copy: exit 1, exactly 2 corrupted destination lines and 1 corrupted source line (4 bits) found" '[ $rc -eq 1 ] && [ "$(grep -c "^ERROR" "$log")" = "3" ] && [ "$(grep -c "copy-dst" "$log")" = "2" ] && [ "$(grep -c "copy-src" "$log")" = "1" ] && grep -q "3 bad lines, 4 bits" "$OUT/inj_copy.out" || { echo "    rc=$rc ERROR lines: $(grep -c "^ERROR" "$log" 2>/dev/null)"; grep "run seed" "$OUT/inj_copy.out"; false; }'
  log="$OUT/inj_rw.log"; rm -f "$log"
  # TSan exclusion, same reason as in the clean runs of selftest.sh: the writer and its verifying partner race by design
  if [ "${SAN:-}" = thread ]; then
    echo "  SKIP  rwstorm injection (races by design, see the comment at the clean-run list)"
  else
    timeout $TMO "$INJ" $RUN -f "Read/write storm, hash data" --log "$log" > "$OUT/inj_rw.out" 2>&1; rc=$?
    check "rwstorm: exit 1, 2 planted lines found by the final check (thr 0) and 1 read-side fault by the concurrent verify (thr 1); 4 bits" '[ $rc -eq 1 ] && [ "$(grep -c "^ERROR" "$log")" = "3" ] && [ "$(grep -c "rwstorm-read thr=0 " "$log")" = "2" ] && [ "$(grep -c "rwstorm-read thr=1 " "$log")" = "1" ] && grep -q "3 bad lines, 4 bits" "$OUT/inj_rw.out" || { echo "    rc=$rc ERROR lines: $(grep -c "^ERROR" "$log" 2>/dev/null)"; grep "run seed" "$OUT/inj_rw.out"; false; }'
  fi
  log="$OUT/inj_stop.log"; rm -f "$log"
  timeout $TMO "$INJ" $RUN -f "Solid and checker" --stop-on-error --log "$log" > "$OUT/inj_stop.out" 2>&1; rc=$?
  check "stop-on-error: exit 1 and no hang (barrier shutdown)" '[ $rc -eq 1 ]'
}

# Standalone run (not sourced): set up what selftest.sh would provide.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  set -u
  INJ=${1:-}
  OUT=${2:-build/inject_checks}
  if [ -z "$INJ" ] || [ ! -f "$INJ" ]; then
    echo "usage: $0 PATH_TO_ddr5test_cli_inject [OUTDIR]   (not found: '${INJ}')" >&2
    exit 2
  fi
  mkdir -p "$OUT" || exit 2
  TMO=${TMO:-120}          # every run is wrapped in timeout: a hang must fail the check (exit code 124), not stall CI
  RUN="-mb 64 -t 2 --no-large-pages --test-seconds 3"
  fail=0
  ok()    { printf '  PASS  %s\n' "$1"; }
  bad()   { printf '  FAIL  %s\n' "$1"; fail=1; }
  check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
  echo "== fault injection: planted faults must be found ($INJ, logs in $OUT)"
  inject_checks_portable "$INJ" "$OUT"
  echo
  if [ $fail -eq 0 ]; then echo "INJECTION CHECKS PASSED"; else echo "INJECTION CHECKS FAILED (outputs in $OUT)"; fi
  exit $fail
fi
