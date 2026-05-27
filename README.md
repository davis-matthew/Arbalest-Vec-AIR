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

