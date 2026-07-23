#!/usr/bin/env bash
# arbalest_ablation_apply_violations.sh
#
# Applies violations_<buildkey>.csv corrections (from
# arbalest_ablation_reverify.sh) onto a benchmark's ablation_results.csv,
# overwriting ONLY the has_violation column for matching config_index rows.
# Leaves every other column (static/dynamic/run_ms/mem_kb/etc.) untouched.
#
# Usage: ./arbalest_ablation_apply_violations.sh [--out-dir DIR] [bench ...]

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
        [[ -f "$d/ablation_results.csv" ]] && BENCHES+=("$(basename "$d")")
    done
fi

for bname in "${BENCHES[@]}"; do
    bout="$OUT_DIR/$bname"
    csv="$bout/ablation_results.csv"
    [[ -f "$csv" ]] || { printf 'WARNING: no ablation_results.csv for %s\n' "$bname" >&2; continue; }

    corrections=("$bout"/violations_*.csv)
    [[ -e "${corrections[0]}" ]] || { printf '%-16s no violations_*.csv to apply, skipping\n' "$bname"; continue; }

    tmp=$(mktemp -p "$bout")
    python3 - "$csv" "$tmp" "${corrections[@]}" << 'PYEOF'
import csv, sys

csv_path, tmp_path = sys.argv[1], sys.argv[2]
correction_files = sys.argv[3:]

corrections = {}
n_corrections = 0
for cf in correction_files:
    with open(cf, newline="") as f:
        for row in csv.DictReader(f):
            corrections[row["config_index"]] = row["has_violation"]
            n_corrections += 1

with open(csv_path, newline="") as f:
    reader = csv.DictReader(f)
    fieldnames = reader.fieldnames
    rows = list(reader)

n_applied = 0
for r in rows:
    ci = r["config_index"]
    if ci in corrections and r["has_violation"] != corrections[ci]:
        r["has_violation"] = corrections[ci]
        n_applied += 1

with open(tmp_path, "w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=fieldnames)
    writer.writeheader()
    writer.writerows(rows)

print(f"  {n_corrections} corrections loaded, {n_applied} rows changed")
PYEOF
    mv "$tmp" "$csv"
    printf '%-16s applied corrections from %d file(s)\n' "$bname" "${#corrections[@]}"
done
