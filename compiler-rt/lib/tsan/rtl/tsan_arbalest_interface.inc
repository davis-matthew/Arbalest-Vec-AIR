#include "tsan_interface.h"
#include "tsan_rtl.h"
#include "tsan_shadow.h"

#define CALLERPC ((uptr)__builtin_return_address(0))

using namespace __tsan;

void __arbalest_init(u32 global_num, void **global_ptr,
                     u64 *global_size, char **global_name) {
  arbalest_enabled = true;
  // VPrintf("arbalest init global num = %u\n", global_num);
  for (u32 i = 0; i < global_num; i++) {
    // VPrintf("  global[%u] %s, ptr = %p, size = %llu\n", i, global_name[i],
    //       global_ptr[i], global_size[i]);
    uptr global_start = reinterpret_cast<uptr>(global_ptr[i]);
    ctx->globals.insert(Interval{global_start, global_start + global_size[i]}, 
                        MapInfo{global_start, global_size[i], global_name[i]});
    VsmRangeSet(global_start, global_size[i], VariableStateMachine::kHostMask);
  }
}

void __arbalest_read1(void *addr) {
  CheckVsm(cur_thread(), CALLERPC, (uptr)addr, 1);
}

void __arbalest_read2(void *addr) {
  CheckVsm(cur_thread(), CALLERPC, (uptr)addr, 2);
}

void __arbalest_read4(void *addr) {
  CheckVsm(cur_thread(), CALLERPC, (uptr)addr, 4);
}

void __arbalest_read8(void *addr) {
  CheckVsm(cur_thread(), CALLERPC, (uptr)addr, 8);
}

void __arbalest_read16(void *addr) {
  CheckVsm16(cur_thread(), CALLERPC, (uptr)addr);
}

void __arbalest_write1(void *addr) {
  UpdateVsm(cur_thread(), (uptr)addr, 1);
}

void __arbalest_write2(void *addr) {
  UpdateVsm(cur_thread(), (uptr)addr, 2);
}

void __arbalest_write4(void *addr){
  UpdateVsm(cur_thread(), (uptr)addr, 4);
}

void __arbalest_write8(void *addr) {
  UpdateVsm(cur_thread(), (uptr)addr, 8);
}

void __arbalest_write16(void *addr) {
  UpdateVsm16(cur_thread(), (uptr)addr);
}

void __arbalest_unaligned_read2(const void *addr) {
  UnalignedCheckVsm(cur_thread(), CALLERPC, (uptr)addr, 2);
}

void __arbalest_unaligned_read4(const void *addr) {
  UnalignedCheckVsm(cur_thread(), CALLERPC, (uptr)addr, 4);
}

void __arbalest_unaligned_read8(const void *addr) {
  UnalignedCheckVsm(cur_thread(), CALLERPC, (uptr)addr, 8);
}

void __arbalest_unaligned_read16(const void *addr) {
  UnalignedCheckVsm16(cur_thread(), CALLERPC, (uptr)addr);
}

void __arbalest_unaligned_write2(void *addr) {
  UnalignedUpdateVsm(cur_thread(), (uptr)addr, 2);
}

void __arbalest_unaligned_write4(void *addr) {
  UnalignedUpdateVsm(cur_thread(), (uptr)addr, 4);
}

void __arbalest_unaligned_write8(void *addr) {
  UnalignedUpdateVsm(cur_thread(), (uptr)addr, 8);
}

void __arbalest_unaligned_write16(void *addr) {
  UnalignedUpdateVsm16(cur_thread(), (uptr)addr);
}

void __arbalest_check_bound(void *base, void *start, unsigned size) {
  CheckBound(cur_thread(), CALLERPC, (uptr)base, (uptr)start, size);
}

// --- Loop-hoisted range/stride callbacks ---
//
// These are emitted by the Arbalest pass when SCEV analysis proves that a
// loop body accesses memory in a predictable linear pattern, allowing the
// per-element callbacks to be replaced with a single pre-loop call.
//
// Naming convention:
//   _range   : contiguous [start, end) — step == elem_size (unit stride)
//   _stride  : strided [start, end) step bytes apart — step unknown at compile time
//   _cstride : same, but step is a compile-time constant (enables SIMD specialization)
//
// For _stride and _cstride, elem_bytes is the size of each individual access
// (may be smaller than step for non-unit strides, e.g. int in a double-stride loop).

// --- helpers (file-local) ---

static void CheckVsmStridedRange(ThreadState *thr, uptr pc, uptr start,
                                  uptr end, uptr step, uptr elem_bytes) {
  for (uptr addr = start; addr < end; addr += step) {
    if (elem_bytes <= kVsmCell) {
      if (UNLIKELY(CheckVsm(thr, pc, addr, elem_bytes)))
        return;
    } else {
      CheckVsmForMemoryRange(thr, pc, addr, elem_bytes);
    }
  }
}

static void UpdateVsmStridedRange(ThreadState *thr, uptr start, uptr end,
                                   uptr step, uptr elem_bytes) {
  for (uptr addr = start; addr < end; addr += step) {
    if (elem_bytes <= kVsmCell)
      UpdateVsm(thr, addr, elem_bytes);
    else
      UpdateVsmForMemoryRange(thr, addr, elem_bytes);
  }
}

// --- contiguous range ---

void __arbalest_read_range(void *start, void *end) {
  uptr s = (uptr)start, e = (uptr)end;
  if (LIKELY(s >= e)) return;
  CheckVsmForMemoryRange(cur_thread(), CALLERPC, s, e - s);
}

void __arbalest_write_range(void *start, void *end) {
  uptr s = (uptr)start, e = (uptr)end;
  if (LIKELY(s >= e)) return;
  UpdateVsmForMemoryRange(cur_thread(), s, e - s);
}

// --- dynamic stride ---

void __arbalest_read_stride(void *start, void *end, uptr step, uptr elem_bytes) {
  uptr s = (uptr)start, e = (uptr)end;
  if (LIKELY(s >= e)) return;
  CheckVsmStridedRange(cur_thread(), CALLERPC, s, e, step, elem_bytes);
}

void __arbalest_write_stride(void *start, void *end, uptr step, uptr elem_bytes) {
  uptr s = (uptr)start, e = (uptr)end;
  if (LIKELY(s >= e)) return;
  UpdateVsmStridedRange(cur_thread(), s, e, step, elem_bytes);
}

// --- constant stride (SIMD specializations for step == 16) ---

void __arbalest_read_cstride(void *start, void *end, uptr step, uptr elem_bytes) {
  uptr s = (uptr)start, e = (uptr)end;
  if (LIKELY(s >= e)) return;
  ThreadState *thr = cur_thread();
  // step==16 && elem==16: each element fills one SSE2 word; use CheckVsm16.
  if (step == 16 && elem_bytes == 16) {
    for (uptr addr = s; addr < e; addr += 16)
      if (UNLIKELY(CheckVsm16(thr, CALLERPC, addr)))
        return;
    return;
  }
  CheckVsmStridedRange(thr, CALLERPC, s, e, step, elem_bytes);
}

void __arbalest_write_cstride(void *start, void *end, uptr step, uptr elem_bytes) {
  uptr s = (uptr)start, e = (uptr)end;
  if (LIKELY(s >= e)) return;
  ThreadState *thr = cur_thread();
  // step==16 && elem==16: use UpdateVsm16 (SSE2 _mm_storeu_si128 path).
  if (step == 16 && elem_bytes == 16) {
    for (uptr addr = s; addr < e; addr += 16)
      UpdateVsm16(thr, addr);
    return;
  }
  UpdateVsmStridedRange(thr, s, e, step, elem_bytes);
}