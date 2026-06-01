# Arbalest-Vec: SIMD-Accelerated Dynamic Data Inconsistency Detector for OpenMP programs

This directory and its sub-directories contain the source code for a customized LLVM 15.
The data inconsistency detector is implemented as a standalone LLVM instrumentation pass (`Arbalest`) that runs alongside ThreadSanitizer, plus runtime helpers added to TSan's runtime library (compiler-rt/lib/tsan).
In addition, we also implemented all OpenMP Tool interface (OMPT) callbacks for OpenMP device offloading according to the 5.2 version specification.

Note: this prototype only supports the x86-64 architecture

## Architecture

Arbalest's instrumentation lives in its own pass — separate from ThreadSanitizer — at:
- `llvm/include/llvm/Transforms/Instrumentation/Arbalest.h`
- `llvm/lib/Transforms/Instrumentation/Arbalest.cpp`

The pass is scheduled immediately after the TSan passes in the clang pipeline. It still depends on TSan being enabled because the `__arbalest_*` runtime callbacks share state with the TSan runtime (`compiler-rt/lib/tsan/rtl/tsan_arbalest_*`).

## How to install Arbalest-Vec

We have provided a bash script to help you install Arbalest-Vec.  

```c
install_arbalest.sh [BUILD_DIR] [INSTALL_DIR]

// [BUILD_DIR]: the directory where cmake & ninja compile the source code
// [INSTALL_DIR]: the directory where the customized LLVM will be installed
```

## How to use Arbalest-Vec
We will use the following example (example.cpp) to show how to use Arbalest-Vec  
```c
     1	#include <cstdio>
     2	#define N 1000
     3
     4	int main() {
     5	    int a[N];
     6	    #pragma omp target teams distribute map(from: a[0:N]) // map-type should be "tofrom"
     7	    for (int i = 0; i < N; i++) {
     8	        a[i] += i;                                 // read uninitialized value from a[i]
     9	    }
    10
    11	    printf("a[%d] = %d\n", 3, a[3]);
    12	    return 0;
    13	}

```

### Compile the OpenMP program with OpenMP and ThreadSanitizer enabled
```c
   clang++ -fopenmp -fopenmp-targets=x86_64-pc-linux-gnu -farbalest -fsanitize=thread -g -o example.exe example.cpp
```

The user-facing driver flag is still `-farbalest` (disable with `-fno-arbalest`). It requires both `-fopenmp` and `-fsanitize=thread`.

Under the hood, `-farbalest` now forwards different `-mllvm` flags than earlier versions of this prototype:

| Old (Arbalest fused into TSan) | New (standalone pass) |
|---|---|
| `-mllvm -tsan-arbalest=1` | `-mllvm -arbalest=1` |
| `-mllvm -tsan-debug-info=1` | `-mllvm -arbalest-debug-info=1` |

The `-arbalest-debug-info=1` flag is auto-added by the driver whenever `-g` is passed.

### Access dedup (default on)

By default the Arbalest pass elides redundant runtime callbacks. Under OpenMP's
serial-elision execution model, the detector only needs to observe the *first*
read of an address and the *first* write that follows it (or the *first* write,
if it precedes any read) within each call-bounded region. The pass tracks
per-address state during a single function walk, resets state at every
call/invoke (which may launch a kernel), and skips emitting `__arbalest_read*`
/ `__arbalest_write*` for accesses whose verdict cannot change.

The aggressiveness of "same address" detection is controlled by
`-mllvm -arbalest-dedupe-mode=<mode>`:

| Mode | What it dedupes on | Cost | When to use |
|---|---|---|---|
| `off` | nothing — instrument every eligible access | none | baseline / debugging the runtime |
| `value` | raw pointer SSA value (TSan-style) | free | conservative, matches TSan's own dedup |
| `strip` *(default)* | pointer after `stripPointerCasts()` | O(1) per access | catches trivial bitcast chains; relies on prior GVN/EarlyCSE for most equivalent-address cases |
| `aa` | AliasAnalysis `MustAlias` queries on `MemoryLocation` | O(N²·AA) per function | most precise; catches semantically equal GEPs that GVN didn't merge |

Examples:

```c
   # Disable dedup (baseline overhead measurement):
   clang++ ... -farbalest -fsanitize=thread -mllvm -arbalest-dedupe-mode=off ...

   # Use AA-based MustAlias dedup (tighter but slower at compile time):
   clang++ ... -farbalest -fsanitize=thread -mllvm -arbalest-dedupe-mode=aa ...
```

You can see how many accesses were elided vs. instrumented by adding
`-mllvm -stats` to the clang invocation; look for
`NumInstrumentedArbalestAccesses` and `NumElidedArbalestAccesses`.

Note that GEP bounds checks (`__arbalest_check_bound`) are not affected by this
dedup — they need to fire on every dynamic execution because the index varies
across loop iterations.

If you are driving `opt` directly instead of `clang`, the new pass names registered in `PassRegistry.def` are:
- Module pass: `arbalest-module`
- Function pass: `arbalest`

For example:
```c
   opt -passes='tsan-module,function(tsan),arbalest-module,function(arbalest)' -arbalest=1 input.ll -o output.bc
```

### Selective instrumentation: function include lists

By default Arbalest instruments every eligible function in the module. Two
mechanisms allow you to restrict instrumentation to a subset of functions,
which reduces runtime overhead and focuses the detector's budget on the code
most likely to contain data-inconsistency bugs.

#### Explicit include list (`-arbalest-only-functions`)

Pass a comma-separated list of function names. Only those functions receive
`__arbalest_*` callbacks; every other function is skipped entirely.

```bash
clang++ ... -farbalest -fsanitize=thread \
    -mllvm -arbalest-only-functions=bad_kernel,helper_fn \
    example.cpp
```

This is the low-level primitive. The OMPSan integration below generates this
list automatically.

#### OMPSan-guided selective instrumentation (`-arbalest-ompsan`)

[OMPSan](llvm/lib/Transforms/Instrumentation/OMPSan/) is a static analysis
that inspects OpenMP runtime calls in the host IR and identifies functions
involved in potential data-mapping inconsistencies — variables sent to the
device with `map(to:)` but never returned with `map(from:)` or `map(tofrom:)`.

When `-arbalest-ompsan` is enabled:

1. **OMPSan analysis** runs as a module-level prerequisite of `arbalest-module`.
   It walks `__tgt_target_data_begin_mapper` / `__tgt_target_data_end_mapper`
   calls, extracts the map-type bits for each argument, and flags any variable
   that is copied host → device (`to:`) but never copied back (`from:`).

2. **Include list** is built from the set of functions that contain those
   suspect mapping calls.  Currently this is a conservative over-approximation
   (true + false positives); the Arbalest runtime then acts as the arbiter at
   execution time.

3. **Arbalest** instruments only the flagged functions, leaving pure host-only
   code uninstrumented.

```bash
# Compile with OMPSan-guided instrumentation:
clang++ -fopenmp -fopenmp-targets=x86_64-pc-linux-gnu \
        -farbalest -fsanitize=thread \
        -mllvm -arbalest-ompsan=1 \
        example.cpp -o example.exe
```

Both flags can be combined — OMPSan's list and the explicit list are unioned,
so any function in either list is instrumented:

```bash
   -mllvm -arbalest-ompsan=1 -mllvm -arbalest-only-functions=extra_fn
```

#### OMPSan output files

Every compilation with `-arbalest-ompsan=1` writes two files to `$HOME`:

| File | Contents |
|---|---|
| `~/OMPSanReport.txt` | One line per detected bug with a human-readable description of the missing map clause |
| `~/OMPSanAllowlist.txt` | Sanitizer allowlist format (`fun:<name>`) — one entry per function Arbalest was asked to instrument |

#### Ablation study with OMPSan

The `arbalest_ablation.sh` script accepts `--ompsan` to re-run every
dedup × hoist configuration with OMPSan enabled and report the reduction in
instrumented call sites:

```bash
./arbalest_ablation.sh --ompsan --no-run \
    --cflags "-I$BUILD/projects/openmp/runtime/src" \
    my_program.c
```

The extra **OMPSan Impact** table in the report shows, for each configuration:

| Column | Meaning |
|---|---|
| `before` | total `__arbalest_*` call sites without OMPSan filtering |
| `after`  | call sites retained after OMPSan restricts the include list |
| `removed` | call sites eliminated |
| `saved%` | reduction percentage (green = fewer calls, red = more) |

#### Running OMPSan standalone via `opt`

The four OMPSan analysis passes and their printer counterparts are all
registered with the LLVM pass manager:

| Pass name | Type | Description |
|---|---|---|
| `mem-use-def` | Module analysis | Interprocedural memory use-def chain analysis |
| `omp-diagnostics` | Module analysis | OpenMP RTL call parser; builds host↔device copy maps |
| `omp-sanitizer` | Module analysis | Cross-boundary bug detector; produces the include list |
| `omp-optimem` | Module analysis | Memory-mapping optimisation analysis (experimental) |
| `omp-mem-def-print` | Module pass | Printer for `mem-use-def` results |
| `omp-diagnostics-print` | Module pass | Printer for `omp-diagnostics` results |
| `omp-sanitizer-print` | Module pass | Printer for `omp-sanitizer` results |

The analyses run lazily — requesting `omp-sanitizer` automatically triggers
`omp-diagnostics` and `mem-use-def`. Arbalest does this internally when
`-arbalest-ompsan=1` is set, so you do not need to list them explicitly in a
`-passes=` string.

#### Implementation notes and known limitations

**LLVM ≥14 device-code visibility.** In LLVM ≥14 the device kernel is
compiled into a separate object and is not present in the host IR that OMPSan
analyses. The original OMPSan reaching-definition analysis relied on seeing
`__omp_offloading_*` function definitions in the host module (as was the case
in LLVM ≤13). For LLVM 15 a **map-type fallback** was added: rather than
tracing def/use chains across the host/device boundary, it inspects the
`map(to:)` / `map(from:)` flags on every `__tgt_target_data_begin_mapper`
call and flags functions where a variable leaves for the device but is never
returned. This catches the same class of bugs with no false negatives, at the
cost of potential false positives (variables intentionally mapped `to:`
without needing a readback).

**Conservative include list.** OMPSan reports both true and false positives.
Arbalest instruments all flagged functions; the runtime's happens-before
tracking then determines which accesses are actual inconsistencies. False
positives add instrumentation overhead but do not produce spurious warnings.

**Mapper API alignment.** The pass recognises both the legacy
`__tgt_target_data_begin` API (LLVM ≤13) and the mapper variant
`__tgt_target_data_begin_mapper` (LLVM ≥14). The argument-position table
in `OmpDiagnosticsAnalysis.h` (`TargetRTLMap`) encodes the positional
differences between the two calling conventions.

### Execute the OpenMP program
```c
   export TSAN_OPTIONS='ignore_noninstrumented_modules=1' // this option is needed to avoid false positives
   ./example.exe
```

### Arbalest-Vec's Output

Note: The line numbers in the Arbalest-Vec report may experience slight discrepancies, either shifting up or down. The underlying issue lies in ThreadSanitizer's inability to accurately retrieve every line number when OpenMP is enabled. We are actively working on resolving this issue to ensure accurate line numbers in the report.

```c
*****************************
Arbalest successfully starts
*****************************

==================
WARNING: ThreadSanitizer: data inconsistency (uninitialized access) (pid=5075) on the target
  Read of size 4 at 0x7b8000002000 by main thread:
    #0 .omp_outlined._debug__ /home/lyu/Test/example.cpp:8:14 (tmpfile_KlukIr+0xb8e)
    #1 .omp_outlined. /home/lyu/Test/example.cpp:6:5 (tmpfile_KlukIr+0xcd4)
    #2 __kmp_invoke_microtask <null> (libomp.so+0xb9202)

  Location is heap block of size 4000 at 0x7b8000002000 allocated by main thread:
    #0 malloc /home/lyu/Repository/arbalest-vec/compiler-rt/lib/tsan/rtl/tsan_interceptors_posix.cpp:667:5 (example.exe+0x4d221)
    #1 DeviceTy::getTargetPointer(void*, void*, long, void*, bool, bool, bool, bool, bool, bool, bool, AsyncInfoTy&, void*) <null> (libomptarget.so.15+0xdfca)
    #2 main /home/lyu/Test/example.cpp:6:5 (example.exe+0xd76c9)
    #3 __libc_start_main /build/glibc-CVJwZb/glibc-2.27/csu/../csu/libc-start.c:310 (libc.so.6+0x21c86) (BuildId: f7307432a8b162377e77a182b6cc2e53d771ec4b)

  Variable/array involved in data inconsistency: a[0] (4-byte element)

SUMMARY: ThreadSanitizer: data inconsistency (uninitialized access) /home/lyu/Test/example.cpp:8:14 in .omp_outlined._debug__
==================
a[3] = 3
ThreadSanitizer: reported 1 warnings
```

## Citing Arbalest-Vec
If you are referring to Arbalest-Vec in a publication, please cite the following paper.
```
@inproceedings{yu2024facilitating,
  title={Facilitating Bug Detection for OpenMP Offloading Applications},
  author={Yu, Lechen and Jin, Feiyang and Jenke, Joachim and Sarkar, Vivek},
  booktitle={SC24-W: Workshops of the International Conference for High Performance Computing, Networking, Storage and Analysis},
  pages={189--195},
  year={2024},
  organization={IEEE}
}
```

