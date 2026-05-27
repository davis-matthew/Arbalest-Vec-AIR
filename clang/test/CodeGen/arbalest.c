// Verify that the Arbalest pass is scheduled when -farbalest -fsanitize=thread
// are both active, and that the correct runtime callbacks are emitted.
// -fopenmp is required by the -farbalest driver flag.
//
// REQUIRES: x86-registered-target

// RUN: %clang -target x86_64-unknown-linux-gnu -fsanitize=thread -fopenmp \
// RUN:        -fopenmp-targets=x86_64-pc-linux-gnu -farbalest \
// RUN:        %s -S -emit-llvm -o - 2>/dev/null | FileCheck %s

int foo(int *a) { return *a; }
void bar(int *a) { *a = 42; }

// In the emitted IR, function bodies come before the module constructors.
// Read and write callbacks are inserted around the accesses in foo/bar.
// CHECK: call void @__arbalest_read4
// CHECK: call void @__arbalest_write4

// Module constructor calls __arbalest_init (x86_64-unknown-linux-gnu is
// the host triple that triggers insertArbalestCtor).
// CHECK: call void @__arbalest_init
