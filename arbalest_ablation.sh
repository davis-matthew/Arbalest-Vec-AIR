#!/usr/bin/env bash
# arbalest_ablation.sh
#
# Full ablation study of Arbalest flag combinations on a source file.
# For each configuration, reports IR-level call counts (by type), compile-time
# overhead, optional binary runtime, and Arbalest/TSan violation output.
# All instrumented IRs are saved to a per-input output directory for review.
#
# Usage:
#   ./arbalest_ablation.sh [options] <file.ll|file.c>
#
# Options:
#   --no-color       Disable ANSI color output
#   --no-run         Skip binary compilation and execution (implied for .ll)
#   --ompsan         Also run each config with -arbalest-ompsan and report the
#                    reduction in instrumented call sites (before vs after
#                    OMPSan static analysis guidance).
#   --out-dir DIR    Directory to write IRs (default: <stem>.arbalest_ablation)
#   --cflags "..."   Extra flags forwarded to every clang invocation.
#                    Use this to supply headers, link paths, etc. that your
#                    source file needs.  Example:
#                      --cflags "-I/usr/lib/gcc/x86_64-redhat-linux/11/include"
#
# Configurations tested (dedup × hoist):
#   [0] baseline       off   × false  — raw instrumentation, no optimizations
#   [1] hoist-only     off   × true   — loop hoisting; cstride path → SIMD when
#                                       step==16 && elem==16 at runtime
#   [2] dedup:strip    strip × false  — stripPointerCasts() dedup, no hoist
#   [3] dedup:value    value × false  — value-identity dedup, no hoist
#   [4] hoist+strip    strip × true   — both: hoist first, dedup residual
#   [5] hoist+value    value × true   — both: hoist first, dedup residual
#
# With --ompsan, each config above is re-run with -arbalest-ompsan=1, which
# routes the OMPSan data-mapping analysis first.  Only functions flagged by
# OMPSan (true + false positives) are instrumented.  The final "OMPSan Impact"
# table shows the change in call-site count for every dedup×hoist pair.

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD=/scratch/mdavis/arbalest-build

OPT=$BUILD/bin/opt
CLANG=$BUILD/bin/clang
OMP_INC=$BUILD/projects/openmp/runtime/src   # omp.h from the in-tree OpenMP build

# ── args ──────────────────────────────────────────────────────────────────────
USE_COLOR=true
NO_RUN=false
RUN_OMPSAN=false
OUT_DIR=""
EXTRA_CFLAGS=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-color)         USE_COLOR=false; shift ;;
        --no-run)           NO_RUN=true;     shift ;;
        --ompsan)           RUN_OMPSAN=true;  shift ;;
        --out-dir)          OUT_DIR="$2";    shift 2 ;;
        --out-dir=*)        OUT_DIR="${1#*=}"; shift ;;
        --cflags)           EXTRA_CFLAGS="$2"; shift 2 ;;
        --cflags=*)         EXTRA_CFLAGS="${1#*=}"; shift ;;
        -*)  printf 'Unknown option: %s\n' "$1" >&2; exit 1 ;;
        *)   break ;;
    esac
done

INPUT="${1:-}"
if [[ -z "$INPUT" || ! -f "$INPUT" ]]; then
    printf 'Usage: %s [--no-color] [--no-run] [--out-dir DIR] [--cflags "FLAGS"] <file.ll|file.c>\n' "$0" >&2
    exit 1
fi

EXT="${INPUT##*.}"
INPUT_STEM="$(basename "${INPUT%.*}")"
[[ "$EXT" == "ll" ]] && NO_RUN=true

# Default output directory beside the input file
[[ -z "$OUT_DIR" ]] && OUT_DIR="$(dirname "$(realpath "$INPUT")")/${INPUT_STEM}.arbalest_ablation"
mkdir -p "$OUT_DIR"

# Temp dir for binaries (cleaned up on exit; IRs go to OUT_DIR)
TMPDIR_ABL=$(mktemp -d /tmp/arbalest_ablation.XXXXXX)
trap 'rm -rf "$TMPDIR_ABL"' EXIT

# ── colors ────────────────────────────────────────────────────────────────────
green() { $USE_COLOR && printf '\033[32m%s\033[0m' "$*" || printf '%s' "$*"; }
red()   { $USE_COLOR && printf '\033[31m%s\033[0m' "$*" || printf '%s' "$*"; }
bold()  { $USE_COLOR && printf '\033[1m%s\033[0m\n'  "$*" || printf '%s\n' "$*"; }
dim()   { $USE_COLOR && printf '\033[2m%s\033[0m'   "$*" || printf '%s' "$*"; }

# ── preflight ─────────────────────────────────────────────────────────────────
for b in "$OPT" "$CLANG"; do
    [[ -x "$b" ]] || { printf 'ERROR: not executable: %s\n' "$b" >&2; exit 1; }
done

# ── ms timestamp ──────────────────────────────────────────────────────────────
ms_now() { printf '%d' "$(( $(date +%s%N) / 1000000 ))"; }

# ── configuration matrix ──────────────────────────────────────────────────────
# Each entry: "short_slug|display label|dedupe_mode|hoist"
CFGS=(
    "baseline|baseline (no dedup, no hoist)|off|false"
    "hoist_only|hoist only  (SIMD via cstride at runtime)|off|true"
    "dedup_strip|dedup:strip, no hoist|strip|false"
    "dedup_value|dedup:value, no hoist|value|false"
    "hoist_strip|hoist + dedup:strip|strip|true"
    "hoist_value|hoist + dedup:value|value|true"
)
NC=${#CFGS[@]}

cfg_slug()   { cut -d'|' -f1 <<< "${CFGS[$1]}"; }
cfg_label()  { cut -d'|' -f2 <<< "${CFGS[$1]}"; }
cfg_dedupe() { cut -d'|' -f3 <<< "${CFGS[$1]}"; }
cfg_hoist()  { cut -d'|' -f4 <<< "${CFGS[$1]}"; }

# ── pass pipeline (no literal single-quotes; used as -passes="$PASSES_FUNC") ──
PASSES_FUNC='arbalest-module,function(arbalest)'

# ── call-type patterns ────────────────────────────────────────────────────────
# Per-element aligned reads/writes
PE_TYPES=(read1 read2 read4 read8 read16 write1 write2 write4 write8 write16)
# Per-element unaligned reads/writes
UA_TYPES=(unaligned_read2 unaligned_read4 unaligned_read8 unaligned_read16
          unaligned_write2 unaligned_write4 unaligned_write8 unaligned_write16)
# Hoisted range/stride calls
HT_TYPES=(read_range write_range read_cstride write_cstride read_stride write_stride)

# ── IR generation ─────────────────────────────────────────────────────────────
# Writes IR to stdout; stderr goes to the per-config log file (set by caller).
# $4 (ompsan): if "true", add -arbalest-ompsan=1 so that only functions flagged
#              by the OMPSan data-mapping analysis are instrumented.
emit_ir() {
    local dedupe="$1" hoist="$2" logfile="${3:-/dev/null}" ompsan="${4:-false}"
    local ompsan_flag=()
    $ompsan && ompsan_flag=(-arbalest-ompsan=1)

    if [[ "$EXT" == "ll" ]]; then
        $OPT < "$INPUT" -passes="$PASSES_FUNC" \
            -arbalest=1 \
            -arbalest-dedupe-mode="$dedupe" \
            -arbalest-hoist="$hoist" \
            "${ompsan_flag[@]}" \
            -S 2>"$logfile"
    else
        local mllvm_ompsan=()
        $ompsan && mllvm_ompsan=(-mllvm -arbalest-ompsan=1)
        # shellcheck disable=SC2086
        $CLANG -target x86_64-unknown-linux-gnu \
            -fsanitize=thread -fopenmp \
            -fopenmp-targets=x86_64-pc-linux-gnu \
            -farbalest \
            -O1 \
            -I"$OMP_INC" \
            $EXTRA_CFLAGS \
            -mllvm -arbalest-dedupe-mode="$dedupe" \
            -mllvm -arbalest-hoist="$hoist" \
            "${mllvm_ompsan[@]}" \
            -S -emit-llvm "$INPUT" -o - 2>"$logfile"
    fi
}

# Count call instructions matching @__arbalest_<suffix>( in an IR file.
# Requires "call" before the name so declare lines are excluded.
# The trailing \( prevents read1 matching read16, etc.
# grep -c exits 1 (no matches) but still prints "0"; capture with || n=0
# to avoid the double-print that occurs with the naive "|| printf '0'" form.
#
# IMPORTANT: @llvm.embedded.object is the device IR stored as a single-line
# text-escaped string constant in the host module (used by -fopenmp-targets).
# It contains escaped call instructions (e.g. "\0Acall void @__arbalest_..."),
# which grep would count as real calls. Strip that line before matching.
count_type() {
    local ir_file="$1" suffix="$2"
    local n
    n=$(grep -v '@llvm\.embedded\.object' "$ir_file" | \
        grep -cE "\bcall\b.*@__arbalest_${suffix}\(" 2>/dev/null) || n=0
    printf '%d' "${n:-0}"
}

# ── collect results ───────────────────────────────────────────────────────────
printf '\nCollecting data for %s\n' "$(basename "$INPUT")"
printf 'IR output directory: %s\n\n' "$OUT_DIR"

declare -a IR_FILE      # path to saved IR per config
declare -a IR_MS        # ms to generate IR per config
declare -a PE_TOTAL     # total per-element aligned calls per config
declare -a UA_TOTAL     # total per-element unaligned calls per config
declare -a HT_TOTAL     # total hoisted calls per config
declare -a BIN_MS       # binary run time in ms ("-" or "ERR" if skipped/failed)
declare -a VIOLATIONS   # captured stderr from binary run per config

declare -A PE_CNT       # PE_CNT[ci,type] — aligned per-element by type
declare -A UA_CNT       # UA_CNT[ci,type] — unaligned per-element by type
declare -A HT_CNT       # HT_CNT[ci,type] — hoisted by type

# OMPSan-guided variants (populated only when --ompsan is set)
declare -a OS_IR_FILE   # path to saved OMPSan-guided IR per config
declare -a OS_IR_MS     # ms to generate OMPSan-guided IR per config
declare -a OS_PE_TOTAL  # per-element aligned calls in OMPSan-guided IR
declare -a OS_UA_TOTAL  # per-element unaligned calls in OMPSan-guided IR
declare -a OS_HT_TOTAL  # hoisted calls in OMPSan-guided IR
declare -a OS_OK        # "ok" or "fail" — whether IR generation succeeded

for (( ci=0; ci<NC; ci++ )); do
    slug=$(cfg_slug  $ci)
    dedupe=$(cfg_dedupe $ci)
    hoist=$(cfg_hoist  $ci)

    ir_path="$OUT_DIR/cfg${ci}_${slug}.ll"
    ir_log="$OUT_DIR/cfg${ci}_${slug}.log"
    IR_FILE[$ci]="$ir_path"

    printf '  [%d] %-45s ... ' "$ci" "$(cfg_label $ci)"

    t0=$(ms_now)
    emit_ir "$dedupe" "$hoist" "$ir_log" > "$ir_path"
    ir_ok=$?
    t1=$(ms_now)
    IR_MS[$ci]=$(( t1 - t0 ))

    # Warn immediately if IR generation failed (log has the compiler error)
    if [[ $ir_ok -ne 0 || ! -s "$ir_path" ]]; then
        printf '\n  WARNING: IR generation failed for [%d]; see %s\n' "$ci" "$ir_log" >&2
        if [[ -s "$ir_log" ]]; then
            head -3 "$ir_log" | sed 's/^/    /' >&2
        fi
    fi

    # Count per-element aligned
    pe_total=0
    for typ in "${PE_TYPES[@]}"; do
        n=$(count_type "$ir_path" "$typ")
        PE_CNT["$ci,$typ"]=${n:-0}
        pe_total=$(( pe_total + ${n:-0} ))
    done
    PE_TOTAL[$ci]=$pe_total

    # Count per-element unaligned
    ua_total=0
    for typ in "${UA_TYPES[@]}"; do
        n=$(count_type "$ir_path" "$typ")
        UA_CNT["$ci,$typ"]=${n:-0}
        ua_total=$(( ua_total + ${n:-0} ))
    done
    UA_TOTAL[$ci]=$ua_total

    # Count hoisted
    ht_total=0
    for typ in "${HT_TYPES[@]}"; do
        n=$(count_type "$ir_path" "$typ")
        HT_CNT["$ci,$typ"]=${n:-0}
        ht_total=$(( ht_total + ${n:-0} ))
    done
    HT_TOTAL[$ci]=$ht_total

    # Binary compilation + run (only for .c files)
    if $NO_RUN; then
        BIN_MS[$ci]="-"
        VIOLATIONS[$ci]=""
        printf 'IR(%dms)\n' "${IR_MS[$ci]}"
    else
        binpath="$TMPDIR_ABL/bin_${ci}"
        bin_log="$OUT_DIR/cfg${ci}_${slug}_link.log"
        # shellcheck disable=SC2086
        if $CLANG -target x86_64-unknown-linux-gnu \
                -fsanitize=thread -fopenmp \
                -fopenmp-targets=x86_64-pc-linux-gnu \
                -farbalest \
                -O1 \
                -I"$OMP_INC" \
                $EXTRA_CFLAGS \
                -mllvm -arbalest-dedupe-mode="$dedupe" \
                -mllvm -arbalest-hoist="$hoist" \
                -L"$BUILD/lib" -Wl,-rpath,"$BUILD/lib" \
                "$INPUT" -o "$binpath" 2>"$bin_log" \
           && [[ -x "$binpath" ]]; then
            t0=$(ms_now)
            viol=$(LD_LIBRARY_PATH="$BUILD/lib:${LD_LIBRARY_PATH:-}" \
                   OMP_TOOL_LIBRARIES="$BUILD/lib/libarcher.so" \
                   TSAN_OPTIONS='ignore_noninstrumented_modules=1' \
                   "$binpath" 2>&1 || true)
            t1=$(ms_now)
            BIN_MS[$ci]=$(( t1 - t0 ))
            VIOLATIONS[$ci]="$viol"
            printf 'IR(%dms) run(%dms)\n' "${IR_MS[$ci]}" "${BIN_MS[$ci]}"
        else
            BIN_MS[$ci]="ERR"
            VIOLATIONS[$ci]="(compilation failed; see ${bin_log})"
            printf 'IR(%dms) run(ERR)\n' "${IR_MS[$ci]}"
            if [[ -s "$bin_log" ]]; then
                head -3 "$bin_log" | sed 's/^/    /' >&2
            fi
        fi
    fi
done
printf '\n'

# ── OMPSan-guided data collection ─────────────────────────────────────────────
# Re-run each dedup×hoist config with -arbalest-ompsan=1.  OMPSan runs as a
# module analysis before Arbalest and restricts instrumentation to functions it
# identifies as data-mapping suspects (true + false positives).
if $RUN_OMPSAN; then
    printf 'Collecting OMPSan-guided data (--ompsan mode)\n\n'
    for (( ci=0; ci<NC; ci++ )); do
        slug=$(cfg_slug  $ci)
        dedupe=$(cfg_dedupe $ci)
        hoist=$(cfg_hoist  $ci)

        os_ir_path="$OUT_DIR/cfg${ci}_${slug}_ompsan.ll"
        os_ir_log="$OUT_DIR/cfg${ci}_${slug}_ompsan.log"
        OS_IR_FILE[$ci]="$os_ir_path"

        printf '  [%d] %-45s +OMPSan ... ' "$ci" "$(cfg_label $ci)"

        t0=$(ms_now)
        emit_ir "$dedupe" "$hoist" "$os_ir_log" "true" > "$os_ir_path"
        os_ok=$?
        t1=$(ms_now)
        OS_IR_MS[$ci]=$(( t1 - t0 ))

        if [[ $os_ok -ne 0 || ! -s "$os_ir_path" ]]; then
            OS_OK[$ci]="fail"
            OS_PE_TOTAL[$ci]=0
            OS_UA_TOTAL[$ci]=0
            OS_HT_TOTAL[$ci]=0
            printf 'FAILED (see %s)\n' "$os_ir_log"
            if [[ -s "$os_ir_log" ]]; then
                head -3 "$os_ir_log" | sed 's/^/    /' >&2
            fi
            continue
        fi
        OS_OK[$ci]="ok"

        os_pe=0
        for typ in "${PE_TYPES[@]}"; do
            n=$(count_type "$os_ir_path" "$typ")
            os_pe=$(( os_pe + ${n:-0} ))
        done
        OS_PE_TOTAL[$ci]=$os_pe

        os_ua=0
        for typ in "${UA_TYPES[@]}"; do
            n=$(count_type "$os_ir_path" "$typ")
            os_ua=$(( os_ua + ${n:-0} ))
        done
        OS_UA_TOTAL[$ci]=$os_ua

        os_ht=0
        for typ in "${HT_TYPES[@]}"; do
            n=$(count_type "$os_ir_path" "$typ")
            os_ht=$(( os_ht + ${n:-0} ))
        done
        OS_HT_TOTAL[$ci]=$os_ht

        printf 'IR(%dms)\n' "${OS_IR_MS[$ci]}"
    done
    printf '\n'
fi

# ── summary table ─────────────────────────────────────────────────────────────
BASE_PE=${PE_TOTAL[0]}
BASE_UA=${UA_TOTAL[0]}
BASE_ALL=$(( BASE_PE + BASE_UA ))
LW=44   # label column width

bold "════ Arbalest Ablation Study: $(basename "$INPUT") ════"
printf '\n'

# Print saved IR list
bold '── Saved IRs ───────────────────────────────────────────────────────────'
for (( ci=0; ci<NC; ci++ )); do
    printf '  [%d] %s\n' "$ci" "${IR_FILE[$ci]}"
done
printf '\n'

bold '── Summary ─────────────────────────────────────────────────────────────'
SEP="$(printf '─%.0s' {1..95})"

# Header
if $NO_RUN; then
    printf '  %-*s  %8s  %8s  %8s  %8s  %8s\n' \
        $LW "configuration" "pe-calls" "unalign" "hoisted" "pe-saved" "IR(ms)"
else
    printf '  %-*s  %8s  %8s  %8s  %8s  %8s  %8s\n' \
        $LW "configuration" "pe-calls" "unalign" "hoisted" "pe-saved" "IR(ms)" "run(ms)"
fi
printf '  %s\n' "$SEP"

for (( ci=0; ci<NC; ci++ )); do
    pe=${PE_TOTAL[$ci]}
    ua=${UA_TOTAL[$ci]}
    ht=${HT_TOTAL[$ci]}

    if [[ $BASE_ALL -gt 0 ]]; then
        elim=$(( BASE_ALL - pe - ua ))
        pct=$(( elim * 100 / BASE_ALL ))
    else
        pct=0
    fi

    if $NO_RUN; then
        printf '  %-*s  %8d  %8d  %8d  %7d%%  %8d\n' \
            $LW "[$ci] $(cfg_label $ci)" "$pe" "$ua" "$ht" "$pct" "${IR_MS[$ci]}"
    else
        printf '  %-*s  %8d  %8d  %8d  %7d%%  %8d  %8s\n' \
            $LW "[$ci] $(cfg_label $ci)" "$pe" "$ua" "$ht" "$pct" \
            "${IR_MS[$ci]}" "${BIN_MS[$ci]}"
    fi
done
printf '\n'

# ── per-element aligned breakdown ─────────────────────────────────────────────
bold '── Per-element call breakdown (aligned) ────────────────────────────────'

# Header row: r1 r2 r4 r8 r16 | w1 w2 w4 w8 w16 | total
printf '  %-*s' $LW "configuration"
for typ in "${PE_TYPES[@]}"; do
    abbr="${typ/read/r}"; abbr="${abbr/write/w}"
    printf '  %4s' "$abbr"
done
printf '  %6s\n' "total"
printf '  %s\n' "$SEP"

for (( ci=0; ci<NC; ci++ )); do
    printf '  %-*s' $LW "[$ci] $(cfg_label $ci)"
    row_total=0
    for typ in "${PE_TYPES[@]}"; do
        n=${PE_CNT["$ci,$typ"]:-0}
        printf '  %4d' "$n"
        row_total=$(( row_total + n ))
    done
    printf '  %6d\n' "$row_total"
done
printf '\n'

# ── unaligned breakdown (only if any exist across all configs) ─────────────────
ua_any=0
for (( ci=0; ci<NC; ci++ )); do
    [[ ${UA_TOTAL[$ci]:-0} -gt 0 ]] && ua_any=1 && break
done

if [[ $ua_any -eq 1 ]]; then
    bold '── Per-element call breakdown (unaligned) ──────────────────────────────'

    printf '  %-*s' $LW "configuration"
    for typ in "${UA_TYPES[@]}"; do
        # unaligned_read4 -> ur4, unaligned_write4 -> uw4
        abbr="${typ/unaligned_read/ur}"; abbr="${abbr/unaligned_write/uw}"
        printf '  %5s' "$abbr"
    done
    printf '  %6s\n' "total"
    printf '  %s\n' "$SEP"

    for (( ci=0; ci<NC; ci++ )); do
        printf '  %-*s' $LW "[$ci] $(cfg_label $ci)"
        row_total=0
        for typ in "${UA_TYPES[@]}"; do
            n=${UA_CNT["$ci,$typ"]:-0}
            printf '  %5d' "$n"
            row_total=$(( row_total + n ))
        done
        printf '  %6d\n' "$row_total"
    done
    printf '\n'
fi

# ── hoisted call breakdown ─────────────────────────────────────────────────────
bold '── Hoisted call breakdown ──────────────────────────────────────────────'

printf '  %-*s  %8s  %8s  %8s  %8s  %8s  %8s  %6s\n' \
    $LW "configuration" \
    "rd_range" "wr_range" "rd_cstrd" "wr_cstrd" "rd_strd" "wr_strd" "total"
printf '  %s\n' "$SEP"

for (( ci=0; ci<NC; ci++ )); do
    printf '  %-*s' $LW "[$ci] $(cfg_label $ci)"
    row_total=0
    for typ in "${HT_TYPES[@]}"; do
        n=${HT_CNT["$ci,$typ"]:-0}
        printf '  %8d' "$n"
        row_total=$(( row_total + n ))
    done
    printf '  %6d\n' "$row_total"
done
printf '\n'

# ── runtime call projection ────────────────────────────────────────────────────
bold '── Projected runtime call count (per loop invocation at trip count T) ──'
printf '\n'
printf '  Hoisted calls execute once per invocation; per-element execute T times.\n'
printf '  SIMD specialization in __arbalest_{read,write}_cstride activates at\n'
printf '  runtime when step==16 && elem_bytes==16 (SSE2 _mm_storeu_si128 path).\n'
printf '\n'

printf '  %-*s  %12s  %12s  %12s  %12s\n' \
    $LW "configuration" "T=1" "T=10" "T=100" "T=1000"
printf '  %s\n' "$SEP"

for (( ci=0; ci<NC; ci++ )); do
    pe=$(( PE_TOTAL[$ci] + UA_TOTAL[$ci] ))
    ht=${HT_TOTAL[$ci]}
    printf '  %-*s  %12d  %12d  %12d  %12d\n' \
        $LW "[$ci] $(cfg_label $ci)" \
        "$(( pe*1    + ht ))" \
        "$(( pe*10   + ht ))" \
        "$(( pe*100  + ht ))" \
        "$(( pe*1000 + ht ))"
done
printf '\n'

# ── calls removed vs baseline ──────────────────────────────────────────────────
bold '── Calls removed vs baseline [0] ───────────────────────────────────────'
printf '\n'
printf '  Positive = fewer calls at runtime. Hoisted calls count as +1 (not +T).\n'
printf '\n'
printf '  %-*s  %12s  %12s  %12s  %12s\n' \
    $LW "configuration" "T=1" "T=10" "T=100" "T=1000"
printf '  %s\n' "$SEP"

base_pe=$(( PE_TOTAL[0] + UA_TOTAL[0] ))
base_ht=${HT_TOTAL[0]}

for (( ci=0; ci<NC; ci++ )); do
    pe=$(( PE_TOTAL[$ci] + UA_TOTAL[$ci] ))
    ht=${HT_TOTAL[$ci]}

    r1=$(( (base_pe*1    + base_ht) - (pe*1    + ht) ))
    r10=$(( (base_pe*10   + base_ht) - (pe*10   + ht) ))
    r100=$(( (base_pe*100  + base_ht) - (pe*100  + ht) ))
    r1000=$(( (base_pe*1000 + base_ht) - (pe*1000 + ht) ))

    if [[ $ci -eq 0 ]]; then
        printf '  %-*s  %12s  %12s  %12s  %12s\n' \
            $LW "[$ci] $(cfg_label $ci)" "— (baseline)" "" "" ""
    else
        printf '  %-*s  %12d  %12d  %12d  %12d\n' \
            $LW "[$ci] $(cfg_label $ci)" "$r1" "$r10" "$r100" "$r1000"
    fi
done
printf '\n'

# ── violation reports ──────────────────────────────────────────────────────────
bold '── Violation reports ───────────────────────────────────────────────────'

if $NO_RUN; then
    printf '\n'
    printf '  Not available for .ll files or when --no-run is set.\n'
    printf '  Pass a .c file with a main() and actual data-mapping violations to\n'
    printf '  see TSan/Arbalest output per configuration.\n'
else
    any_violations=false
    for (( ci=0; ci<NC; ci++ )); do
        v="${VIOLATIONS[$ci]}"
        # Only print section if there's non-trivial output (violations or errors)
        if [[ -n "$v" && "$v" != *"(no output)"* ]]; then
            any_violations=true
            printf '\n'
            $USE_COLOR && printf '  \033[1m[%d] %s\033[0m\n' "$ci" "$(cfg_label $ci)" \
                       || printf '  [%d] %s\n' "$ci" "$(cfg_label $ci)"
            printf '  %s\n' "$(printf '─%.0s' {1..70})"
            while IFS= read -r line; do
                printf '  %s\n' "$line"
            done <<< "$v"
        fi
    done
    if ! $any_violations; then
        printf '\n  (no output from any configuration)\n'
    fi
fi

printf '\n'

# ── OMPSan impact table ────────────────────────────────────────────────────────
if $RUN_OMPSAN; then
    bold '── OMPSan Impact: before vs after static analysis guidance ─────────────'
    printf '\n'
    printf '  OMPSan pre-screens OpenMP data-mapping and flags suspect functions.\n'
    printf '  "before" = full Arbalest instrumentation for that dedup×hoist config.\n'
    printf '  "after"  = same config with -arbalest-ompsan: only flagged functions\n'
    printf '             are instrumented (true + false positives from OMPSan).\n'
    printf '  Columns: total = pe + unaligned + hoisted call sites in the IR.\n'
    printf '\n'

    # Header
    printf '  %-*s  %10s  %10s  %10s  %8s\n' \
        $LW "configuration" "before" "after" "removed" "saved%"
    printf '  %s\n' "$SEP"

    for (( ci=0; ci<NC; ci++ )); do
        before_total=$(( PE_TOTAL[$ci] + UA_TOTAL[$ci] + HT_TOTAL[$ci] ))

        if [[ "${OS_OK[$ci]:-fail}" != "ok" ]]; then
            printf '  %-*s  %10d  %10s  %10s  %8s\n' \
                $LW "[$ci] $(cfg_label $ci)" \
                "$before_total" "N/A (failed)" "" ""
            continue
        fi

        after_total=$(( OS_PE_TOTAL[$ci] + OS_UA_TOTAL[$ci] + OS_HT_TOTAL[$ci] ))
        removed=$(( before_total - after_total ))

        if [[ $before_total -gt 0 ]]; then
            saved_pct=$(( removed * 100 / before_total ))
        else
            saved_pct=0
        fi

        # Colour the saved% green when positive, red when negative (more calls)
        if [[ $removed -gt 0 ]]; then
            pct_str="$(green "${saved_pct}%")"
        elif [[ $removed -lt 0 ]]; then
            pct_str="$(red "${saved_pct}%")"
        else
            pct_str="${saved_pct}%"
        fi

        printf '  %-*s  %10d  %10d  %10d  %8s\n' \
            $LW "[$ci] $(cfg_label $ci)" \
            "$before_total" "$after_total" "$removed" "$pct_str"
    done
    printf '\n'

    # Per-category breakdown (pe / unaligned / hoisted separately)
    bold '── OMPSan Impact: per-category breakdown ───────────────────────────────'
    printf '\n'
    printf '  %-*s  %6s %6s  %6s %6s  %6s %6s\n' \
        $LW "configuration" \
        "pe-b" "pe-a" "ua-b" "ua-a" "ht-b" "ht-a"
    printf '  %-*s  %13s  %13s  %13s\n' \
        $LW "" \
        "(per-element aligned)" "(unaligned)" "(hoisted)"
    printf '  %s\n' "$SEP"

    for (( ci=0; ci<NC; ci++ )); do
        if [[ "${OS_OK[$ci]:-fail}" != "ok" ]]; then
            printf '  %-*s  %6d %6s  %6d %6s  %6d %6s\n' \
                $LW "[$ci] $(cfg_label $ci)" \
                "${PE_TOTAL[$ci]}" "N/A" \
                "${UA_TOTAL[$ci]}" "N/A" \
                "${HT_TOTAL[$ci]}" "N/A"
        else
            printf '  %-*s  %6d %6d  %6d %6d  %6d %6d\n' \
                $LW "[$ci] $(cfg_label $ci)" \
                "${PE_TOTAL[$ci]}"    "${OS_PE_TOTAL[$ci]}" \
                "${UA_TOTAL[$ci]}"    "${OS_UA_TOTAL[$ci]}" \
                "${HT_TOTAL[$ci]}"    "${OS_HT_TOTAL[$ci]}"
        fi
    done
    printf '\n'

    # List OMPSan-guided IR files
    bold '── OMPSan-guided IRs ───────────────────────────────────────────────────'
    for (( ci=0; ci<NC; ci++ )); do
        status="[${OS_OK[$ci]:-fail}]"
        printf '  [%d] %-6s  %s  (%dms)\n' \
            "$ci" "$status" "${OS_IR_FILE[$ci]}" "${OS_IR_MS[$ci]:-0}"
    done
    printf '\n'
fi

bold '════════════════════════════════════════════════════════════════════════'
