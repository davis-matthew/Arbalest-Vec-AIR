#!/usr/bin/env bash
# arbalest_ablation_reverify.sh
#
# Re-runs an ALREADY-BUILT config's binary once (no recompilation) purely to
# re-capture whether it actually reports a TSan/Arbalest violation, using the
# fixed is_real_violation() detector (looks for "WARNING: ThreadSanitizer" /
# "SUMMARY: ThreadSanitizer", not just "the program printed something").
#
# Needed because some early ablation_spec.sh runs used an older, overly
# broad heuristic ("any non-empty stderr") before it was fixed, and no raw
# output was saved for those runs to reparse -- so the only way to get
# trustworthy violation data for them is to execute the (already-compiled)
# binary again.
#
# Writes corrections to <bout>/violations_<buildkey>.csv (config_index,
# has_violation), one row per CFGS entry sharing that buildkey. Does NOT
# touch static/dynamic/run_ms/mem_kb -- apply the corrections on top of the
# existing ablation_results.csv with arbalest_ablation_apply_violations.sh.
#
# Usage:
#   ./arbalest_ablation_reverify.sh --bench N --only-buildkey KEY [--out-dir DIR] [--input-size test|train|ref]

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD=/scratch/mdavis/arbalest-build
OUT_DIR="$SCRIPT_DIR/arbalest_ablation_spec_out"
BENCH_IDX=""
ONLY_BUILDKEY=""
INPUT_SIZE=test

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bench)            BENCH_IDX="$2"; shift 2 ;;
        --bench=*)          BENCH_IDX="${1#*=}"; shift ;;
        --only-buildkey)    ONLY_BUILDKEY="$2"; shift 2 ;;
        --only-buildkey=*)  ONLY_BUILDKEY="${1#*=}"; shift ;;
        --out-dir)          OUT_DIR="$2"; shift 2 ;;
        --out-dir=*)        OUT_DIR="${1#*=}"; shift ;;
        --input-size)       INPUT_SIZE="$2"; shift 2 ;;
        --input-size=*)     INPUT_SIZE="${1#*=}"; shift ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; exit 1 ;;
    esac
done
[[ -n "$BENCH_IDX" && -n "$ONLY_BUILDKEY" ]] || { printf 'ERROR: --bench and --only-buildkey are required\n' >&2; exit 1; }

is_real_violation() { [[ "$1" == *"WARNING: ThreadSanitizer"* || "$1" == *"SUMMARY: ThreadSanitizer"* ]]; }

BENCH_NAME=('503.postencil' '504.polbm' '514.pomriq' '552.pep' '554.pcg')
BENCH_EXE=('stencil_exe' 'polbm' 'omriq_exe' 'pep' 'pcg')

TEST_INPUT=('512 512 64 100' '20 reference.dat 0 1 100_100_130_cf_a.of' '-i 32_32_32_dataset.bin -o 514.out' '' '')
TRAIN_INPUT=('512 512 64 100' '300 reference.dat 0 1 100_100_130_cf_b.of' '-i 64_64_64_dataset.bin -o 1028.out' '' '')
REF_INPUT=('512 512 64 100' '20 reference.dat 0 1 100_100_130_cf_a.of' '-i 128_128_128_dataset.bin -o 2056.out' '' '')
case "$INPUT_SIZE" in
    test)  INPUT=("${TEST_INPUT[@]}") ;;
    train) INPUT=("${TRAIN_INPUT[@]}") ;;
    ref)   INPUT=("${REF_INPUT[@]}") ;;
    *) printf 'ERROR: bad --input-size\n' >&2; exit 1 ;;
esac

# slug|label|dedupe|hoist|simd|ompsan|buildkey  (must match arbalest_ablation_spec.sh)
CFGS=(
    "baseline|baseline|off|false|na|false|off_false_false"
    "dedup_strip|dedup:strip|strip|false|na|false|strip_false_false"
    "dedup_value|dedup:value|value|false|na|false|value_false_false"
    "hoist_only_nosimd|hoist (no SIMD)|off|true|false|false|off_true_false"
    "hoist_only_simd|hoist (SIMD)|off|true|true|false|off_true_false"
    "hoist_strip_nosimd|hoist+strip (no SIMD)|strip|true|false|false|strip_true_false"
    "hoist_strip_simd|hoist+strip (SIMD)|strip|true|true|false|strip_true_false"
    "hoist_value_nosimd|hoist+value (no SIMD)|value|true|false|false|value_true_false"
    "hoist_value_simd|hoist+value (SIMD)|value|true|true|false|value_true_false"
)
BASE_NC=${#CFGS[@]}
for (( i=0; i<BASE_NC; i++ )); do
    IFS='|' read -r s l d h si _ bk <<< "${CFGS[$i]}"
    CFGS+=("${s}_ompsan|${l} +OMPSan|${d}|${h}|${si}|true|${bk}_ompsan")
done
NC=${#CFGS[@]}

bi="$BENCH_IDX"
bname="${BENCH_NAME[$bi]}"
bexe="${BENCH_EXE[$bi]}"
bargs="${INPUT[$bi]}"
bout="$OUT_DIR/$bname"

run_once_and_check() {
    local ci="$1" binpath="$2" arbalest="$3" simd="$4"
    local -a run_env=("LD_LIBRARY_PATH=$BUILD/lib:${LD_LIBRARY_PATH:-}")
    if $arbalest; then
        run_env+=("OMP_TOOL_LIBRARIES=$BUILD/lib/libarcher.so"
                  "TSAN_OPTIONS=ignore_noninstrumented_modules=1"
                  "ARBALEST_COUNT_CALLS=1")
        [[ "$simd" == "false" ]] && run_env+=("ARBALEST_DISABLE_SIMD=1")
    fi
    local out
    out=$(cd "$SCRIPT_DIR" && env "${run_env[@]}" "$binpath" $bargs 2>&1)
    printf '%s\n' "$out" > "$bout/run_${ci}.log"
    is_real_violation "$out" && printf 'yes' || printf 'no'
}

OUTCSV="$bout/violations_${ONLY_BUILDKEY}.csv"
printf 'config_index,has_violation\n' > "$OUTCSV"

if [[ "$ONLY_BUILDKEY" == "native" ]]; then
    binpath="$bout/build_native/$bexe"
    [[ -x "$binpath" ]] || { printf 'ERROR: no existing native build for %s\n' "$bname" >&2; exit 1; }
    v=$(run_once_and_check "native" "$binpath" false "na")
    printf 'native,%s\n' "$v" >> "$OUTCSV"
    printf '  [native] %s -> %s\n' "$bname" "$v"
else
    binpath="$bout/build_${ONLY_BUILDKEY}/$bexe"
    [[ -x "$binpath" ]] || { printf 'ERROR: no existing build for %s buildkey=%s (%s)\n' "$bname" "$ONLY_BUILDKEY" "$binpath" >&2; exit 1; }
    for (( ci=0; ci<NC; ci++ )); do
        IFS='|' read -r slug label dedupe hoist simd ompsan buildkey <<< "${CFGS[$ci]}"
        [[ "$buildkey" != "$ONLY_BUILDKEY" ]] && continue
        v=$(run_once_and_check "$ci" "$binpath" true "$simd")
        printf '%d,%s\n' "$ci" "$v" >> "$OUTCSV"
        printf '  [%d] %s %s -> %s\n' "$ci" "$bname" "$label" "$v"
    done
fi

printf 'Wrote %s\n' "$OUTCSV"
