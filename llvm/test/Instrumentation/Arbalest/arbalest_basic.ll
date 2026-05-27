; Verify that the Arbalest pass instruments loads and stores with
; __arbalest_read* / __arbalest_write* callbacks.
;
; RUN: opt < %s -passes='arbalest-module,function(arbalest)' -arbalest=1 -S 2>/dev/null | FileCheck %s
; RUN: opt < %s -passes='arbalest-module,function(arbalest)' -S 2>/dev/null | FileCheck %s --check-prefix=NOFLAG

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"
target triple = "x86_64-unknown-linux-gnu"

; --- Load instrumentation ---

define i32 @read_4_bytes(i32* %a) {
entry:
  %tmp1 = load i32, i32* %a, align 4
  ret i32 %tmp1
}

; CHECK-LABEL: define i32 @read_4_bytes
; CHECK:         bitcast i32* %a to i8*
; CHECK-NEXT:    call void @__arbalest_read4
; CHECK-NEXT:    %tmp1 = load i32, i32* %a, align 4

; --- Store instrumentation ---

define void @write_4_bytes(i32* %a, i32 %v) {
entry:
  store i32 %v, i32* %a, align 4
  ret void
}

; CHECK-LABEL: define void @write_4_bytes
; CHECK:         bitcast i32* %a to i8*
; CHECK-NEXT:    call void @__arbalest_write4
; CHECK-NEXT:    store i32 %v, i32* %a, align 4

; --- Various access sizes ---

define i8 @read_1_byte(i8* %a) {
entry:
  %tmp = load i8, i8* %a, align 1
  ret i8 %tmp
}

; CHECK-LABEL: define i8 @read_1_byte
; CHECK:         call void @__arbalest_read1

define i16 @read_2_bytes(i16* %a) {
entry:
  %tmp = load i16, i16* %a, align 2
  ret i16 %tmp
}

; CHECK-LABEL: define i16 @read_2_bytes
; CHECK:         call void @__arbalest_read2

define i64 @read_8_bytes(i64* %a) {
entry:
  %tmp = load i64, i64* %a, align 8
  ret i64 %tmp
}

; CHECK-LABEL: define i64 @read_8_bytes
; CHECK:         call void @__arbalest_read8

; --- Unaligned access instrumentation ---

define i32 @read_4_bytes_unaligned(i32* %a) {
entry:
  %tmp1 = load i32, i32* %a, align 1
  ret i32 %tmp1
}

; CHECK-LABEL: define i32 @read_4_bytes_unaligned
; CHECK:         call void @__arbalest_unaligned_read4

define void @write_4_bytes_unaligned(i32* %a) {
entry:
  store i32 1, i32* %a, align 1
  ret void
}

; CHECK-LABEL: define void @write_4_bytes_unaligned
; CHECK:         call void @__arbalest_unaligned_write4

; --- Module constructor is created for host target ---

; CHECK: define internal void @arbalest.module_ctor
; CHECK:   call void @__arbalest_init

; --- Without -arbalest flag, pass is a no-op ---

; NOFLAG-LABEL: define i32 @read_4_bytes
; NOFLAG-NOT:     call void @__arbalest_read4
; NOFLAG:         %tmp1 = load i32, i32* %a, align 4
