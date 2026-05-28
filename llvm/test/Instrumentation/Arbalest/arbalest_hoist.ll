; Verify loop-hoisting of Arbalest range/stride checks.
; A canonical loop over an array should produce a single hoisted call in the
; preheader instead of per-element callbacks inside the loop body.
;
; RUN: opt < %s -passes='arbalest-module,function(arbalest)' -arbalest=1 \
; RUN:        -arbalest-hoist=true -S 2>/dev/null | FileCheck %s
; RUN: opt < %s -passes='arbalest-module,function(arbalest)' -arbalest=1 \
; RUN:        -arbalest-hoist=false -S 2>/dev/null | FileCheck %s --check-prefix=NOHOIST

; Non-linux triple so insertArbalestCtor is skipped, keeping output clean.
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"
target triple = "x86_64-unknown-linux-musl"

; Unit-stride write: x[i] = v  (step == elem_size == 4).
; Hoisted: one __arbalest_write_range in the preheader.
; Loop body: no __arbalest_write* inside the loop.
define void @store_unit_stride(i32* %x, i64 %n, i32 %v) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %gep = getelementptr i32, i32* %x, i64 %i
  store i32 %v, i32* %gep, align 4
  %i.next = add i64 %i, 1
  %cond = icmp slt i64 %i.next, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret void
}

; CHECK-LABEL: define void @store_unit_stride
; Hoisted call lands in the entry block (the loop preheader).
; CHECK-LABEL: entry:
; CHECK:         call void @__arbalest_write_range
; CHECK-NOT:     call void @__arbalest_write_range
; CHECK-LABEL: loop:
; CHECK-NOT:     call void @__arbalest_write

; Without hoisting every store in the loop body is instrumented.
; NOHOIST-LABEL: define void @store_unit_stride
; NOHOIST-LABEL: loop:
; NOHOIST:         call void @__arbalest_write4

; Unit-stride read: sum = sum + x[i].
; Hoisted: one __arbalest_read_range in the preheader.
define i32 @load_unit_stride(i32* %x, i64 %n) {
entry:
  br label %loop
loop:
  %i   = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %sum = phi i32 [ 0, %entry ], [ %sum.next, %loop ]
  %gep = getelementptr i32, i32* %x, i64 %i
  %v   = load i32, i32* %gep, align 4
  %sum.next = add i32 %sum, %v
  %i.next   = add i64 %i, 1
  %cond = icmp slt i64 %i.next, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret i32 %sum.next
}

; CHECK-LABEL: define i32 @load_unit_stride
; CHECK-LABEL: entry:
; CHECK:         call void @__arbalest_read_range
; CHECK-LABEL: loop:
; CHECK-NOT:     call void @__arbalest_read

; Stride-2 write: x[2*i] = v  (step=8, elem_size=4 → non-unit, const stride).
; Hoisted: one __arbalest_write_cstride with step=8, elem=4.
define void @store_stride2(i32* %x, i64 %n, i32 %v) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %i2 = mul i64 %i, 2
  %gep = getelementptr i32, i32* %x, i64 %i2
  store i32 %v, i32* %gep, align 4
  %i.next = add i64 %i, 1
  %cond = icmp slt i64 %i.next, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret void
}

; CHECK-LABEL: define void @store_stride2
; CHECK-LABEL: entry:
; CHECK:         call void @__arbalest_write_cstride
; CHECK-LABEL: loop:
; CHECK-NOT:     call void @__arbalest_write
