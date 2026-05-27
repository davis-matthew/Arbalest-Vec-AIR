// Verify that -farbalest forwards -arbalest=1 (not the old -tsan-arbalest=1)
// and that -g also appends -arbalest-debug-info=1.
//
// REQUIRES: x86-registered-target

// -farbalest requires -fopenmp; without it the flag is silently dropped.
// RUN: %clang -target x86_64-unknown-linux -fsanitize=thread -fopenmp \
// RUN:        -fopenmp-targets=x86_64-pc-linux-gnu -farbalest %s \
// RUN:        -### 2>&1 | FileCheck %s --check-prefix=FLAG

// With -g, the driver also adds -arbalest-debug-info=1.
// RUN: %clang -target x86_64-unknown-linux -fsanitize=thread -fopenmp \
// RUN:        -fopenmp-targets=x86_64-pc-linux-gnu -farbalest -g %s \
// RUN:        -### 2>&1 | FileCheck %s --check-prefix=DEBUGFLAG

// -fno-arbalest suppresses the flag entirely.
// RUN: %clang -target x86_64-unknown-linux -fsanitize=thread -fopenmp \
// RUN:        -fopenmp-targets=x86_64-pc-linux-gnu -farbalest -fno-arbalest %s \
// RUN:        -### 2>&1 | FileCheck %s --check-prefix=NOFLAG

// FLAG:      "-mllvm" "-arbalest=1"
// FLAG-NOT:  "-tsan-arbalest=1"

// DEBUGFLAG: "-mllvm" "-arbalest=1"
// DEBUGFLAG: "-mllvm" "-arbalest-debug-info=1"
// DEBUGFLAG-NOT: "-tsan-debug-info=1"

// NOFLAG-NOT: "-arbalest=1"

int x;
void foo(void) { x = 1; }
