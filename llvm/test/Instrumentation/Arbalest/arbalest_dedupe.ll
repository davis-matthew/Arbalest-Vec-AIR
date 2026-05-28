; Verify the serial-elision dedup logic: repeated accesses to the same address
; within a call-bounded region should be elided; a call/invoke resets state so
; the first access after a call is still instrumented.
;
; RUN: opt < %s -passes='arbalest-module,function(arbalest)' -arbalest=1 \
; RUN:        -arbalest-dedupe-mode=strip -S 2>/dev/null | FileCheck %s --check-prefix=STRIP
; RUN: opt < %s -passes='arbalest-module,function(arbalest)' -arbalest=1 \
; RUN:        -arbalest-dedupe-mode=off -S 2>/dev/null | FileCheck %s --check-prefix=OFF
; RUN: opt < %s -passes='arbalest-module,function(arbalest)' -arbalest=1 \
; RUN:        -arbalest-dedupe-mode=value -S 2>/dev/null | FileCheck %s --check-prefix=VALUE

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"
target triple = "x86_64-unknown-linux-musl"

declare void @sink()

; Two consecutive loads from the same pointer. With dedup the second load must
; not emit a callback; without dedup both must.
define i32 @double_load(i32* %p) {
entry:
  %v1 = load i32, i32* %p, align 4
  %v2 = load i32, i32* %p, align 4
  %s = add i32 %v1, %v2
  ret i32 %s
}

; STRIP-LABEL: define i32 @double_load
; STRIP:         call void @__arbalest_read4
; STRIP-NEXT:    %v1 = load i32
; STRIP-NOT:     call void @__arbalest_read4
; STRIP:         %v2 = load i32

; OFF-LABEL: define i32 @double_load
; OFF:          call void @__arbalest_read4
; OFF-NEXT:     %v1 = load i32
; OFF:          call void @__arbalest_read4
; OFF-NEXT:     %v2 = load i32

; VALUE-LABEL: define i32 @double_load
; VALUE:          call void @__arbalest_read4
; VALUE-NEXT:     %v1 = load i32
; VALUE-NOT:      call void @__arbalest_read4
; VALUE:          %v2 = load i32

; A read followed by a write to the same pointer: the write must still be
; emitted (first write after a read transitions SeenRead -> SeenWrite).
define void @read_then_write(i32* %p) {
entry:
  %v = load i32, i32* %p, align 4
  store i32 %v, i32* %p, align 4
  ret void
}

; STRIP-LABEL: define void @read_then_write
; STRIP:         call void @__arbalest_read4
; STRIP-NEXT:    %v = load i32
; STRIP:         call void @__arbalest_write4
; STRIP-NEXT:    store i32

; A call between two loads resets dedup state so the post-call load must be
; re-instrumented even though it targets the same address.
define i32 @load_call_load(i32* %p) {
entry:
  %v1 = load i32, i32* %p, align 4
  call void @sink()
  %v2 = load i32, i32* %p, align 4
  %s = add i32 %v1, %v2
  ret i32 %s
}

; STRIP-LABEL: define i32 @load_call_load
; STRIP:         call void @__arbalest_read4
; STRIP-NEXT:    %v1 = load i32
; STRIP:         call void @sink
; STRIP:         call void @__arbalest_read4
; STRIP-NEXT:    %v2 = load i32

; Two writes to the same pointer — second write is elided (SeenWrite terminal).
define void @double_write(i32* %p) {
entry:
  store i32 1, i32* %p, align 4
  store i32 2, i32* %p, align 4
  ret void
}

; STRIP-LABEL: define void @double_write
; STRIP:         call void @__arbalest_write4
; STRIP-NEXT:    store i32 1
; STRIP-NOT:     call void @__arbalest_write4
; STRIP:         store i32 2

; OFF-LABEL: define void @double_write
; OFF:          call void @__arbalest_write4
; OFF-NEXT:     store i32 1
; OFF:          call void @__arbalest_write4
; OFF-NEXT:     store i32 2
