// ompsan_selective.c
//
// Demonstrates OMPSan-guided selective Arbalest instrumentation.
//
// Two functions are defined:
//
//   bad_mapping()  — Has a real OpenMP offload data-mapping bug.
//                    arr[] is declared map(to:), meaning the runtime copies it
//                    to the device before the kernel but does NOT copy it back
//                    afterwards.  The kernel writes arr[], so the host-side
//                    read of arr[] after the target region observes stale data.
//                    OMPSan detects the use-def mismatch (def on device, use
//                    on host, no 'from'/'tofrom' clause) and adds bad_mapping
//                    (and its outlined kernel) to Arbalest's include list.
//
//   pure_host()    — A host-only L2-norm reduction with no OpenMP target
//                    constructs whatsoever.  OMPSan has no data-mapping events
//                    to flag here.  With -arbalest-ompsan, Arbalest skips
//                    read/write race checks for pure_host (the only kind of
//                    instrumentation OMPSan's report has a basis to vouch
//                    for).  Since pure_host has no outlined kernel / GEP
//                    bound-check surface either, that means it drops to zero
//                    arbalest call sites in this example -- but that is a
//                    consequence of its shape, not a guarantee that excluded
//                    functions always get zero instrumentation.
//
// The contrast between the two functions is the key observable:
//   Without --ompsan  →  both functions are instrumented (normal baseline)
//   With    --ompsan  →  only bad_mapping (+ its kernel) gets read/write
//                        checks; pure_host drops to 0 arbalest call sites.
//                        Excluded OpenMP-outlined kernels would still get
//                        GEP bound checks even though their read/write
//                        checks are skipped.
//
// ── How to run ────────────────────────────────────────────────────────────────
//
//   # Ablation comparison (IR only, no binary needed):
//   ./arbalest_ablation.sh --ompsan --no-run \
//       --cflags "-I$BUILD/projects/openmp/runtime/src" \
//       ompsan_selective.c
//
//   # Check what OMPSan reports directly (reads OMPSanReport.txt):
//   clang -target x86_64-unknown-linux-gnu \
//         -fsanitize=thread -fopenmp \
//         -fopenmp-targets=x86_64-pc-linux-gnu \
//         -farbalest -O1 \
//         -I$BUILD/projects/openmp/runtime/src \
//         -mllvm -arbalest=1 \
//         -mllvm -arbalest-ompsan=1 \
//         -S -emit-llvm ompsan_selective.c -o ompsan_selective_guided.ll
//   # Then diff arbalest call counts:
//   grep -c '__arbalest_' ompsan_selective_guided.ll || true
//
// ── Expected OMPSan report (~/OMPSanReport.txt) ──────────────────────────────
//
//   Error: definition of 'arr' on device does not reach host use of 'arr'.
//   Possible fix: add map clause 'from:arr' or change 'to:arr' → 'tofrom:arr'
//
// ── Expected instrumentation summary (arbalest_ablation.sh --ompsan) ─────────
//
//   Without OMPSan:  bad_mapping → N calls,  pure_host → M calls  (M > 0)
//   With    OMPSan:  bad_mapping → N calls,  pure_host → 0 calls
//   Saved: M / (N+M) * 100 %

#include <stdio.h>
#include <stdlib.h>

// ─────────────────────────────────────────────────────────────────────────────
// bad_mapping: the OpenMP data-mapping bug
//
// The kernel doubles every element of arr[] on the device.
// Because the map clause is 'to:' only, the runtime:
//   1. copies arr[] host → device  (✓)
//   2. runs the kernel              (✓, device writes arr[])
//   3. does NOT copy arr[] back    (✗ — 'from:' is absent)
//
// The subsequent host loop reads arr[] and accumulates a sum.
// The sum is computed from the original (pre-kernel) host values, not the
// doubled values.  At runtime this is a silent wrong-answer bug.
// OMPSan catches it statically and flags this function for Arbalest.
// ─────────────────────────────────────────────────────────────────────────────
void bad_mapping(double *arr, int n)
{
    // BUG: map(to:) copies arr host→device but never back (no 'from:').
    // Using '#pragma omp target data' makes this explicit in the IR:
    // the compiler emits __tgt_target_data_begin_mapper (enter) and
    // __tgt_target_data_end_mapper (exit) calls that OMPSan can inspect.
    // With 'tofrom:' those calls would carry the FROM bit; with 'to:' only
    // they don't, so device writes are invisible to the host.
    #pragma omp target data map(to: arr[0:n])   /* BUG: should be tofrom: */
    {
        #pragma omp target map(to: arr[0:n])
        for (int i = 0; i < n; i++)
            arr[i] *= 2.0;      // device store — def on device
    }

    // Host read of arr[] — use on host.  No FROM mapping bridges the gap.
    double sum = 0.0;
    for (int i = 0; i < n; i++)
        sum += arr[i];      // host load — use-def mismatch detected by OMPSan

    // Prints stale (pre-doubling) sum rather than 2× the expected value.
    printf("bad_mapping: sum = %f  (expected %f)\n",
           sum, sum * 2.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// pure_host: host-only L2-norm reduction — no OMP target, no data mapping
//
// Every memory access here is a plain host load.  OMPSan's reaching-definition
// analysis only tracks variables that cross host/device boundaries via OpenMP
// RTL calls; since none appear here, OMPSan emits no bug report for this
// function and Arbalest (in --ompsan mode) leaves it completely uninstrumented.
// ─────────────────────────────────────────────────────────────────────────────
double pure_host(const double *arr, int n)
{
    double norm_sq = 0.0;
    for (int i = 0; i < n; i++)
        norm_sq += arr[i] * arr[i];   // host load — no mapping events
    return norm_sq;
}

int main(void)
{
    const int n = 1024;
    double *a = (double *)malloc((size_t)n * sizeof(double));
    if (!a) { perror("malloc"); return 1; }

    for (int i = 0; i < n; i++)
        a[i] = (double)(i + 1);

    bad_mapping(a, n);

    double ns = pure_host(a, n);
    printf("pure_host:   norm_sq = %f\n", ns);

    free(a);
    return 0;
}
