; Verify that the Arbalest pass instruments GEPs in OpenMP outlined functions
; with __arbalest_check_bound, and that regular functions do not get that
; instrumentation (only loads/stores).
;
; RUN: opt < %s -passes='arbalest-module,function(arbalest)' -arbalest=1 -S 2>/dev/null | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"
target triple = "x86_64-unknown-linux-gnu"

; A regular (non-outlined) function with a GEP-based load should get
; __arbalest_read but NOT __arbalest_check_bound.
define i32 @regular_gep_load(i32* %arr, i64 %idx) {
entry:
  %gep = getelementptr i32, i32* %arr, i64 %idx
  %val = load i32, i32* %gep, align 4
  ret i32 %val
}

; CHECK-LABEL: define i32 @regular_gep_load
; CHECK:         call void @__arbalest_read4
; CHECK-NOT:     call void @__arbalest_check_bound
; CHECK:         %val = load i32, i32* %gep, align 4

; An OpenMP outlined function with a GEP-based load should get BOTH
; __arbalest_read and __arbalest_check_bound.
define void @".omp_outlined."(i32* %arr, i64 %idx) {
entry:
  %gep = getelementptr i32, i32* %arr, i64 %idx
  %val = load i32, i32* %gep, align 4
  ret void
}

; CHECK-LABEL: define void @".omp_outlined."
; CHECK:         call void @__arbalest_read4
; CHECK:         call void @__arbalest_check_bound(i8* {{.*}}, i8* {{.*}}, i32 4)
; CHECK:         %val = load i32, i32* %gep, align 4

; A store through a GEP in an outlined function gets __arbalest_write but no
; check_bound (check_bound is only for loads through GEPs).
define void @".omp_outlined._store"(i32* %arr, i64 %idx) {
entry:
  %gep = getelementptr i32, i32* %arr, i64 %idx
  store i32 42, i32* %gep, align 4
  ret void
}

; CHECK-LABEL: define void @".omp_outlined._store"
; CHECK:         call void @__arbalest_write4
; CHECK-NOT:     call void @__arbalest_check_bound
; CHECK:         store i32 42, i32* %gep, align 4
