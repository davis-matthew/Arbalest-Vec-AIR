#!/usr/bin/env python3
"""arbalest_ablation_latex.py

Turns the per-benchmark ablation_results.csv files produced by
arbalest_ablation_spec.sh into LaTeX tables:

  1. summary_table.tex   -- one compact table, one row per config, averaged
     (geomean for ratios, arithmetic mean for percentages) across all
     benchmarks found. Meant for the paper's main body.
  2. appendix_tables.tex -- one full table per benchmark, every config as a
     row with raw static/dynamic call counts, runtime, memory, and whether a
     TSan violation was reported. Meant for an appendix.

Both are standalone `table` environments (with booktabs rules) meant to be
\\input{} into the paper; they are not full documents. Requires
\\usepackage{booktabs} in the preamble.

Usage:
    ./arbalest_ablation_latex.py [--out-dir DIR] [--results-dir DIR] [bench ...]

    --results-dir DIR   Where the per-benchmark ablation_results.csv files
                         live (default: <script-dir>/arbalest_ablation_spec_out)
    --out-dir DIR       Where to write the .tex files (default: --results-dir)
    bench                Benchmark dir names to include (default: all found)
"""
import csv
import math
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# (config_index as it appears in the CSV, display label). Fixed order so the
# table reads the same regardless of CSV row order. Must match the CFGS
# matrix in arbalest_ablation_spec.sh.
BASE_ROWS = [
    ("native", "native (no Arbalest)"),
    ("0", "baseline"),
    ("1", "dedup:strip"),
    ("2", "dedup:value"),
    ("3", "hoist (no SIMD)"),
    ("4", "hoist (SIMD)"),
    ("5", "hoist+strip (no SIMD)"),
    ("6", "hoist+strip (SIMD)"),
    ("7", "hoist+value (no SIMD)"),
    ("8", "hoist+value (SIMD)"),
]
OMPSAN_ROWS = [
    ("9", "baseline"),
    ("10", "dedup:strip"),
    ("11", "dedup:value"),
    ("12", "hoist (no SIMD)"),
    ("13", "hoist (SIMD)"),
    ("14", "hoist+strip (no SIMD)"),
    ("15", "hoist+strip (SIMD)"),
    ("16", "hoist+value (no SIMD)"),
    ("17", "hoist+value (SIMD)"),
]
ALL_ROWS = BASE_ROWS + OMPSAN_ROWS
BASELINE_CI = "0"


def load_bench(results_dir, bench):
    path = os.path.join(results_dir, bench, "ablation_results.csv")
    rows = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows[r["config_index"]] = r
    return rows


def ompsan_flagged(row):
    """True if OMPSan's data-mapping analysis actually flagged >=1 function
    to instrument for this config. Some benchmarks (pomriq, pep, pcg here)
    have OMPSan flag nothing at all, so every access is skipped and the
    dynamic call count trivially drops to 0 -- a "100% removed" that
    reflects OMPSan finding zero candidates, not genuinely narrowing down a
    nonempty set. Only benchmarks where it flagged something show whether
    OMPSan's selection was actually useful."""
    st = to_float(row, "static_total")
    return st is not None and st > 0


def crashed(row):
    """True if Arbalest detected a real violation and the process then died
    before finishing (run_ms/mem_kb never got recorded) -- e.g. 554.pcg's
    hoisted configs hit a genuine stale-access race and SEGV. Distinct from
    a normal completed run: there is no timing/memory data to report, and
    run_ms==0 must not be treated as "instant" in speedup calculations."""
    return row.get("has_violation") == "yes" and to_float(row, "run_ms") in (None, 0.0)


def to_float(row, key):
    try:
        v = row[key]
        if v in ("", "ERR", "-"):
            return None
        return float(v)
    except (KeyError, ValueError, TypeError):
        return None


def geomean(vals):
    vals = [v for v in vals if v is not None and v > 0]
    if not vals:
        return None
    return math.exp(sum(math.log(v) for v in vals) / len(vals))


def mean(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return None
    return sum(vals) / len(vals)


def esc(s):
    return str(s).replace("_", r"\_").replace("%", r"\%").replace("&", r"\&")


def fmt_x(v):
    return f"{v:.1f}$\\times$" if v is not None else "---"


def fmt_pct(v):
    if v is None:
        return "---"
    # 1 decimal place: several configs round to "100%" at 0dp despite not
    # being an exact, degenerate 100% (see ompsan_flagged) -- keep that
    # distinction visible rather than rounding it away.
    return f"{v:.1f}\\%"


# ── summary tables ───────────────────────────────────────────────────────
def build_summary_dynamic(all_bench_rows, bench_names):
    """Base dedup/hoist configs: speedup + dynamic call-count reduction,
    averaged (geomean/arithmetic mean) across all benchmarks."""
    lines = []
    lines.append(r"\begin{table}[t]")
    lines.append(r"\centering")
    lines.append(
        r"\caption{Arbalest ablation summary (dedup/hoist), averaged across "
        + str(len(bench_names))
        + r" SPEC-ACCEL benchmarks ("
        + ", ".join(esc(b) for b in bench_names)
        + r"). Speedup and memory overhead are geometric means of "
        r"per-benchmark ratios; dynamic checks removed is the arithmetic "
        r"mean of the per-benchmark percentage reduction in dynamic call "
        r"count relative to the \emph{baseline} configuration.}"
    )
    lines.append(r"\label{tab:arbalest-ablation-summary-dynamic}")
    lines.append(r"\begin{tabular}{lrrr}")
    lines.append(r"\toprule")
    lines.append(
        r"Configuration & Speedup vs.\ baseline & Dynamic checks removed & Mem.\ overhead vs.\ native \\"
    )
    lines.append(r"\midrule")

    for ci, label in BASE_ROWS:
        speedups, removed_pcts, mem_overheads = [], [], []
        for rows in all_bench_rows:
            r = rows.get(ci)
            base = rows.get(BASELINE_CI)
            nat = rows.get("native")
            if r is None or crashed(r):
                continue
            run_ms = to_float(r, "run_ms")
            base_ms = to_float(base, "run_ms") if base else None
            if run_ms and base_ms:
                speedups.append(base_ms / run_ms)
            if ci != "native":
                dyn = to_float(r, "dynamic_total")
                base_dyn = to_float(base, "dynamic_total") if base else None
                if dyn is not None and base_dyn:
                    removed_pcts.append((base_dyn - dyn) / base_dyn * 100)
            mem = to_float(r, "mem_kb")
            nat_mem = to_float(nat, "mem_kb") if nat else None
            if mem and nat_mem:
                mem_overheads.append(mem / nat_mem)

        sp = geomean(speedups)
        rp = mean(removed_pcts) if ci != "native" else None
        mo = geomean(mem_overheads)
        lines.append(f"{esc(label)} & {fmt_x(sp)} & {fmt_pct(rp)} & {fmt_x(mo)} \\\\")

    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"\end{table}")
    return "\n".join(lines)


def build_summary_static(all_bench_rows, bench_names):
    """OMPSan-guided configs: speedup + static call-site reduction, averaged
    only over benchmarks where OMPSan actually flagged something (skips the
    ones where it flagged nothing -- a degenerate, meaningless 100%)."""
    ompsan_active_benches = [
        b for b, rows in zip(bench_names, all_bench_rows)
        if "9" in rows and ompsan_flagged(rows["9"])
    ]
    skipped_benches = [b for b in bench_names if b not in ompsan_active_benches]

    lines = []
    lines.append(r"\begin{table}[t]")
    lines.append(r"\centering")
    lines.append(
        r"\caption{Arbalest ablation summary (OMPSan-guided), averaged over "
        + str(len(ompsan_active_benches))
        + r" benchmark(s) where OMPSan actually flagged something to "
        r"instrument (" + ", ".join(esc(b) for b in ompsan_active_benches) + r"). "
        + (
            r"Excludes " + ", ".join(esc(b) for b in skipped_benches)
            + r", where OMPSan flagged zero functions -- a degenerate 100\% "
              r"reduction, not a meaningful selection. "
            if skipped_benches else ""
        )
        + r"Speedup and memory overhead are geometric means of per-benchmark "
        r"ratios; static checks removed is the arithmetic mean of the "
        r"per-benchmark percentage reduction in instrumented call \emph{sites} "
        r"(a compile-time count, unlike the dynamic call-count reduction used "
        r"for the dedup/hoist table) relative to the \emph{baseline} "
        r"configuration.}"
    )
    lines.append(r"\label{tab:arbalest-ablation-summary-static}")
    lines.append(r"\begin{tabular}{lrrr}")
    lines.append(r"\toprule")
    lines.append(
        r"Configuration & Speedup vs.\ baseline & Static checks removed & Mem.\ overhead vs.\ native \\"
    )
    lines.append(r"\midrule")

    for ci, label in OMPSAN_ROWS:
        speedups, removed_pcts, mem_overheads = [], [], []
        for rows in all_bench_rows:
            r = rows.get(ci)
            base = rows.get(BASELINE_CI)
            nat = rows.get("native")
            if r is None or crashed(r) or not ompsan_flagged(r):
                continue
            run_ms = to_float(r, "run_ms")
            base_ms = to_float(base, "run_ms") if base else None
            if run_ms and base_ms:
                speedups.append(base_ms / run_ms)
            static = to_float(r, "static_total")
            base_static = to_float(base, "static_total") if base else None
            if static is not None and base_static:
                removed_pcts.append((base_static - static) / base_static * 100)
            mem = to_float(r, "mem_kb")
            nat_mem = to_float(nat, "mem_kb") if nat else None
            if mem and nat_mem:
                mem_overheads.append(mem / nat_mem)

        sp = geomean(speedups)
        rp = mean(removed_pcts)
        mo = geomean(mem_overheads)
        lines.append(f"{esc(label)} +OMPSan & {fmt_x(sp)} & {fmt_pct(rp)} & {fmt_x(mo)} \\\\")

    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"\end{table}")
    return "\n".join(lines)


# ── per-benchmark appendix tables ────────────────────────────────────────
def fmt_dyn(v):
    if v is None:
        return "---"
    if v >= 1e6:
        return f"{v:.2e}"
    return f"{int(v):,}"


def build_bench_table(bench, rows):
    base = rows.get(BASELINE_CI)
    base_ms = to_float(base, "run_ms") if base else None

    lines = []
    lines.append(r"\begin{table}[t]")
    lines.append(r"\centering")
    lines.append(r"\caption{Arbalest ablation, \texttt{" + esc(bench) + r"} (SPEC-ACCEL, test input).}")
    lines.append(r"\label{tab:arbalest-ablation-" + bench.replace(".", "-") + "}")
    lines.append(r"\begin{tabular}{lrrrrr}")
    lines.append(r"\toprule")
    lines.append(
        r"Configuration & Static calls & Dynamic calls & Run (s) & Speedup & Mem.\ (MB) \\"
    )
    lines.append(r"\midrule")

    def emit_row(ci, label, ompsan_suffix=""):
        r = rows.get(ci)
        if r is None:
            lines.append(f"{esc(label)}{ompsan_suffix} & \\multicolumn{{5}}{{c}}{{(missing)}} \\\\")
            return
        static = to_float(r, "static_total")
        dyn = to_float(r, "dynamic_total")
        run_ms = to_float(r, "run_ms")
        mem_kb = to_float(r, "mem_kb")

        static_s = "---" if ci == "native" else (f"{int(static):,}" if static is not None else "ERR")

        if crashed(r):
            # Arbalest caught a real violation and the process then died
            # (SEGV) before finishing -- no timing/memory/call-count data
            # was ever recorded, so 0 here would be misleading, not exact.
            dyn_s, run_s, speedup, mem_mb = "crashed", "crashed", "crashed", "crashed"
        else:
            run_s = f"{run_ms/1000:.2f}" if run_ms is not None else "---"
            speedup = fmt_x(base_ms / run_ms) if (run_ms and base_ms) else "---"
            mem_mb = f"{mem_kb/1024:.0f}" if mem_kb is not None else "---"
            dyn_s = "---" if ci == "native" else fmt_dyn(dyn)

        lines.append(
            f"{esc(label)}{ompsan_suffix} & {static_s} & {dyn_s} & {run_s} & {speedup} & {mem_mb} \\\\"
        )

    for ci, label in BASE_ROWS:
        emit_row(ci, label)

    # Skip the OMPSan-guided block entirely if OMPSan flagged nothing for
    # this benchmark -- every row would just show a degenerate 100% removed
    # (nothing was ever instrumented), not a real result worth a row.
    if ompsan_flagged(rows.get("9")):
        lines.append(r"\midrule")
        lines.append(r"\multicolumn{6}{l}{\textit{OMPSan-guided}} \\")
        lines.append(r"\midrule")
        for ci, label in OMPSAN_ROWS:
            emit_row(ci, label, ompsan_suffix=" +OMPSan")

    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"\end{table}")
    return "\n".join(lines)


def main():
    args = sys.argv[1:]
    results_dir = os.path.join(SCRIPT_DIR, "arbalest_ablation_spec_out")
    out_dir = None
    benches = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--results-dir":
            results_dir = args[i + 1]
            i += 2
        elif a.startswith("--results-dir="):
            results_dir = a.split("=", 1)[1]
            i += 1
        elif a == "--out-dir":
            out_dir = args[i + 1]
            i += 2
        elif a.startswith("--out-dir="):
            out_dir = a.split("=", 1)[1]
            i += 1
        else:
            benches.append(a)
            i += 1
    if out_dir is None:
        out_dir = results_dir

    if not benches:
        benches = sorted(
            d for d in os.listdir(results_dir)
            if os.path.isfile(os.path.join(results_dir, d, "ablation_results.csv"))
        )
    if not benches:
        print(f"ERROR: no ablation_results.csv found under {results_dir}", file=sys.stderr)
        sys.exit(1)

    os.makedirs(out_dir, exist_ok=True)

    all_bench_rows = []
    appendix_chunks = []
    for b in benches:
        rows = load_bench(results_dir, b)
        all_bench_rows.append(rows)
        appendix_chunks.append(build_bench_table(b, rows))

    summary_dynamic_tex = build_summary_dynamic(all_bench_rows, benches)
    summary_static_tex = build_summary_static(all_bench_rows, benches)
    appendix_tex = "\n\n".join(appendix_chunks)

    summary_dynamic_path = os.path.join(out_dir, "summary_table_dynamic.tex")
    summary_static_path = os.path.join(out_dir, "summary_table_static.tex")
    appendix_path = os.path.join(out_dir, "appendix_tables.tex")
    with open(summary_dynamic_path, "w") as f:
        f.write(summary_dynamic_tex + "\n")
    with open(summary_static_path, "w") as f:
        f.write(summary_static_tex + "\n")
    with open(appendix_path, "w") as f:
        f.write(appendix_tex + "\n")

    print(f"Wrote {summary_dynamic_path}")
    print(f"Wrote {summary_static_path}")
    print(f"Wrote {appendix_path} ({len(benches)} benchmark tables)")
    print("Requires \\usepackage{booktabs} in your LaTeX preamble.")


if __name__ == "__main__":
    main()
