; arbalest_ompsan_selective.ll
;
; Verifies that the -arbalest-only-functions include list restricts
; instrumentation to the nominated function and leaves the other untouched.
;
; This is the IR-level analogue of the OMPSan-guided path:
;   - bad_mapping  is listed → must receive arbalest call sites
;   - pure_host    is absent → must receive NO arbalest call sites
;
; The same selective behaviour is exercised at the C level via ompsan_selective.c
; with -arbalest-ompsan, where OMPSan's bug report populates the include list
; automatically.  Here we drive the include list directly with
; -arbalest-only-functions=bad_mapping so the test runs without an OpenMP
; toolchain or an OMPSan-capable build.
;
; RUN: opt < %s \
; RUN:   -passes='arbalest-module,function(arbalest)' \
; RUN:   -arbalest=1 \
; RUN:   -arbalest-only-functions=bad_mapping \
; RUN:   -S 2>/dev/null \
; RUN: | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-musl"

; ── bad_mapping ───────────────────────────────────────────────────────────────
; Models the host-side loop that reads arr[] after a target kernel has written
; it with a map(to:) — the use-def mismatch that OMPSan would flag.
; Because bad_mapping is in the include list, Arbalest MUST instrument the
; load inside it.
;
; The loop body is an affine recurrence over arr[] with a statically-known
; element size, so Arbalest's hoist pass emits a single __arbalest_read_range
; call in the loop preheader rather than per-element callbacks.  We match
; the prefix @__arbalest_read to cover both the hoisted path (_read_range,
; _read_cstride) and the per-element path (_read8, etc.) without pinning the
; test to a specific dedup/hoist configuration.
;
; CHECK-LABEL: define void @bad_mapping
; CHECK:         call void @__arbalest_read
define void @bad_mapping(double* %arr, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %ptr = getelementptr inbounds double, double* %arr, i64 %i
  ; Host read of arr[i] — use on host after a (modelled) device write.
  ; Arbalest should insert a read callback here.
  %val = load double, double* %ptr, align 8
  %doubled = fmul double %val, 2.0
  %i.next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; ── pure_host ─────────────────────────────────────────────────────────────────
; A host-only L2-norm reduction.  No OpenMP target constructs, no data-mapping
; events.  Because pure_host is NOT in the include list, Arbalest must emit
; zero call sites here.
;
; CHECK-LABEL: define double @pure_host
; CHECK-NOT:     call void @__arbalest_
define double @pure_host(double* %arr, i64 %n) {
entry:
  br label %loop

loop:
  %i    = phi i64    [ 0,    %entry ], [ %i.next, %loop ]
  %acc  = phi double [ 0.0,  %entry ], [ %acc.next, %loop ]
  %ptr2 = getelementptr inbounds double, double* %arr, i64 %i
  ; Host-only load — Arbalest must NOT instrument this when pure_host
  ; is absent from the include list.
  %val2 = load double, double* %ptr2, align 8
  %sq   = fmul double %val2, %val2
  %acc.next = fadd double %acc, %sq
  %i.next   = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret double %acc.next
}
