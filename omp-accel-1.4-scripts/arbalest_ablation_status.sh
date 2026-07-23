#!/usr/bin/env bash
# arbalest_ablation_status.sh
#
# Quick status report for arbalest_ablation_spec.sh runs: SLURM job state
# for the abl_* jobs, plus per-benchmark progress (configs completed / most
# recently touched file, as a proxy for "what it's doing right now").
#
# Usage: ./arbalest_ablation_status.sh [--out-dir DIR] [--no-ompsan]
#
# --no-ompsan   Expect 10 configs/benchmark (native + 9) instead of the
#               default 19 (native + 9 base + 9 OMPSan mirrors) -- match
#               whatever --ompsan setting arbalest_ablation_spec.sh was run
#               with.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/arbalest_ablation_spec_out"
WITH_OMPSAN=true

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out-dir)   OUT_DIR="$2"; shift 2 ;;
        --out-dir=*) OUT_DIR="${1#*=}"; shift ;;
        --no-ompsan) WITH_OMPSAN=false; shift ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; exit 1 ;;
    esac
done

bold() { printf '\033[1m%s\033[0m\n' "$*"; }

bold "── SLURM jobs (abl_*) ──"
squeue -u "$USER" -o "%.10i %.14j %.2t %.12M %.12L %R" 2>/dev/null | { read -r header; printf '%s\n' "$header"; grep -E ' abl_' || printf '  (none queued/running)\n'; }
printf '\n'

BENCH_NAME=('503.postencil' '504.polbm' '514.pomriq' '552.pep' '554.pcg')
TOTAL_BASE=9    # baseline..hoist+value(SIMD)
TOTAL_WITH_OMPSAN=18
PLUS_NATIVE=1

bold "── Per-benchmark progress ──"
for bname in "${BENCH_NAME[@]}"; do
    bout="$OUT_DIR/$bname"
    if [[ ! -d "$bout" ]]; then
        printf '  %-16s not started (no output dir yet)\n' "$bname"
        continue
    fi

    csv="$bout/ablation_results.csv"
    done_n=0
    [[ -f "$csv" ]] && done_n=$(( $(wc -l < "$csv") - 1 ))
    [[ $done_n -lt 0 ]] && done_n=0

    total=$(( TOTAL_BASE + PLUS_NATIVE ))
    $WITH_OMPSAN && total=$(( TOTAL_WITH_OMPSAN + PLUS_NATIVE ))

    # Most recently modified file anywhere under this benchmark's out-dir is
    # a reasonable proxy for "what it's doing right now" (a compile.log while
    # building, a run_<ci>.log right after a run finishes, etc.). Skip the
    # transient /usr/bin/time tmp files -- not informative by name.
    latest=$(find "$bout" -type f -not -name 'tmp.*' -not -name 'ablation_results.csv' -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1)
    latest_path="${latest#* }"
    latest_age=""
    if [[ -n "$latest_path" ]]; then
        latest_epoch="${latest%% *}"
        now=$(date +%s)
        age=$(( now - ${latest_epoch%.*} ))
        latest_age="${age}s ago"
    fi

    status="running"
    [[ $done_n -ge $total ]] && status="DONE"

    printf '  %-16s %2d/%2d configs done  [%s]\n' "$bname" "$done_n" "$total" "$status"
    if [[ "$status" != "DONE" && -n "$latest_path" ]]; then
        printf '      last activity (%s): %s\n' "$latest_age" "${latest_path#"$OUT_DIR"/}"
    fi
done
