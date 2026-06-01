//===- OmpOptiMem.h - Analyze omp target clauses and infer data
// mapping -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// OpenMp target data mapping Analysis,
// A Diagnostics Pass to help understand usage of data mapping clauses.
// Looks at all the omp target RTL calls, and interprets their semantics,
// to conclude which variable is residing on Device or Host memory,
// outside and inside target regions.
// RTL is an acronym for "Run Time Library"
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_OMPSAN_OMPOPTIMEM_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_OMPSAN_OMPOPTIMEM_H

#include "llvm/ADT/BreadthFirstIterator.h"
#include "llvm/Transforms/Instrumentation/OMPSan/MemUseDefAnalysis.h"
#include "llvm/Transforms/Instrumentation/OMPSan/OmpDiagnosticsAnalysis.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/DominanceFrontier.h"
#include "llvm/IR/AbstractCallSite.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/LoopInfo.h"
#include <queue>
#include <set>
#include <stack>

namespace llvm {

using namespace llvm;
using SetOfValNames = std::set<std::string>;
using FuncToLoopInfoTy = std::map< const Function *, LoopInfo *>;
using FuncToDomTreeTy = std::map< const Function *, DominatorTree*>;
using BasicBlockListTy = std::vector<const BasicBlock*>;

const StringRef HostOffloadingCallName = "__omp_offloading";
class ClassifyHostDevice {
  using BeginAndEndSeq = std::pair<unsigned, unsigned>;
  using ListOfFuncsTy = std::vector<const Function *>;

  Module &ThisModule;
  const MemoryLdStMapClass &MemInfo;
  std::map<const Function *, const Function *> FuncEnvMap;
  std::map<const Function *, BeginAndEndSeq > CalledFuncLocationMap;
  ListOfFuncsTy CallStack;
  std::map<const Function*, std::set<ListOfFuncsTy>> FuncToCallContext;
  const Function *InsideOMPCall;
  void analyzeBasicBlock(const BasicBlock &BB);
  void analyzeFunction(const Function &F);

  public:
  ClassifyHostDevice(Module &M, const MemoryLdStMapClass &MemI ): ThisModule(M), MemInfo(MemI), InsideOMPCall(nullptr){};
  void run();
  const Function *isDevice(const Instruction &I) const ;
  unsigned getFunctionPos(const Function *F) {
    if (EXISTSinMap(CalledFuncLocationMap,F))
      return CalledFuncLocationMap[F].first;
    return 0 ;
  }
  unsigned getTargetEndPos(const Function *F) {
    if (EXISTSinMap(CalledFuncLocationMap,F))
      return CalledFuncLocationMap[F].second;
    return 0 ;
  }
};

class FindOptiMem {
  Module &ThisModule;
  const MemoryLdStMapClass &MemInfo;
  FunctionAnalysisManager &FunctionAM;
  const FuncToLoopInfoTy &FuncToLoopInfoMap;
  //const OmpDiagnosticsInfo &OmpInfo;

  unsigned  findEquivalnceSet(const BasicBlock *UseBB, const SetOfInstructions &ReachingDefsAtUser, const LoopInfo &LI );

  bool isInEquivalenceSet(const BasicBlock*BB, const SetOfInstructions &RDefs);
  unsigned  searchOptimalBB(BasicBlockListTy &EquivalenceBBQueue);

  bool filterReachingDefs(const SetOfInstructions &RDefs, SetOfInstructions &FilteredSet, const ClassifyHostDevice &CHD, const Function *OmpFuncUser);
  bool isValidUseInstruction(const Instruction *I);
  std::string getArrayName(const Instruction *I) const ;
  unsigned getBestEquivalaneceBB(const BasicBlock *UseBB, const Instruction &Reaching);
  unsigned getBestEquivalaneceBB(const BasicBlock *UseBB, const SetOfInstructions &ReachingDefs);

  void getCallStack(Function &Func, std::set<std::string> SetOfCallStacks);
  unsigned getBBLineNum(const BasicBlock *BB)const;
  const BasicBlock *getParentThatGeneratesDef(const Instruction &I, const Instruction &ReachingDef);
  public:
  FindOptiMem(Module &M, MemoryLdStMapClass &L, FunctionAnalysisManager &FM, FuncToLoopInfoTy &FtoL):ThisModule(M), MemInfo(L), FunctionAM(FM), FuncToLoopInfoMap(FtoL){}
  void run();
};

/// This pass performs the global (interprocedural) Omp Data Mapping Analysis.
/// (New pass manager).
class OmpOptiMemGlobalAnalysis
    : public AnalysisInfoMixin<OmpOptiMemGlobalAnalysis> {
  friend AnalysisInfoMixin<OmpOptiMemGlobalAnalysis>;
  static AnalysisKey Key;
  Module *ThisModule;
  ModuleAnalysisManager *AnalysisManager;
  FunctionAnalysisManager *FunctionAM;

public:
  using Result = PreservedAnalyses;
  Result run(Module &M, ModuleAnalysisManager &AM);
};

/// Printer pass for the \c OmpDiagnosticsGlobalAnalysis results.
class OmpOptiMemGlobalPrinterPass
    : public PassInfoMixin<OmpOptiMemGlobalPrinterPass> {
  raw_ostream &OS;

public:
  explicit OmpOptiMemGlobalPrinterPass(raw_ostream &OS) : OS(OS) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_OMPSAN_OMPOPTIMEM_H
