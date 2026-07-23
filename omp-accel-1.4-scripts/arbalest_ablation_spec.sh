#!/usr/bin/env bash
# arbalest_ablation_spec.sh
#
# Arbalest ablation study (dedup x hoist x simd, optionally x ompsan) applied
# to the SPEC-ACCEL-style multi-file benchmarks in this directory's ..
# (503.postencil, 504.polbm, 514.pomriq, 552.pep, 554.pcg), instead of the
# single-file .c/.ll input that arbalest_ablation.sh takes.
#
# Unlike arbalest_ablation.sh, this script does NOT go through `runspec` --
# the bundled SPEC Perl toolchain (specperl) segfaults against this host's
# glibc and won't rebuild cleanly either. Instead it replays each benchmark's
# own known-good compile/link recipe (lifted verbatim from its
# build/build_base_compsys.0000/make.out, e.g. -O3 -DSPEC -DSPEC_ACCEL
# -DNDEBUG -g) via the Arbalest clang directly, adding only
# -mllvm -arbalest-dedupe-mode / -arbalest-hoist / -arbalest-ompsan on top.
# Nothing about optimization level, defines, or libs is changed from each
# benchmark's native build.
#
# Configs tested per benchmark (mirrors arbalest_ablation.sh's matrix):
#   [N] native (no Arbalest, no TSan)   -- reference baseline, always first
#   [0] baseline                         off   x false x  -
#   [1] dedup:strip                      strip x false x  -
#   [2] dedup:value                      value x false x  -
#   [3] hoist-only, no SIMD              off   x true  x false
#   [4] hoist-only, with SIMD            off   x true  x true
#   [5] hoist+strip, no SIMD             strip x true  x false
#   [6] hoist+strip, with SIMD           strip x true  x true
#   [7] hoist+value, no SIMD             value x true  x false
#   [8] hoist+value, with SIMD           value x true  x true
#   with --ompsan: [9]..[17] mirror [0]..[8] with -arbalest-ompsan=1
#
# Usage:
#   ./arbalest_ablation_spec.sh [options]
#
# Options:
#   --bench "0 1 2 3 4"     Benchmark indices to run (0=503.postencil,
#                            1=504.polbm, 2=514.pomriq, 3=552.pep, 4=554.pcg).
#                            Default: all five.
#   --ompsan                 Include the OMPSan-guided mirror configs.
#   --input-size test|train|ref   Which SPEC input size to run. Default: test.
#   --repeat N                Runs per config for median timing/memory.
#                              Default: 3.
#   --out-dir DIR              Where to write binaries/logs/CSV. Default:
#                               <script-dir>/arbalest_ablation_spec_out.
#   --only-buildkey KEY        Restrict to the CFGS rows that share this
#                               build (e.g. "strip_true_false" covers both
#                               the no-SIMD and SIMD report rows, since they
#                               share one binary). Implies --ompsan (so
#                               *_ompsan build keys resolve) and skips the
#                               native baseline. Results go to a per-buildkey
#                               CSV (ablation_results_<KEY>.csv) instead of
#                               the shared per-benchmark one, so this is safe
#                               to run as several concurrent jobs per
#                               benchmark -- one per remaining build -- without
#                               two jobs racing to compile into the same
#                               build_<key> dir or the same CSV file. Merge
#                               the pieces afterward with
#                               arbalest_ablation_merge.sh.
#   --no-color

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD=/scratch/mdavis/arbalest-build
CLANG=$BUILD/bin/clang
OMP_INC=$BUILD/projects/openmp/runtime/src

USE_COLOR=true
RUN_OMPSAN=false
INPUT_SIZE=test
REPEAT=3
OUT_DIR="$SCRIPT_DIR/arbalest_ablation_spec_out"
BENCH_SEL="0 1 2 3 4"
ONLY_BUILDKEY=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bench)         BENCH_SEL="$2"; shift 2 ;;
        --bench=*)       BENCH_SEL="${1#*=}"; shift ;;
        --ompsan)        RUN_OMPSAN=true; shift ;;
        --input-size)    INPUT_SIZE="$2"; shift 2 ;;
        --input-size=*)  INPUT_SIZE="${1#*=}"; shift ;;
        --repeat)        REPEAT="$2"; shift 2 ;;
        --repeat=*)       REPEAT="${1#*=}"; shift ;;
        --out-dir)       OUT_DIR="$2"; shift 2 ;;
        --out-dir=*)     OUT_DIR="${1#*=}"; shift ;;
        --only-buildkey)   ONLY_BUILDKEY="$2"; shift 2 ;;
        --only-buildkey=*) ONLY_BUILDKEY="${1#*=}"; shift ;;
        --no-color)      USE_COLOR=false; shift ;;
        -*) printf 'Unknown option: %s\n' "$1" >&2; exit 1 ;;
        *)  printf 'Unexpected argument: %s\n' "$1" >&2; exit 1 ;;
    esac
done

[[ -n "$ONLY_BUILDKEY" ]] && RUN_OMPSAN=true

case "$INPUT_SIZE" in
    test|train|ref) ;;
    *) printf 'ERROR: --input-size must be test, train, or ref (got: %s)\n' "$INPUT_SIZE" >&2; exit 1 ;;
esac
if ! [[ "$REPEAT" =~ ^[0-9]+$ ]] || [[ "$REPEAT" -lt 1 ]]; then
    printf 'ERROR: --repeat must be a positive integer\n' >&2; exit 1
fi

for b in "$CLANG" objdump objcopy python3; do
    command -v "$b" >/dev/null 2>&1 || [[ -x "$b" ]] || { printf 'ERROR: required tool not found: %s\n' "$b" >&2; exit 1; }
done

mkdir -p "$OUT_DIR"

green() { $USE_COLOR && printf '\033[32m%s\033[0m' "$*" || printf '%s' "$*"; }
red()   { $USE_COLOR && printf '\033[31m%s\033[0m' "$*" || printf '%s' "$*"; }
bold()  { $USE_COLOR && printf '\033[1m%s\033[0m\n'  "$*" || printf '%s\n' "$*"; }

ms_now() { printf '%d' "$(( $(date +%s%N) / 1000000 ))"; }
# Real TSan/Arbalest violation reports, not just "the program printed
# something to stderr" (these benchmarks print substantial informational
# output of their own -- grid sizes, iteration counts, etc. -- which is not
# a violation).
is_real_violation() { [[ "$1" == *"WARNING: ThreadSanitizer"* || "$1" == *"SUMMARY: ThreadSanitizer"* ]]; }
median_of() {
    local -a vals=("$@") sorted
    IFS=$'\n' sorted=($(sort -n <<< "${vals[*]}")); unset IFS
    local n=${#sorted[@]}
    local mid=$(( n / 2 ))
    if (( n % 2 == 1 )); then printf '%s' "${sorted[$mid]}"
    else printf '%s' "$(( (sorted[mid-1] + sorted[mid]) / 2 ))"; fi
}

# ── benchmark definitions ──────────────────────────────────────────────────
BENCH_NAME=('503.postencil' '504.polbm' '514.pomriq' '552.pep' '554.pcg')
BENCH_EXE=('stencil_exe' 'polbm' 'omriq_exe' 'pep' 'pcg')
# Space-separated source file lists, relative to the benchmark's src/ dir.
BENCH_SRCS=(
    'main.c file.c kernels.c specrand.c parboil.c'
    'lbm.c main.c'
    'main.c file.c parboil.c'
    'ep.c print_results.c c_timers.c wtime.c'
    'cg.c print_results.c randdp.c c_timers.c wtime.c'
)
# Extra -D flags beyond -DSPEC -DSPEC_ACCEL -DNDEBUG (from each make.out).
BENCH_EXTRA_DEFS=('-I.' '-DSPEC' '-I.' '-DSPEC' '-DSPEC')
BENCH_LIBS=('-lm' '-lm' '-lm' '-lm' '-lm')

# argv for each benchmark, one entry per BENCH_NAME index, for test/train/ref.
TEST_INPUT=('512 512 64 100' '20 reference.dat 0 1 100_100_130_cf_a.of' '-i 32_32_32_dataset.bin -o 514.out' '' '')
TRAIN_INPUT=('512 512 64 100' '300 reference.dat 0 1 100_100_130_cf_b.of' '-i 64_64_64_dataset.bin -o 1028.out' '' '')
REF_INPUT=('512 512 64 100' '20 reference.dat 0 1 100_100_130_cf_a.of' '-i 128_128_128_dataset.bin -o 2056.out' '' '')
case "$INPUT_SIZE" in
    test)  INPUT=("${TEST_INPUT[@]}") ;;
    train) INPUT=("${TRAIN_INPUT[@]}") ;;
    ref)   INPUT=("${REF_INPUT[@]}") ;;
esac

# ── ablation configuration matrix (mirrors arbalest_ablation.sh) ────────────
# slug|label|dedupe|hoist|simd|ompsan|buildkey
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
if $RUN_OMPSAN; then
    for (( i=0; i<BASE_NC; i++ )); do
        IFS='|' read -r s l d h si _ bk <<< "${CFGS[$i]}"
        CFGS+=("${s}_ompsan|${l} +OMPSan|${d}|${h}|${si}|true|${bk}_ompsan")
    done
fi
NC=${#CFGS[@]}

PE_TYPES=(read1 read2 read4 read8 read16 write1 write2 write4 write8 write16)
UA_TYPES=(unaligned_read2 unaligned_read4 unaligned_read8 unaligned_read16
          unaligned_write2 unaligned_write4 unaligned_write8 unaligned_write16)
HT_TYPES=(read_range write_range read_cstride write_cstride read_stride write_stride)
ALL_TYPES=("${PE_TYPES[@]}" "${UA_TYPES[@]}" "${HT_TYPES[@]}")

# ── static call-site counting (host binary + embedded offload device ELF) ──
# Counts "call ... <__arbalest_SUFFIX>" or "<__arbalest_SUFFIX@plt>" sites.
count_calls_in_binary() {
    local bin="$1" suffix="$2"
    objdump -d "$bin" 2>/dev/null | grep -cE "\bcall\b.*<__arbalest_${suffix}(@plt)?>" || true
}

# Extracts any ELF images embedded in .llvm.offloading (OpenMP target device
# code compiled for the host-fallback triple) so hoisted/dedup'd calls inside
# #pragma omp target regions aren't missed by a host-only objdump.
extract_offload_elfs() {
    local bin="$1" out_prefix="$2"
    local sec_bin="${out_prefix}.offload_section"
    objcopy -O binary --only-section=.llvm.offloading "$bin" "$sec_bin" 2>/dev/null
    [[ -s "$sec_bin" ]] || return 0
    python3 - "$sec_bin" "$out_prefix" << 'PYEOF'
import sys
data = open(sys.argv[1], 'rb').read()
prefix = sys.argv[2]
magic = b'\x7fELF'
i = 0
n = 0
while True:
    i = data.find(magic, i)
    if i == -1:
        break
    with open(f"{prefix}.dev{n}.elf", 'wb') as f:
        f.write(data[i:])
    n += 1
    i += 1
PYEOF
    rm -f "$sec_bin"
}

static_count_all_types() {
    # $1=binary  populates STATIC_CNT[$3,type] for the given config index $3
    local bin="$1" tmp_prefix="$2" ci="$3"
    local -a images=("$bin")
    extract_offload_elfs "$bin" "$tmp_prefix"
    for f in "${tmp_prefix}".dev*.elf; do
        [[ -e "$f" ]] && images+=("$f")
    done
    local total=0
    for typ in "${ALL_TYPES[@]}"; do
        local n=0 img c
        for img in "${images[@]}"; do
            c=$(count_calls_in_binary "$img" "$typ")
            n=$(( n + ${c:-0} ))
        done
        STATIC_CNT["$ci,$typ"]=$n
        total=$(( total + n ))
    done
    STATIC_TOTAL[$ci]=$total
    rm -f "${tmp_prefix}".dev*.elf
}

# ── main loop ─────────────────────────────────────────────────────────────
# Each benchmark gets its own CSV under its own out-dir (not a single shared
# top-level CSV) so that concurrent per-benchmark invocations of this script
# (e.g. one SLURM job per benchmark) never race on the same file.

for bi in $BENCH_SEL; do
    bname="${BENCH_NAME[$bi]}"
    bexe="${BENCH_EXE[$bi]}"
    read -r -a bsrcs <<< "${BENCH_SRCS[$bi]}"
    bdefs="${BENCH_EXTRA_DEFS[$bi]}"
    blibs="${BENCH_LIBS[$bi]}"
    bargs="${INPUT[$bi]}"
    srcdir="$ROOT/benchspec/ACCEL/$bname/src"
    bout="$OUT_DIR/$bname"
    mkdir -p "$bout"
    if [[ -n "$ONLY_BUILDKEY" ]]; then
        CSV="$bout/ablation_results_${ONLY_BUILDKEY}.csv"
    else
        CSV="$bout/ablation_results.csv"
    fi
    printf 'benchmark,config_index,slug,label,dedupe,hoist,simd,ompsan,static_total,dynamic_total,build_ms,run_ms,mem_kb,has_violation\n' > "$CSV"

    if [[ ! -d "$srcdir" ]]; then
        printf 'WARNING: source dir not found for %s (%s); skipping\n' "$bname" "$srcdir" >&2
        continue
    fi

    bold "════ $bname (input: $INPUT_SIZE) ════"

    declare -A BUILD_DONE=()      # buildkey -> binary path (this benchmark)
    declare -A BUILD_MS=()        # buildkey -> compile+link ms
    declare -A STATIC_CNT=()      # "ci,type" -> count
    declare -A STATIC_TOTAL=()    # ci -> total
    declare -A RUN_MS=()          # ci -> median run ms
    declare -A MEM_KB=()          # ci -> median RSS kb
    declare -A DYN_TOTAL=()       # ci -> total measured runtime calls
    declare -A DYN_OK=()          # ci -> "ok" or reason
    declare -A VIOLATIONS=()      # ci -> captured non-count stderr

    # Compile one config's sources+link into $bout/$buildkey/$bexe.
    # $1=buildkey $2=dedupe $3=hoist $4=ompsan(true/false) $5=arbalest(true/false)
    compile_config() {
        local buildkey="$1" dedupe="$2" hoist="$3" ompsan="$4" arbalest="$5"
        local cdir="$bout/build_${buildkey}"
        mkdir -p "$cdir"
        local log="$cdir/compile.log"
        : > "$log"

        local -a opt_flags=(-O3 -fopenmp -fopenmp-targets=x86_64-pc-linux-gnu -g)
        if $arbalest; then
            opt_flags+=(-fsanitize=thread -farbalest
                        -mllvm -arbalest-dedupe-mode="$dedupe"
                        -mllvm -arbalest-hoist="$hoist")
            $ompsan && opt_flags+=(-mllvm -arbalest-ompsan=1)
        fi

        local t0 t1 ok=0
        t0=$(ms_now)
        local -a objs=()
        local src obj
        for src in "${bsrcs[@]}"; do
            obj="${src%.c}.o"
            objs+=("$cdir/$obj")
            # shellcheck disable=SC2086
            "$CLANG" -c -o "$cdir/$obj" -DSPEC -DSPEC_ACCEL -DNDEBUG $bdefs \
                -I"$srcdir" -I"$OMP_INC" \
                "${opt_flags[@]}" \
                "$srcdir/$src" >> "$log" 2>&1 || ok=1
        done
        if [[ $ok -eq 0 ]]; then
            # shellcheck disable=SC2086
            "$CLANG" "${opt_flags[@]}" "${objs[@]}" $blibs -o "$cdir/$bexe" >> "$log" 2>&1 || ok=1
        fi
        t1=$(ms_now)
        BUILD_MS["$buildkey"]=$(( t1 - t0 ))
        if [[ $ok -eq 0 && -x "$cdir/$bexe" ]]; then
            BUILD_DONE["$buildkey"]="$cdir/$bexe"
        else
            BUILD_DONE["$buildkey"]=""
            printf '    WARNING: build failed for %s; see %s\n' "$buildkey" "$log" >&2
        fi
    }

    # Run a built binary $REPEAT times, capture median run(ms), median mem(kb),
    # first run's stderr (for violations + ARBALEST_CALL_COUNT parsing).
    run_config() {
        local ci="$1" binpath="$2" arbalest="$3" simd="$4"
        local -a run_env=("LD_LIBRARY_PATH=$BUILD/lib:${LD_LIBRARY_PATH:-}")
        if $arbalest; then
            run_env+=("OMP_TOOL_LIBRARIES=$BUILD/lib/libarcher.so"
                      "TSAN_OPTIONS=ignore_noninstrumented_modules=1"
                      "ARBALEST_COUNT_CALLS=1")
            [[ "$simd" == "false" ]] && run_env+=("ARBALEST_DISABLE_SIMD=1")
        fi

        local -a t_times=() m_samples=()
        local first_out=""
        local rep
        for (( rep=1; rep<=REPEAT; rep++ )); do
            local timef; timef=$(mktemp -p "$bout")
            local out
            out=$(cd "$SCRIPT_DIR" && env "${run_env[@]}" /usr/bin/time -f '%e %M' -o "$timef" "$binpath" $bargs 2>&1)
            local tline; tline=$(cat "$timef" 2>/dev/null)
            rm -f "$timef"
            local secs kb
            secs=$(awk '{print $1}' <<< "$tline")
            kb=$(awk '{print $2}' <<< "$tline")
            [[ -n "$secs" ]] && t_times+=("$(awk -v s="$secs" 'BEGIN{printf "%d", s*1000}')")
            [[ -n "$kb" ]] && m_samples+=("$kb")
            [[ $rep -eq 1 ]] && first_out="$out"
        done
        printf '%s\n' "$first_out" > "$bout/run_${ci}.log"

        RUN_MS[$ci]=$([[ ${#t_times[@]} -gt 0 ]] && median_of "${t_times[@]}" || printf 'ERR')
        MEM_KB[$ci]=$([[ ${#m_samples[@]} -gt 0 ]] && median_of "${m_samples[@]}" || printf '-')

        if $arbalest; then
            local counts_raw; counts_raw=$(grep '^ARBALEST_CALL_COUNT ' <<< "$first_out")
            VIOLATIONS[$ci]="$(grep -v '^ARBALEST_CALL_COUNT ' <<< "$first_out")"
            if [[ -n "$counts_raw" ]]; then
                DYN_OK[$ci]="ok"
                local rt_total=0 _ typ n
                while read -r _ typ n; do
                    [[ -z "$typ" ]] && continue
                    rt_total=$(( rt_total + n ))
                done <<< "$counts_raw"
                DYN_TOTAL[$ci]=$rt_total
            else
                DYN_OK[$ci]="no ARBALEST_CALL_COUNT output"
                DYN_TOTAL[$ci]=0
            fi
        else
            VIOLATIONS[$ci]="$first_out"
            DYN_OK[$ci]="n/a (no arbalest)"
            DYN_TOTAL[$ci]=0
        fi
    }

    # ── native (no Arbalest, no TSan) baseline ──────────────────────────────
    if [[ -z "$ONLY_BUILDKEY" ]]; then
        printf '  [native] no Arbalest / no TSan ... '
        compile_config "native" "off" "false" "false" "false"
        if [[ -n "${BUILD_DONE[native]}" ]]; then
            run_config "native" "${BUILD_DONE[native]}" false "na"
            printf 'build(%dms) run(median of %d: %sms) mem(%skb)\n' \
                "${BUILD_MS[native]}" "$REPEAT" "${RUN_MS[native]}" "${MEM_KB[native]}"
        else
            printf 'BUILD FAILED\n'
        fi
        printf '%s,native,native,native (no arbalest),-,-,-,-,0,0,%s,%s,%s,%s\n' \
            "$bname" "${BUILD_MS[native]:-ERR}" "${RUN_MS[native]:-ERR}" "${MEM_KB[native]:-'-'}" \
            "$(is_real_violation "${VIOLATIONS[native]:-}" && echo yes || echo no)" >> "$CSV"
    fi

    # ── ablation matrix ─────────────────────────────────────────────────────
    for (( ci=0; ci<NC; ci++ )); do
        IFS='|' read -r slug label dedupe hoist simd ompsan buildkey <<< "${CFGS[$ci]}"

        if [[ -n "$ONLY_BUILDKEY" && "$buildkey" != "$ONLY_BUILDKEY" ]]; then
            continue
        fi

        printf '  [%d] %-28s ... ' "$ci" "$label"

        if [[ -z "${BUILD_DONE[$buildkey]+x}" ]]; then
            compile_config "$buildkey" "$dedupe" "$hoist" "$ompsan" true
        fi
        local_bin="${BUILD_DONE[$buildkey]}"
        if [[ -z "$local_bin" ]]; then
            printf 'BUILD FAILED\n'
            printf '%s,%d,%s,%s,%s,%s,%s,%s,0,0,ERR,ERR,-,no\n' \
                "$bname" "$ci" "$slug" "$label" "$dedupe" "$hoist" "$simd" "$ompsan" >> "$CSV"
            continue
        fi

        static_count_all_types "$local_bin" "$bout/static_${buildkey}" "$ci"
        run_config "$ci" "$local_bin" true "$simd"

        viol_flag="no"
        is_real_violation "${VIOLATIONS[$ci]}" && viol_flag="yes"

        printf 'static(%d) build(%dms) run(median of %d: %sms) mem(%skb) dyn(%s)\n' \
            "${STATIC_TOTAL[$ci]}" "${BUILD_MS[$buildkey]}" "$REPEAT" "${RUN_MS[$ci]}" "${MEM_KB[$ci]}" "${DYN_TOTAL[$ci]:-0}"

        printf '%s,%d,%s,%s,%s,%s,%s,%s,%d,%d,%d,%s,%s,%s\n' \
            "$bname" "$ci" "$slug" "$label" "$dedupe" "$hoist" "$simd" "$ompsan" \
            "${STATIC_TOTAL[$ci]}" "${DYN_TOTAL[$ci]:-0}" "${BUILD_MS[$buildkey]}" \
            "${RUN_MS[$ci]}" "${MEM_KB[$ci]}" "$viol_flag" >> "$CSV"
    done

    # ── per-benchmark summary table ─────────────────────────────────────────
    printf '\n'
    bold "── Summary: $bname ──"
    LW=30
    printf '  %-*s  %10s  %10s  %8s  %8s  %8s\n' $LW "configuration" "static" "dynamic" "run(ms)" "mem(kb)" "violation"
    printf '  %s\n' "$(printf -- '-%.0s' {1..80})"
    printf '  %-*s  %10s  %10s  %8s  %8s  %8s\n' $LW "native (no arbalest)" "-" "-" "${RUN_MS[native]:-ERR}" "${MEM_KB[native]:-'-'}" "-"
    for (( ci=0; ci<NC; ci++ )); do
        $RUN_OMPSAN && [[ $ci -eq $BASE_NC ]] && printf '  %s\n' "── with --ompsan ──"
        IFS='|' read -r slug label _ _ _ _ _ <<< "${CFGS[$ci]}"
        vflag="no"
        is_real_violation "${VIOLATIONS[$ci]:-}" && vflag="yes"
        printf '  [%d] %-*s  %10s  %10s  %8s  %8s  %8s\n' "$ci" $(( LW - 4 )) "$label" \
            "${STATIC_TOTAL[$ci]:-ERR}" "${DYN_TOTAL[$ci]:-ERR}" "${RUN_MS[$ci]:-ERR}" "${MEM_KB[$ci]:-'-'}" "$vflag"
    done
    printf '\n'

    unset -f compile_config run_config
    unset BUILD_DONE BUILD_MS STATIC_CNT STATIC_TOTAL RUN_MS MEM_KB DYN_TOTAL DYN_OK VIOLATIONS
done

bold "Done. Per-benchmark CSVs under: $OUT_DIR/<benchmark>/ablation_results.csv"
