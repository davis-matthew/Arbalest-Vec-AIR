//===- Transforms/Instrumentation/Arbalest.h - Arbalest Pass --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Arbalest data inconsistency detector instrumentation pass. Runs alongside
// ThreadSanitizer and emits __arbalest_* runtime callbacks that share TSan's
// runtime library. Enabled with -mllvm -arbalest=1 (driver flag: -farbalest).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_ARBALEST_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_ARBALEST_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;
class Module;

struct ArbalestPass : public PassInfoMixin<ArbalestPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
  static bool isRequired() { return true; }
};

struct ModuleArbalestPass : public PassInfoMixin<ModuleArbalestPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_ARBALEST_H
