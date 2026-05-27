; Verify that the Arbalest pass skips loads from constant globals and does not
; insert instrumentation for them.
;
; A non-linux target triple is used so that insertArbalestCtor does not run
; (it only activates for "x86_64-unknown-linux-gnu"), keeping this test focused
; solely on the skipping logic.
;
; RUN: opt < %s -passes='arbalest-module,function(arbalest)' -arbalest=1 -S 2>/dev/null | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"
target triple = "x86_64-unknown-linux-musl"

@const_global = constant i32 42
@mutable_global = global i32 0

; Loads from a constant global must not be instrumented.
define i32 @read_constant() {
entry:
  %tmp = load i32, i32* @const_global, align 4
  ret i32 %tmp
}

; CHECK-LABEL: define i32 @read_constant
; CHECK-NOT:     call void @__arbalest_read
; CHECK:         %tmp = load i32, i32* @const_global

; Loads from a mutable global must still be instrumented.
define i32 @read_mutable() {
entry:
  %tmp = load i32, i32* @mutable_global, align 4
  ret i32 %tmp
}

; CHECK-LABEL: define i32 @read_mutable
; CHECK:         call void @__arbalest_read4
; CHECK:         %tmp = load i32, i32* @mutable_global
