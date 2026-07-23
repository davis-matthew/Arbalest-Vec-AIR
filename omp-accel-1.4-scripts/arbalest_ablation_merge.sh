#!/usr/bin/env bash
# arbalest_ablation_merge.sh
#
# Merges a benchmark's main ablation_results.csv (native + whatever the
# original sequential job already finished) with any per-buildkey CSVs
# produced by `arbalest_ablation_spec.sh --only-buildkey KEY` (one such file
# per concurrently-run remaining build: ablation_results_<KEY>.csv), sorted
# by config_index (native first, then 0..17). Writes the merged result back
# to ablation_results.csv (the per-buildkey files are left alone -- rerun
# this any time to refresh as more jobs finish).
#
# Usage: ./arbalest_ablation_merge.sh [--out-dir DIR] [bench ...]
#   bench: benchmark dir names to merge (default: all under --out-dir)

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/arbalest_ablation_spec_out"

while [[ "${1:-}" == --out-dir* ]]; do
    case "$1" in
        --out-dir)   OUT_DIR="$2"; shift 2 ;;
        --out-dir=*) OUT_DIR="${1#*=}"; shift ;;
    esac
done

if [[ $# -gt 0 ]]; then
    BENCHES=("$@")
else
    BENCHES=()
    for d in "$OUT_DIR"/*/; do
        [[ -f "$d/ablation_results.csv" || -n "$(ls "$d"/ablation_results_*.csv 2>/dev/null)" ]] && BENCHES+=("$(basename "$d")")
    done
fi

HEADER='benchmark,config_index,slug,label,dedupe,hoist,simd,ompsan,static_total,dynamic_total,build_ms,run_ms,mem_kb,has_violation'

# Strips the header line from EVERY input file (not just the first line of
# the whole stream -- multiple per-buildkey CSVs are concatenated together,
# each with its own header), sorts by config_index ("native" first), and
# de-duplicates by config_index (last occurrence wins, in case a config was
# computed by more than one source, e.g. an old sequential run and a newer
# per-buildkey job overlapped). Relies on `sort -s` being stable so that for
# equal keys, later input rows stay after earlier ones.
sort_key() {
    awk -F, -v OFS=, '
        $1=="benchmark" { next }
        { print ($2=="native" ? -1 : $2), $0 }
    ' | sort -t, -k1,1n -s | awk -F, '
        $1 != prev_key { if (NR > 1) print prev_line; prev_key=$1 }
        { prev_line=$0 }
        END { if (NR > 0) print prev_line }
    ' | cut -d, -f2-
}

for bname in "${BENCHES[@]}"; do
    bout="$OUT_DIR/$bname"
    [[ -d "$bout" ]] || { printf 'WARNING: no such dir: %s\n' "$bout" >&2; continue; }

    tmp=$(mktemp -p "$bout")
    {
        [[ -f "$bout/ablation_results.csv" ]] && cat "$bout/ablation_results.csv"
        for f in "$bout"/ablation_results_*.csv; do
            [[ -f "$f" ]] && cat "$f"
        done
    } | sort_key > "$tmp"

    n=$(wc -l < "$tmp")
    { printf '%s\n' "$HEADER"; cat "$tmp"; } > "$bout/ablation_results.csv"
    rm -f "$tmp"
    printf '%-16s merged -> %d config rows in %s/ablation_results.csv\n' "$bname" "$n" "$bname"
done
