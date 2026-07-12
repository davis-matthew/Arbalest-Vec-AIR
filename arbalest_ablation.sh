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
#   --ompsan         Append an OMPSan-guided mirror of all 6 configs below
#                    (same dedup×hoist, plus -arbalest-ompsan=1) to the
#                    matrix, so NC becomes 12. The OMPSan-guided rows get the
#                    exact same treatment as the rest — compiled, run, static
#                    + dynamic call counts, timing — not just an IR diff.
#                    Every report table prints rows 0..5, a divider, then
#                    rows 6..11.
#   --out-dir DIR    Directory to write IRs (default: <stem>.arbalest_ablation)
#   --cflags "..."   Extra flags forwarded to every clang invocation.
#                    Use this to supply headers, link paths, etc. that your
#                    source file needs.  Example:
#                      --cflags "-I/usr/lib/gcc/x86_64-redhat-linux/11/include"
#   --input-sizes "N1 N2 ..."
#                    Run each compiled binary once per size, passing the size
#                    as argv[1] (e.g. "$binpath" "N1"). Your source file's
#                    main() must itself read argv[1] and use it (e.g.
#                    `int n = argc > 1 ? atoi(argv[1]) : DEFAULT;`) for sizes
#                    to actually change behavior — this flag only controls
#                    what gets passed on the command line. Prints one
#                    Instrumentation Summary table per size at the end of the
#                    report. Without this flag, the binary is run once with
#                    no arguments (unchanged default behavior).
#   --repeat N       Run each binary N times per (config, size) and report
#                    the median wall-clock time in run(ms) columns, instead
#                    of a single noisy sample. Default: 3. Call-count and
#                    violation output are only captured from the first run
#                    (they're deterministic for a fixed input — only timing
#                    needs repeats).
#
# Configurations tested (dedup × hoist × simd):
#   [0] baseline               off   × false ×   —   — raw instrumentation
#   [1] dedup:strip            strip × false ×   —   — dedup, no hoist
#   [2] dedup:value            value × false ×   —   — dedup, no hoist
#   [3] hoist-only, no SIMD    off   × true  × false — loop hoisting only
#   [4] hoist-only, with SIMD  off   × true  × true  — + SSE2 fast path for
#                                                       16-byte-aligned ranges
#   [5] hoist+strip, no SIMD   strip × true  × false — hoist first, dedup residual
#   [6] hoist+strip, with SIMD strip × true  × true
#   [7] hoist+value, no SIMD   value × true  × false
#   [8] hoist+value, with SIMD value × true  × true
#
# SIMD is a *runtime* dispatch inside __arbalest_{read,write}_range (unit-
# stride hoisted accesses — where step==elem_bytes — always land there, never
# in cstride; see tsan_arbalest_interface.inc), not a separate compiled code
# path, so the no-SIMD/with-SIMD pair of a given dedup×hoist combo share
# identical IR/call sites and differ only in whether ARBALEST_DISABLE_SIMD is
# set when the binary runs — isolating the SSE2 fast path's runtime cost
# (chunking a 16-byte-aligned range into SSE2 words) from hoisting itself.
#
# With --ompsan, configs [9]..[17] are OMPSan-guided mirrors of [0]..[8]: same
# dedup×hoist, plus -arbalest-ompsan=1, which routes the OMPSan data-mapping
# analysis first so only functions it flags (true + false positives) get
# instrumented. The "OMPSan Impact" table pairs [i] with [i+6].

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
INPUT_SIZES_RAW=""
REPEAT=3

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-color)         USE_COLOR=false; shift ;;
        --no-run)           NO_RUN=true;     shift ;;
        --ompsan)           RUN_OMPSAN=true;  shift ;;
        --out-dir)          OUT_DIR="$2";    shift 2 ;;
        --out-dir=*)        OUT_DIR="${1#*=}"; shift ;;
        --cflags)           EXTRA_CFLAGS="$2"; shift 2 ;;
        --cflags=*)         EXTRA_CFLAGS="${1#*=}"; shift ;;
        --input-sizes)      INPUT_SIZES_RAW="$2"; shift 2 ;;
        --input-sizes=*)    INPUT_SIZES_RAW="${1#*=}"; shift ;;
        --repeat)           REPEAT="$2";     shift 2 ;;
        --repeat=*)         REPEAT="${1#*=}"; shift ;;
        -*)  printf 'Unknown option: %s\n' "$1" >&2; exit 1 ;;
        *)   break ;;
    esac
done

if ! [[ "$REPEAT" =~ ^[0-9]+$ ]] || [[ "$REPEAT" -lt 1 ]]; then
    printf 'ERROR: --repeat must be a positive integer (got: %s)\n' "$REPEAT" >&2
    exit 1
fi

# INPUT_SIZES holds the argv[1] values to run each binary with. A single
# empty-string entry means "run with no arguments" (the pre-existing,
# single-run default behavior).
if [[ -n "$INPUT_SIZES_RAW" ]]; then
    # shellcheck disable=SC2206
    INPUT_SIZES=($INPUT_SIZES_RAW)
else
    INPUT_SIZES=("")
fi
MULTI_SIZE=false
[[ ${#INPUT_SIZES[@]} -gt 1 ]] && MULTI_SIZE=true

INPUT="${1:-}"
if [[ -z "$INPUT" || ! -f "$INPUT" ]]; then
    printf 'Usage: %s [--no-color] [--no-run] [--out-dir DIR] [--cflags "FLAGS"] [--input-sizes "N1 N2 ..."] [--repeat N] <file.ll|file.c>\n' "$0" >&2
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

# Median of N integer ms samples (average of the middle two if N is even).
median_of() {
    local -a vals=("$@")
    local -a sorted
    IFS=$'\n' sorted=($(sort -n <<< "${vals[*]}")); unset IFS
    local n=${#sorted[@]} mid=$(( ${#sorted[@]} / 2 ))
    if (( n % 2 == 1 )); then
        printf '%d' "${sorted[$mid]}"
    else
        printf '%d' "$(( (sorted[mid-1] + sorted[mid]) / 2 ))"
    fi
}

# ── configuration matrix ──────────────────────────────────────────────────────
# Each entry: "short_slug|display label|dedupe_mode|hoist|simd|ompsan"
#
# SIMD is a runtime dispatch inside __arbalest_{read,write}_cstride (the SSE2
# fast path fires when step==16 && elem_bytes==16), not a separate compiled
# code path — hoisted configs compile identically either way. So every
# hoist=true config is split into a "no SIMD" / "with SIMD" pair that share
# the same dedup×hoist compile flags and differ only in whether
# ARBALEST_DISABLE_SIMD is set when the binary runs, isolating the SIMD fast
# path's runtime cost from hoisting itself. `simd` is "na" for hoist=false
# configs, where cstride is never emitted so the split is meaningless.
CFGS=(
    "baseline|baseline|off|false|na|false"
    "dedup_strip|dedup:strip|strip|false|na|false"
    "dedup_value|dedup:value|value|false|na|false"
    "hoist_only_nosimd|hoist (no SIMD)|off|true|false|false"
    "hoist_only_simd|hoist (SIMD)|off|true|true|false"
    "hoist_strip_nosimd|hoist+strip (no SIMD)|strip|true|false|false"
    "hoist_strip_simd|hoist+strip (SIMD)|strip|true|true|false"
    "hoist_value_nosimd|hoist+value (no SIMD)|value|true|false|false"
    "hoist_value_simd|hoist+value (SIMD)|value|true|true|false"
)
BASE_NC=${#CFGS[@]}

# --ompsan appends a paired OMPSan-guided mirror of every config above (same
# dedup×hoist×simd, plus -arbalest-ompsan=1) so rows [BASE_NC..NC) get the
# exact same treatment as rows [0..BASE_NC) — compiled, run, static + dynamic
# call counts, timing. Report tables print [0..BASE_NC), a divider, then rest.
if $RUN_OMPSAN; then
    for (( i=0; i<BASE_NC; i++ )); do
        IFS='|' read -r cfgi_slug cfgi_label cfgi_dedupe cfgi_hoist cfgi_simd _ <<< "${CFGS[$i]}"
        CFGS+=("${cfgi_slug}_ompsan|${cfgi_label} +OMPSan|${cfgi_dedupe}|${cfgi_hoist}|${cfgi_simd}|true")
    done
fi
NC=${#CFGS[@]}

cfg_slug()   { cut -d'|' -f1 <<< "${CFGS[$1]}"; }
cfg_label()  { cut -d'|' -f2 <<< "${CFGS[$1]}"; }
cfg_dedupe() { cut -d'|' -f3 <<< "${CFGS[$1]}"; }
cfg_hoist()  { cut -d'|' -f4 <<< "${CFGS[$1]}"; }
cfg_simd()   { cut -d'|' -f5 <<< "${CFGS[$1]}"; }
cfg_ompsan() { cut -d'|' -f6 <<< "${CFGS[$1]}"; }

# Printed between rows [BASE_NC-1] and [BASE_NC] in every per-config report
# table, so the OMPSan-guided rows are visually grouped as their own block.
print_ompsan_divider() {
    if $USE_COLOR; then
        printf '  \033[2m%s\033[0m\n' "── with --ompsan ($BASE_NC configs re-run with -arbalest-ompsan=1) ──"
    else
        printf '  %s\n' "── with --ompsan ($BASE_NC configs re-run with -arbalest-ompsan=1) ──"
    fi
}

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
# IMPORTANT: @llvm.embedded.object is the *device*-side IR (the outlined
# omp-target kernel body), stored as a single-line text-escaped string
# constant in the host module (that's how -fopenmp-targets packages device
# code for offload alongside the host code in the same .ll file). Real call
# sites can live ONLY inside that blob (e.g. a hoisted range/stride call
# emitted for the device kernel's loop) — they never appear unescaped
# anywhere else in the file — so they must be counted, not discarded.
#
# grep -c on the raw blob line would badly undercount: the whole blob is one
# gigantic physical line (original newlines became literal "\0A" two-char
# escape sequences), so `grep -c` — which counts matching *lines*, not
# occurrences — would report at most 1 for a type no matter how many times
# it actually appears in that line. Restore the escaped newlines first so
# each instruction is back on its own line, then reuse the exact same
# per-line counting logic used for the host IR.
count_type() {
    local ir_file="$1" suffix="$2"
    local n_host n_dev
    n_host=$(grep -v '@llvm\.embedded\.object' "$ir_file" | \
        grep -cE "\bcall\b.*@__arbalest_${suffix}\(" 2>/dev/null) || n_host=0
    n_dev=$(grep '@llvm\.embedded\.object' "$ir_file" 2>/dev/null | \
        sed 's/\\0A/\n/g' | \
        grep -cE "\bcall\b.*@__arbalest_${suffix}\(" 2>/dev/null) || n_dev=0
    printf '%d' "$(( ${n_host:-0} + ${n_dev:-0} ))"
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

declare -A RT_CNT       # RT_CNT[ci,type] — actual runtime calls, measured
                        # (mirrors the first --input-sizes entry; existing
                        # single-run sections below read from these)
declare -a RT_TOTAL     # total measured runtime calls per config (first size)
declare -a RT_OK        # "ok" if runtime counts were captured for this config,
                        # else a short reason string (first size)

# Per-(config,size) results, keyed "$ci,$sz". $sz is the literal --input-sizes
# value (or "" for the single no-args default run). Used by the final
# Instrumentation Summary table(s), which are per input size.
declare -A RT_CNT_SZ    # RT_CNT_SZ[ci,sz,type]
declare -A RT_TOTAL_SZ  # RT_TOTAL_SZ[ci,sz]
declare -A RT_OK_SZ     # RT_OK_SZ[ci,sz]
declare -A BIN_MS_SZ    # BIN_MS_SZ[ci,sz]

for (( ci=0; ci<NC; ci++ )); do
    if $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]]; then
        printf '\n  ── now compiling/running the OMPSan-guided variants ──\n\n'
    fi

    slug=$(cfg_slug  $ci)
    dedupe=$(cfg_dedupe $ci)
    hoist=$(cfg_hoist  $ci)
    simd=$(cfg_simd  $ci)
    ompsan=$(cfg_ompsan $ci)

    ir_path="$OUT_DIR/cfg${ci}_${slug}.ll"
    ir_log="$OUT_DIR/cfg${ci}_${slug}.log"
    IR_FILE[$ci]="$ir_path"

    printf '  [%d] %-45s ... ' "$ci" "$(cfg_label $ci)"

    t0=$(ms_now)
    emit_ir "$dedupe" "$hoist" "$ir_log" "$ompsan" > "$ir_path"
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

    # Binary compilation (once per config, independent of --input-sizes) +
    # run (once per entry in INPUT_SIZES; a single "" entry if not given).
    if $NO_RUN; then
        BIN_MS[$ci]="-"
        VIOLATIONS[$ci]=""
        RT_OK[$ci]="no binary (--no-run or .ll input)"
        RT_TOTAL[$ci]=0
        for sz in "${INPUT_SIZES[@]}"; do
            BIN_MS_SZ["$ci,$sz"]="-"
            RT_OK_SZ["$ci,$sz"]="no binary (--no-run or .ll input)"
            RT_TOTAL_SZ["$ci,$sz"]=0
        done
        printf 'IR(%dms)\n' "${IR_MS[$ci]}"
    else
        binpath="$TMPDIR_ABL/bin_${ci}"
        bin_log="$OUT_DIR/cfg${ci}_${slug}_link.log"
        mllvm_ompsan=()
        [[ "$ompsan" == "true" ]] && mllvm_ompsan=(-mllvm -arbalest-ompsan=1)
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
                "${mllvm_ompsan[@]}" \
                -L"$BUILD/lib" -Wl,-rpath,"$BUILD/lib" \
                "$INPUT" -o "$binpath" 2>"$bin_log" \
           && [[ -x "$binpath" ]]; then
            printf 'IR(%dms)' "${IR_MS[$ci]}"
            first_sz=true
            for sz in "${INPUT_SIZES[@]}"; do
                bin_args=()
                [[ -n "$sz" ]] && bin_args=("$sz")

                # Run REPEAT times; report the median wall-clock time (single
                # samples are noisy — dominated by TSan shadow-memory setup
                # and OpenMP thread-pool spin-up, both scheduler-dependent).
                # Call counts/violations are deterministic for a fixed input,
                # so only the first run's output is parsed for those.
                run_env=(
                    "LD_LIBRARY_PATH=$BUILD/lib:${LD_LIBRARY_PATH:-}"
                    "OMP_TOOL_LIBRARIES=$BUILD/lib/libarcher.so"
                    "TSAN_OPTIONS=ignore_noninstrumented_modules=1"
                    # Dumps one "ARBALEST_CALL_COUNT <type> <n>" line per call
                    # type to stderr at exit, so we can measure actual
                    # invocation counts alongside the static (IR call-site)
                    # counts collected above.
                    "ARBALEST_COUNT_CALLS=1"
                )
                # simd=="false" configs disable the SSE2 fast path inside
                # __arbalest_{read,write}_cstride at runtime (see CFGS
                # comment above) — isolates SIMD's cost from hoisting itself.
                [[ "$simd" == "false" ]] && run_env+=("ARBALEST_DISABLE_SIMD=1")

                rep_times=()
                for (( rep=1; rep<=REPEAT; rep++ )); do
                    t0=$(ms_now)
                    run_out=$(env "${run_env[@]}" "$binpath" "${bin_args[@]}" 2>&1 || true)
                    t1=$(ms_now)
                    rep_times+=( $(( t1 - t0 )) )
                    [[ $rep -eq 1 ]] && first_run_out="$run_out"
                done
                bin_ms=$(median_of "${rep_times[@]}")
                BIN_MS_SZ["$ci,$sz"]=$bin_ms
                viol="$(grep -v '^ARBALEST_CALL_COUNT ' <<< "$first_run_out")"

                counts_raw="$(grep '^ARBALEST_CALL_COUNT ' <<< "$first_run_out")"
                if [[ -n "$counts_raw" ]]; then
                    RT_OK_SZ["$ci,$sz"]="ok"
                    rt_total=0
                    while read -r _ rt_typ rt_n; do
                        [[ -z "$rt_typ" ]] && continue
                        RT_CNT_SZ["$ci,$sz,$rt_typ"]=$rt_n
                        rt_total=$(( rt_total + rt_n ))
                    done <<< "$counts_raw"
                    RT_TOTAL_SZ["$ci,$sz"]=$rt_total
                else
                    RT_OK_SZ["$ci,$sz"]="no ARBALEST_CALL_COUNT output (rebuild the tsan runtime with call-count instrumentation)"
                    RT_TOTAL_SZ["$ci,$sz"]=0
                fi

                if [[ -n "$sz" ]]; then
                    printf ' run[n=%s](median of %d: %dms)' "$sz" "$REPEAT" "$bin_ms"
                else
                    printf ' run(median of %d: %dms)' "$REPEAT" "$bin_ms"
                fi

                # Mirror the first size's results into the original unkeyed
                # arrays so every pre-existing single-run section (Measured
                # runtime call counts, Violation reports, etc.) keeps working
                # unchanged, using the first size as "the" representative run.
                if $first_sz; then
                    BIN_MS[$ci]=$bin_ms
                    VIOLATIONS[$ci]="$viol"
                    RT_OK[$ci]="${RT_OK_SZ["$ci,$sz"]}"
                    RT_TOTAL[$ci]=${RT_TOTAL_SZ["$ci,$sz"]}
                    for typ in "${PE_TYPES[@]}" "${UA_TYPES[@]}" "${HT_TYPES[@]}"; do
                        [[ -v "RT_CNT_SZ[$ci,$sz,$typ]" ]] && \
                            RT_CNT["$ci,$typ"]=${RT_CNT_SZ["$ci,$sz,$typ"]}
                    done
                    first_sz=false
                fi
            done
            printf '\n'
        else
            BIN_MS[$ci]="ERR"
            VIOLATIONS[$ci]="(compilation failed; see ${bin_log})"
            RT_OK[$ci]="binary compilation failed"
            RT_TOTAL[$ci]=0
            for sz in "${INPUT_SIZES[@]}"; do
                BIN_MS_SZ["$ci,$sz"]="ERR"
                RT_OK_SZ["$ci,$sz"]="binary compilation failed"
                RT_TOTAL_SZ["$ci,$sz"]=0
            done
            printf 'IR(%dms) run(ERR)\n' "${IR_MS[$ci]}"
            if [[ -s "$bin_log" ]]; then
                head -3 "$bin_log" | sed 's/^/    /' >&2
            fi
        fi
    fi
done
printf '\n'


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
    $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]] && print_ompsan_divider
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
    $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]] && print_ompsan_divider
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
    $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]] && print_ompsan_divider
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
        $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]] && print_ompsan_divider
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
    $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]] && print_ompsan_divider
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

# ── measured runtime call counts (actual execution) ───────────────────────────
# Looks up the static (IR call-site) count for a type, checking whichever of
# PE_CNT/UA_CNT/HT_CNT it belongs to.
static_count() {
    local ci="$1" typ="$2"
    if [[ -v "PE_CNT[$ci,$typ]" ]]; then printf '%s' "${PE_CNT[$ci,$typ]}"
    elif [[ -v "UA_CNT[$ci,$typ]" ]]; then printf '%s' "${UA_CNT[$ci,$typ]}"
    else printf '%s' "${HT_CNT[$ci,$typ]:-0}"
    fi
}

if ! $NO_RUN; then
    bold '── Measured runtime call counts (actual execution, this input) ─────────'
    printf '\n'
    printf '  static  = call sites emitted in the IR (from the tables above).\n'
    printf '  runtime = times that call site'"'"'s __arbalest_* callback actually\n'
    printf '            fired while the binary ran just now. A hoisted call site\n'
    printf '            can fire far fewer times than a per-element site covering\n'
    printf '            the same accesses (e.g. one write_stride call vs. T write\n'
    printf '            calls for a loop of trip count T).\n'
    printf '\n'

    ALL_TYPES=("${PE_TYPES[@]}" "${UA_TYPES[@]}" "${HT_TYPES[@]}")

    for (( ci=0; ci<NC; ci++ )); do
        $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]] && print_ompsan_divider
        $USE_COLOR && printf '  \033[1m[%d] %s\033[0m\n' "$ci" "$(cfg_label $ci)" \
                   || printf '  [%d] %s\n' "$ci" "$(cfg_label $ci)"

        if [[ "${RT_OK[$ci]}" != "ok" ]]; then
            printf '      (%s)\n\n' "${RT_OK[$ci]}"
            continue
        fi

        printf '      %-20s  %10s  %10s\n' "type" "static" "runtime"
        printf '      %s\n' "$(printf -- '-%.0s' {1..44})"
        any_row=0
        for typ in "${ALL_TYPES[@]}"; do
            s=$(static_count "$ci" "$typ")
            r=${RT_CNT["$ci,$typ"]:-0}
            if [[ $s -gt 0 || $r -gt 0 ]]; then
                printf '      %-20s  %10d  %10d\n' "$typ" "$s" "$r"
                any_row=1
            fi
        done
        [[ $any_row -eq 0 ]] && printf '      (no calls, static or runtime)\n'
        printf '      %s\n' "$(printf -- '-%.0s' {1..44})"
        static_all=$(( PE_TOTAL[$ci] + UA_TOTAL[$ci] + HT_TOTAL[$ci] ))
        printf '      %-20s  %10d  %10d\n' "total" "$static_all" "${RT_TOTAL[$ci]}"
        printf '\n'
    done

    bold '── Measured calls removed vs baseline (actual execution) ───────────────'
    printf '\n'
    printf '  Positive = fewer calls actually executed than baseline, for this input.\n'
    if $RUN_OMPSAN; then
        printf '  Baseline is [0] for rows without --ompsan, [%d] for rows with it —\n' "$BASE_NC"
        printf '  i.e. dedup/hoist impact is measured within each OMPSan mode.\n'
    fi
    printf '\n'
    printf '  %-*s  %10s\n' $LW "configuration" "removed"
    printf '  %s\n' "$SEP"
    for (( ci=0; ci<NC; ci++ )); do
        $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]] && print_ompsan_divider
        group_base=$(( ci < BASE_NC ? 0 : BASE_NC ))
        if [[ $ci -eq $group_base ]]; then
            printf '  %-*s  %10s\n' $LW "[$ci] $(cfg_label $ci)" "— (baseline)"
        elif [[ "${RT_OK[$group_base]}" == "ok" && "${RT_OK[$ci]}" == "ok" ]]; then
            printf '  %-*s  %10d\n' $LW "[$ci] $(cfg_label $ci)" \
                "$(( RT_TOTAL[group_base] - RT_TOTAL[$ci] ))"
        else
            printf '  %-*s  %10s\n' $LW "[$ci] $(cfg_label $ci)" "N/A"
        fi
    done
    printf '\n'
fi

# ── runtime call projection ────────────────────────────────────────────────────
bold '── Projected runtime call count (per loop invocation at trip count T) ──'
printf '\n'
printf '  Hoisted calls execute once per invocation; per-element execute T times.\n'
printf '  SIMD fast path in __arbalest_{read,write}_range activates at runtime\n'
printf '  when the range length is a multiple of 16 bytes (SSE2 path) — see the\n'
printf '  "(with/without SIMD)" config pairs for its isolated runtime cost.\n'
printf '\n'

printf '  %-*s  %12s  %12s  %12s  %12s\n' \
    $LW "configuration" "T=1" "T=10" "T=100" "T=1000"
printf '  %s\n' "$SEP"

for (( ci=0; ci<NC; ci++ )); do
    $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]] && print_ompsan_divider
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
bold '── Calls removed vs baseline ─────────────────────────────────────────────'
printf '\n'
printf '  Positive = fewer calls at runtime. Hoisted calls count as +1 (not +T).\n'
if $RUN_OMPSAN; then
    printf '  Baseline is [0] for rows without --ompsan, [%d] for rows with it —\n' "$BASE_NC"
    printf '  i.e. dedup/hoist impact is measured within each OMPSan mode.\n'
fi
printf '\n'
printf '  %-*s  %12s  %12s  %12s  %12s\n' \
    $LW "configuration" "T=1" "T=10" "T=100" "T=1000"
printf '  %s\n' "$SEP"

for (( ci=0; ci<NC; ci++ )); do
    $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]] && print_ompsan_divider
    group_base=$(( ci < BASE_NC ? 0 : BASE_NC ))
    base_pe=$(( PE_TOTAL[group_base] + UA_TOTAL[group_base] ))
    base_ht=${HT_TOTAL[group_base]}
    pe=$(( PE_TOTAL[$ci] + UA_TOTAL[$ci] ))
    ht=${HT_TOTAL[$ci]}

    r1=$(( (base_pe*1    + base_ht) - (pe*1    + ht) ))
    r10=$(( (base_pe*10   + base_ht) - (pe*10   + ht) ))
    r100=$(( (base_pe*100  + base_ht) - (pe*100  + ht) ))
    r1000=$(( (base_pe*1000 + base_ht) - (pe*1000 + ht) ))

    if [[ $ci -eq $group_base ]]; then
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
        if $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]]; then
            printf '\n'
            print_ompsan_divider
        fi
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
# Pairs config [i] (no --ompsan) with its mirror [i+BASE_NC] (-arbalest-ompsan=1)
# directly, using the same unified per-config data every other table above
# uses — including runtime, now that the OMPSan-guided rows are actually
# compiled and run instead of just IR-diffed. The full per-type static +
# dynamic breakdown for both rows of each pair is already in the
# Instrumentation Summary table(s) below; this is just the compact digest.
if $RUN_OMPSAN; then
    bold '── OMPSan Impact: before vs after static analysis guidance (paired) ────'
    printf '\n'
    printf '  OMPSan pre-screens OpenMP data-mapping and flags suspect functions.\n'
    printf '  "before" = full Arbalest instrumentation for that dedup×hoist config.\n'
    printf '  "after"  = same config with -arbalest-ompsan: only flagged functions\n'
    printf '             are instrumented (true + false positives from OMPSan).\n'
    printf '  call columns: total = pe + unaligned + hoisted call sites in the IR.\n'
    printf '  run columns: median executable runtime (see run(ms) above).\n'
    printf '\n'

    printf '  %-*s  %10s  %10s  %10s  %8s  %10s  %10s\n' \
        $LW "configuration" "calls-before" "calls-after" "removed" "saved%" "run-before" "run-after"
    printf '  %s\n' "$SEP"

    for (( i=0; i<BASE_NC; i++ )); do
        j=$(( i + BASE_NC ))
        before_total=$(( PE_TOTAL[i] + UA_TOTAL[i] + HT_TOTAL[i] ))
        after_total=$(( PE_TOTAL[j] + UA_TOTAL[j] + HT_TOTAL[j] ))
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

        printf '  [%d]/[%-2d] %-*s  %10d  %10d  %10d  %8s  %10s  %10s\n' \
            "$i" "$j" $(( LW - 9 )) "$(cfg_label $i)" \
            "$before_total" "$after_total" "$removed" "$pct_str" \
            "${BIN_MS[$i]}" "${BIN_MS[$j]}"
    done
    printf '\n'
fi

# ── instrumentation summary (one table per --input-sizes entry) ───────────────
# One row per configuration: static call-site counts and measured dynamic
# call counts (both broken out by type), plus IR-pass time and executable
# runtime. Only types with a nonzero count somewhere (static or dynamic, any
# config, any size) get a column, to keep the table from being mostly zeros.
abbr_type() {
    local typ="$1" abbr
    case "$typ" in
        read_range)    abbr="rdrng" ;;
        write_range)   abbr="wrrng" ;;
        read_stride)   abbr="rdstr" ;;
        write_stride)  abbr="wrstr" ;;
        read_cstride)  abbr="rdcst" ;;
        write_cstride) abbr="wrcst" ;;
        unaligned_read*)  abbr="ur${typ#unaligned_read}" ;;
        unaligned_write*) abbr="uw${typ#unaligned_write}" ;;
        read*)  abbr="r${typ#read}" ;;
        write*) abbr="w${typ#write}" ;;
        *)      abbr="$typ" ;;
    esac
    printf '%s' "$abbr"
}

ALL_TYPES_SUMMARY=("${PE_TYPES[@]}" "${UA_TYPES[@]}" "${HT_TYPES[@]}")
ACTIVE_TYPES=()
for typ in "${ALL_TYPES_SUMMARY[@]}"; do
    active=false
    for (( ci=0; ci<NC; ci++ )); do
        s=$(static_count "$ci" "$typ")
        if [[ $s -gt 0 ]]; then active=true; break; fi
        for sz in "${INPUT_SIZES[@]}"; do
            d=${RT_CNT_SZ["$ci,$sz,$typ"]:-0}
            if [[ $d -gt 0 ]]; then active=true; break; fi
        done
        $active && break
    done
    $active && ACTIVE_TYPES+=("$typ")
done

print_instrumentation_summary() {
    local sz="$1" title
    if [[ -n "$sz" ]]; then
        title="Instrumentation Summary — input size: $sz"
    elif $MULTI_SIZE; then
        title="Instrumentation Summary — default run (no argv)"
    else
        title="Instrumentation Summary"
    fi
    bold "── $title ──"
    printf '\n'
    if $NO_RUN; then
        printf '  (--no-run: dynamic counts and executable runtime were not measured;\n'
        printf '   dynamic columns show 0, not "measured zero".)\n\n'
    fi

    printf '  %-*s' $LW "configuration"
    for typ in "${ACTIVE_TYPES[@]}"; do printf '  %7s' "s_$(abbr_type "$typ")"; done
    for typ in "${ACTIVE_TYPES[@]}"; do printf '  %7s' "d_$(abbr_type "$typ")"; done
    printf '  %8s  %8s\n' "IR(ms)" "run(ms)"
    printf '  %s\n' "$SEP"

    for (( ci=0; ci<NC; ci++ )); do
        $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]] && print_ompsan_divider
        printf '  %-*s' $LW "[$ci] $(cfg_label $ci)"
        for typ in "${ACTIVE_TYPES[@]}"; do
            printf '  %7d' "$(static_count "$ci" "$typ")"
        done
        for typ in "${ACTIVE_TYPES[@]}"; do
            printf '  %7d' "${RT_CNT_SZ["$ci,$sz,$typ"]:-0}"
        done
        printf '  %8d  %8s\n' "${IR_MS[$ci]}" "${BIN_MS_SZ["$ci,$sz"]:-"-"}"
    done
    printf '\n'
}

if [[ ${#ACTIVE_TYPES[@]} -eq 0 ]]; then
    bold '── Instrumentation Summary ──────────────────────────────────────────────'
    printf '\n  (no __arbalest_* call sites found in any configuration)\n\n'
else
    for sz in "${INPUT_SIZES[@]}"; do
        print_instrumentation_summary "$sz"
    done
fi

bold '════════════════════════════════════════════════════════════════════════'
