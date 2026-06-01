//===- OMPSanitizer.h - Analyze omp target clauses and infer data
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

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_OMPSAN_OMPSANITIZER_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_OMPSAN_OMPSANITIZER_H

#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/AbstractCallSite.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Transforms/Instrumentation/OMPSan/MemUseDefAnalysis.h"
#include "llvm/Transforms/Instrumentation/OMPSan/OmpDiagnosticsAnalysis.h"


#include <queue>
#include <set>
#include <unordered_map>


namespace llvm {

using namespace llvm;
using SetOfValNames = std::set<std::string>;
using ConstInstrPtr = const Instruction *;
using SetOfInstructions = std::set<ConstInstrPtr>;

class ValidateOmpReachingDefs {

  // const InstructionToMemCopyMapType &AllocatedDevice, &HostToDeviceCopy,
  // &DeviceToHostCopy, &DevicePersistent;

  // const MemUseToReachingDefsMapType &MemUseToReachingDefsMap;

  MemoryLdStMapClass &MemInfo;
  OmpDiagnosticsInfo &OmpInfo;
  /// Map of a function to the enclosing "omp" RTL function, that is if a
  /// function is called within an OMP RTL function, then record the parent RTL,
  /// else just store nullptr. So, if a function is mapped to nullptr, that
  /// means it is executed on host, and if a function is mapped to a valid
  /// function, it is executed on the device.
  std::map<const Function *, const Function *> FuncEnvMap;
  std::map<const Function *, unsigned> CalledFuncLocationMap;
  // std::map<unsigned , std::set<const Function *>> LocationToCalledFuncMap;
  // DebugLocation OmpDiagnosticsLocationInfo;
  std::map<unsigned, SetOfValNames> AllocatedOnDeviceMap;
  std::map<unsigned, SetOfValNames> HostToDeviceMap;
  std::map<unsigned, SetOfValNames> DeviceToHostMap;
  std::map<unsigned, SetOfValNames> PersistentInMap;
  std::map<unsigned, SetOfValNames> PersistentOutMap;
  const Function *InsideOMPCall;
  // bool InsideTargetEnv;

  void recordOmpMaps();

public:
  ValidateOmpReachingDefs(MemoryLdStMapClass &M, OmpDiagnosticsInfo &O)
      : MemInfo(M), OmpInfo(O), InsideOMPCall(nullptr) {
    recordOmpMaps();
  }

  // ValidateOmpReachingDefs(InstructionToMemCopyMapType &A,
  // InstructionToMemCopyMapType &B, InstructionToMemCopyMapType &C,
  // InstructionToMemCopyMapType &D, MemUseToReachingDefsMapType &E,
  // DebugLocation &F): AllocatedDevice(A), HostToDeviceCopy(B),
  // DeviceToHostCopy(C), DevicePersistent(D), MemUseToReachingDefsMap(E),
  // OmpDiagnosticsLocationInfo(F), InsideTargetEnv(false) {}
  void analyzeModule(Module &M);
  void analyzeFunction(const Function &F);
  void analyzeBasicBlock(const BasicBlock &BB);
};

class BugInfo {
  public:
    MemoryLdStMapClass memInfo;
    const Value *memAlloc; 
    ConstInstrPtr memDefInstr;
    ConstInstrPtr memUseInstr;
    std::string varName, suggestion;

  BugInfo(
    MemoryLdStMapClass MemInfo, const Value * MemAlloc, 
    ConstInstrPtr MemDefInstr, ConstInstrPtr MemUseInstr, 
    //ConstInstrPtr DefPragmaOmpSeq, ConstInstrPtr UsePragmaOmpSeq, 
    std::string DefArrayName, 
    std::string ErrorMessage) :

    memInfo(MemInfo), memAlloc(MemAlloc),
    memDefInstr(MemDefInstr), memUseInstr(MemUseInstr), 
    varName(DefArrayName), suggestion(ErrorMessage){}

    std::vector<ConstInstrPtr> getInstructions();
};

// For set to remove duplicates
bool operator <(const BugInfo & a, const BugInfo &b);

// struct LLVMFuncHash   
// {
//   size_t operator()(const Function& f) const { 
//     return std::hash<std::string>{}(f.getName().str());
//   }
// };

// bool operator ==(const Function &A, const Function &B){
//   return A.getName() == B.getName();
// }

class OMPSanInfo {
  public:
    std::set<BugInfo> bugs;
    // using the string name of an llvm function instead of writing hash functions
    std::unordered_map<std::string, SetOfInstructions> includeList;
    std::set<ConstValuePtr> variableList;

    void setBugs(std::set<BugInfo>);
    std::unordered_map<std::string, SetOfInstructions> getIncludeList();
};


/// This pass performs the global (interprocedural) Omp Data Mapping Analysis.
/// (New pass manager).
class OmpSanitizerGlobalAnalysis
    : public AnalysisInfoMixin<OmpSanitizerGlobalAnalysis> {
  friend AnalysisInfoMixin<OmpSanitizerGlobalAnalysis>;
  static AnalysisKey Key;
  Module *ThisModule;
  ModuleAnalysisManager *AnalysisManager;

public:
  using Result = OMPSanInfo;
  Result run(Module &M, ModuleAnalysisManager &AM);
};

/// Printer pass for the \c OmpDiagnosticsGlobalAnalysis results.
class OmpSanitizerGlobalPrinterPass
    : public PassInfoMixin<OmpSanitizerGlobalPrinterPass> {
  raw_ostream &OS;

public:
  explicit OmpSanitizerGlobalPrinterPass(raw_ostream &OS) : OS(OS) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_INSTRUMENTATION_OMPSAN_OMPSANITIZER_H
