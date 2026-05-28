#!/usr/bin/env bash
# arbalest_check.sh
#
# Runs all Arbalest correctness tests (opt/FileCheck, clang driver/codegen,
# compiler-rt unit tests) and reports pass/fail.
#
# Usage:
#   ./arbalest_check.sh
#   ./arbalest_check.sh --no-color
#
# Exit code: 0 if all tests pass, nonzero otherwise.

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD=/scratch/mdavis/arbalest-build

OPT=$BUILD/bin/opt
FC=$BUILD/bin/FileCheck
CLANG=$BUILD/bin/clang
UNIT=$BUILD/runtimes/runtimes-bins/compiler-rt/lib/tsan/tests/unit/TsanUnitTest-x86_64-Test
IDIR=$REPO/llvm/test/Instrumentation/Arbalest

# ── color ─────────────────────────────────────────────────────────────────────
USE_COLOR=true
[[ "${1:-}" == "--no-color" ]] && USE_COLOR=false

green() { $USE_COLOR && printf '\033[32m%s\033[0m\n' "$*" || printf '%s\n' "$*"; }
red()   { $USE_COLOR && printf '\033[31m%s\033[0m\n' "$*" || printf '%s\n' "$*"; }
bold()  { $USE_COLOR && printf '\033[1m%s\033[0m\n'  "$*" || printf '%s\n' "$*"; }

# ── preflight ─────────────────────────────────────────────────────────────────
missing=false
for b in "$OPT" "$FC" "$CLANG" "$UNIT"; do
    [[ -x "$b" ]] || { echo "ERROR: not found or not executable: $b"; missing=true; }
done
$missing && exit 1

# ── test harness ──────────────────────────────────────────────────────────────
PASS=0; FAIL=0; ERRORS=()

chk() {
    local name="$1" cmd="$2"
    if eval "$cmd" >/dev/null 2>&1; then
        PASS=$((PASS+1))
        $USE_COLOR && printf '  \033[32mPASS\033[0m  %s\n' "$name" \
                   || printf '  PASS  %s\n' "$name"
    else
        FAIL=$((FAIL+1))
        $USE_COLOR && printf '  \033[31mFAIL\033[0m  %s\n' "$name" \
                   || printf '  FAIL  %s\n' "$name"
        ERRORS+=("$name")
    fi
}

# ── opt / FileCheck tests ──────────────────────────────────────────────────────
bold '── opt / FileCheck ─────────────────────────────────────────────────────'

PASSES="-passes='arbalest-module,function(arbalest)'"

chk 'arbalest_basic (with -arbalest=1)' \
    "$OPT < $IDIR/arbalest_basic.ll $PASSES -arbalest=1 -S 2>/dev/null \
     | $FC $IDIR/arbalest_basic.ll"

chk 'arbalest_basic (NOFLAG: pass is no-op)' \
    "$OPT < $IDIR/arbalest_basic.ll $PASSES -S 2>/dev/null \
     | $FC $IDIR/arbalest_basic.ll --check-prefix=NOFLAG"

chk 'arbalest_no_instrument (constant globals skipped)' \
    "$OPT < $IDIR/arbalest_no_instrument.ll $PASSES -arbalest=1 -S 2>/dev/null \
     | $FC $IDIR/arbalest_no_instrument.ll"

chk 'arbalest_outlined (GEP check_bound in outlined functions)' \
    "$OPT < $IDIR/arbalest_outlined.ll $PASSES -arbalest=1 -S 2>/dev/null \
     | $FC $IDIR/arbalest_outlined.ll"

chk 'arbalest_dedupe (STRIP mode)' \
    "$OPT < $IDIR/arbalest_dedupe.ll $PASSES -arbalest=1 \
     -arbalest-dedupe-mode=strip -S 2>/dev/null \
     | $FC $IDIR/arbalest_dedupe.ll --check-prefix=STRIP"

chk 'arbalest_dedupe (OFF mode: no dedup)' \
    "$OPT < $IDIR/arbalest_dedupe.ll $PASSES -arbalest=1 \
     -arbalest-dedupe-mode=off -S 2>/dev/null \
     | $FC $IDIR/arbalest_dedupe.ll --check-prefix=OFF"

chk 'arbalest_dedupe (VALUE mode)' \
    "$OPT < $IDIR/arbalest_dedupe.ll $PASSES -arbalest=1 \
     -arbalest-dedupe-mode=value -S 2>/dev/null \
     | $FC $IDIR/arbalest_dedupe.ll --check-prefix=VALUE"

chk 'arbalest_hoist (hoist=true: range/cstride in preheader)' \
    "$OPT < $IDIR/arbalest_hoist.ll $PASSES -arbalest=1 \
     -arbalest-hoist=true -S 2>/dev/null \
     | $FC $IDIR/arbalest_hoist.ll"

chk 'arbalest_hoist (hoist=false: per-element in loop body)' \
    "$OPT < $IDIR/arbalest_hoist.ll $PASSES -arbalest=1 \
     -arbalest-hoist=false -S 2>/dev/null \
     | $FC $IDIR/arbalest_hoist.ll --check-prefix=NOHOIST"

# ── clang driver / codegen tests ──────────────────────────────────────────────
bold '── clang driver / codegen ──────────────────────────────────────────────'

DFILE=$REPO/clang/test/Driver/arbalest.c
CFILE=$REPO/clang/test/CodeGen/arbalest.c
OMP="-fsanitize=thread -fopenmp -fopenmp-targets=x86_64-pc-linux-gnu -farbalest -O1"

chk 'driver: -farbalest emits -arbalest=1 (FLAG)' \
    "$CLANG -target x86_64-unknown-linux $OMP $DFILE -### 2>&1 \
     | $FC $DFILE --check-prefix=FLAG"

chk 'driver: -g adds -arbalest-debug-info=1 (DEBUGFLAG)' \
    "$CLANG -target x86_64-unknown-linux $OMP -g $DFILE -### 2>&1 \
     | $FC $DFILE --check-prefix=DEBUGFLAG"

chk 'driver: -fno-arbalest suppresses flag (NOFLAG)' \
    "$CLANG -target x86_64-unknown-linux $OMP -fno-arbalest $DFILE -### 2>&1 \
     | $FC $DFILE --check-prefix=NOFLAG"

chk 'codegen: read4/write4/init emitted end-to-end' \
    "$CLANG -target x86_64-unknown-linux-gnu $OMP \
     $CFILE -S -emit-llvm -o - 2>/dev/null | $FC $CFILE"

# ── compiler-rt unit tests ─────────────────────────────────────────────────────
bold '── compiler-rt unit (Arbalest.*) ───────────────────────────────────────'

cur_test=""
while IFS= read -r line; do
    if [[ "$line" == *"[ RUN      ]"* ]]; then
        cur_test="${line##*] }"
    elif [[ "$line" == *"[       OK ]"* ]]; then
        PASS=$((PASS+1))
        $USE_COLOR && printf '  \033[32mPASS\033[0m  TsanUnit::%s\n' "$cur_test" \
                   || printf '  PASS  TsanUnit::%s\n' "$cur_test"
    elif [[ "$line" == *"[  FAILED  ]"* ]]; then
        FAIL=$((FAIL+1))
        $USE_COLOR && printf '  \033[31mFAIL\033[0m  TsanUnit::%s\n' "$cur_test" \
                   || printf '  FAIL  TsanUnit::%s\n' "$cur_test"
        ERRORS+=("TsanUnit::$cur_test")
    fi
done < <(TSAN_OPTIONS='ignore_noninstrumented_modules=1' \
          "$UNIT" --gtest_filter="Arbalest.*" 2>&1 || true)

# ── summary ───────────────────────────────────────────────────────────────────
printf '\n'
bold '════════════════════════════════════════════════════════════════════════'
if [[ $FAIL -eq 0 ]]; then
    green "  ALL PASSED  ($PASS tests)"
else
    red   "  $FAIL FAILED, $PASS PASSED"
    for e in "${ERRORS[@]}"; do
        red "    ✗ $e"
    done
fi
bold '════════════════════════════════════════════════════════════════════════'

exit "$FAIL"
