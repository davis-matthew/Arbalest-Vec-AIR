//===- OMPSanitizer.cpp - Stack memory safety analysis
//-------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/OMPSan/OmpSanitizer.h"

using namespace llvm;

#define DEBUG_TYPE "omp-sanitizer"

// OMP COMPOUND
///////////////////////////////////////////////////
///////////////////////////////////////////////////
///////////////////////////////////////////////////
///////////////////////////////////////////////////
///////////////////////////////////////////////////
#include <fstream>
#include <string>
#include <iostream>
#include <unordered_map>

namespace llvm {
bool operator<(const BugInfo &a, const BugInfo &b) {
  return a.suggestion < b.suggestion;
}
} // namespace llvm

std::vector<ConstInstrPtr> BugInfo::getInstructions(){
  std::vector<ConstInstrPtr> insts;
  // memAlloc is a Value* and may or may not be an Instruction.
  if (const auto *AllocInstr = dyn_cast<Instruction>(memAlloc))
    insts.push_back(AllocInstr);
  insts.push_back(memDefInstr);
  insts.push_back(memUseInstr);
  return insts;
}



void OMPSanInfo::setBugs(std::set<BugInfo> bugList)
{
  bugs = bugList;
  for(BugInfo bug : bugs){
    variableList.insert(bug.memAlloc);
    variableList.insert((Value*) bug.memDefInstr);
    variableList.insert((Value*) bug.memUseInstr);
  }
}
std::unordered_map<std::string, SetOfInstructions> OMPSanInfo::getIncludeList() {
  if(includeList.size() == 0) { // construct map
    for(BugInfo bug : bugs) {
      for (auto inst : bug.getInstructions()) {
        includeList[inst->getFunction()->getName().str()].insert(inst);
      }
    }
  }
  return includeList;
}

std::set<BugInfo> bugs;

void generateOMPSanReport(){
  std::ofstream output("/home/mdavis/OMPSanReport.txt");
  if(output.is_open()){
    for(BugInfo bug : bugs){
        output << bug.suggestion << "\n";
    }
    output.close();
  }
  else{
    //Error
  }
}

// Write an LLVM sanitizer allowlist file with one "fun:<name>" entry per
// function implicated in any detected bug.  The file is written to
// /home/mdavis/OMPSanAllowlist.txt so it can be inspected alongside the
// bug report.  Arbalest reads the same information directly via
// OMPSanInfo::getIncludeList() rather than parsing this file.
static void generateAllowlist(){
  std::set<std::string> allowList;

  for (const BugInfo &bug : bugs) {
    if (const auto *AllocInstr = dyn_cast<Instruction>(bug.memAlloc))
      allowList.insert("fun:" + AllocInstr->getFunction()->getName().str());
    allowList.insert("fun:" + bug.memDefInstr->getFunction()->getName().str());
    allowList.insert("fun:" + bug.memUseInstr->getFunction()->getName().str());
  }

  std::ofstream output("/home/mdavis/OMPSanAllowlist.txt");
  if (output.is_open()) {
    output << "# OMPSan-derived function allowlist for Arbalest\n";
    for (const std::string &l : allowList)
      output << l << "\n";
    output.close();
  }
}

///////////////////////////////////////////////////
///////////////////////////////////////////////////
///////////////////////////////////////////////////
///////////////////////////////////////////////////
///////////////////////////////////////////////////


void ValidateOmpReachingDefs::analyzeBasicBlock(const BasicBlock &BB) {
  for (auto &I : BB) {
    if (const CallInst *Call = dyn_cast<CallInst>(&I)) {
      const Function *CalledFunc = Call->getCalledFunction();
      for (auto &A : Call->args()) {
        if (auto F =
                dyn_cast<Function>(A->stripPointerCastsAndAliases())) {
          LLVM_DEBUG(dbgs() << " Arg ::" << A << ":"
                            << *(A->stripPointerCastsAndAliases()));
          CalledFunc = F;
        }
      }
      if (CalledFunc == nullptr || !CalledFunc->hasName())
        continue;
      // Ignore recursive calls
      if (CalledFunc == BB.getParent())
        continue;
      if (CalledFunc->isIntrinsic() || CalledFunc->isDeclaration())
        continue;
      CalledFuncLocationMap[CalledFunc] =
          MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(Call);
      analyzeFunction(*CalledFunc);
    }
  }
}

void ValidateOmpReachingDefs::analyzeFunction(const Function &F) {
  if (EXISTSinMap(FuncEnvMap, &F))
    return;
  bool TargetRegBeginCall = false;
  if (F.getName().find("__omp_offloading") != std::string::npos &&
      InsideOMPCall == nullptr) {
    TargetRegBeginCall = true;
    InsideOMPCall = &F;
  }
  if (InsideOMPCall != nullptr) {
    LLVM_DEBUG(dbgs() << "\n Target Called at Line:"
                      << MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(
                             CalledFuncLocationMap[InsideOMPCall]));
  }
  FuncEnvMap[&F] = InsideOMPCall;
  // SmallVector<const BasicBlock*, 10> BBVisitQ;
  std::queue<const BasicBlock *> BBVisitQ;
  std::set<const BasicBlock *> BBVisitedSet;
  BBVisitQ.push(&F.getEntryBlock());
  LLVM_DEBUG(dbgs() << "\n AnalyzeFunction : " << F.getName()
                    << " is Target region?" << InsideOMPCall << "\n");
  // Traverse the CFG in BFS
  while (!BBVisitQ.empty()) {
    auto VisitBB = BBVisitQ.front();
    BBVisitQ.pop();
    if (EXISTSinMap(BBVisitedSet, VisitBB))
      continue;
    BBVisitedSet.insert(VisitBB);
    for (auto SuccBB = succ_begin(VisitBB); SuccBB != succ_end(VisitBB);
         SuccBB++) {
      BBVisitQ.push(*SuccBB);
    }
    analyzeBasicBlock(*VisitBB);
  }
  // Once we return from the function that was the target region begin, we are
  // out of the Target environment and back to host environment.
  if (TargetRegBeginCall)
    InsideOMPCall = nullptr;
}

void ValidateOmpReachingDefs::recordOmpMaps() {
  for (auto Iter : OmpInfo.getAllocatedItems()) {
    auto LocSeq = MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(Iter.first);
    for (auto MapTypeIter : Iter.second) {
      const std::string VN = MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(
          MapTypeIter.MappedValue);
      AllocatedOnDeviceMap[LocSeq].insert(VN);
      LLVM_DEBUG(
          dbgs() << "\n Allocated on line:"
                 << MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(LocSeq)
                 << " val:" << MapTypeIter.MappedValue << "= "
                 << *MapTypeIter.MappedValue);
    }
  }
  for (auto Iter : OmpInfo.getHostDeviceCopy()) {
    auto LocSeq = MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(Iter.first);
    for (auto MapTypeIter : Iter.second) {
      const std::string VN = MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(
          MapTypeIter.MappedValue);
      HostToDeviceMap[LocSeq].insert(VN);
      LLVM_DEBUG(
          dbgs() << "\n Host to Device copy on line:"
                 << MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(LocSeq)
                 << ", Seq:" << LocSeq << ",val:" << MapTypeIter.MappedValue
                 << "= " << *MapTypeIter.MappedValue << " \n Val Name: " << VN);
    }
  }
  for (auto Iter : OmpInfo.getDeviceHostCopy()) {
    auto LocSeq = MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(Iter.first);
    for (auto MapTypeIter : Iter.second) {
      const std::string VN = MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(
          MapTypeIter.MappedValue);
      DeviceToHostMap[LocSeq].insert(VN);
      LLVM_DEBUG(
          dbgs() << "\n Device to Host copy on line:"
                 << MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(LocSeq)
                 << " val:"
                 << MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(
                        MapTypeIter.MappedValue)
                 << "= " << *MapTypeIter.MappedValue << " \n Val Name: " << VN);
    }
  }
  for (auto Iter : OmpInfo.getDevicePersistentIn()) {
    auto LocSeq = MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(Iter.first);
    for (auto MapTypeIter : Iter.second) {
      const std::string VN = MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(
          MapTypeIter.MappedValue);
      PersistentInMap[LocSeq].insert(VN);
      LLVM_DEBUG(
          dbgs() << "\n Persistent in on line:"
                 << MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(LocSeq)
                 << " val:" << VN << "= " << *MapTypeIter.MappedValue);
    }
  }
  for (auto Iter : OmpInfo.getDevicePersistentOut()) {
    auto LocSeq = MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(Iter.first);
    for (auto MapTypeIter : Iter.second) {
      const std::string VN = MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(
          MapTypeIter.MappedValue);
      PersistentOutMap[LocSeq].insert(VN);
      LLVM_DEBUG(
          dbgs() << "\n Persistent out on line:"
                 << MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(LocSeq)
                 << " val:" << VN << "= " << *MapTypeIter.MappedValue);
    }
  }
}

void ValidateOmpReachingDefs::analyzeModule(Module &M) {
  const StringRef MainFuncName = "main";
  LLVM_DEBUG(dbgs() << "\n Analysis module:");
  // Find the "main" function, and start analysis from that.
  for (Function &Func : M) {
    if (!Func.hasName() || Func.isIntrinsic() || Func.isDeclaration())
      continue;
    if (Func.getName() == MainFuncName) {
      analyzeFunction(Func);
      break;
    }
  }

  MemUseToReachingDefsMapType MemUseToReachingDefsMap;
  MemInfo.getMemUseToReachingDefsMap(MemUseToReachingDefsMap);
  // MemUseDefAnalysis, provides all the reaching definitions information, the
  // OMP pragmas must respect these use def relations. So, iterate over the
  // reaching defs information.
  for (auto Iter : MemUseToReachingDefsMap) {
    bool UseOnDevice = false;
    if (Iter.second.empty())
      continue;
    auto MemUseInstr = Iter.first;
    auto UseArrayName = MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(
        MemInfo.getMemoryForLdSt(MemUseInstr));
    if (UseArrayName == "")
      continue;
    if (!isa<LoadInst>(MemUseInstr))
      continue;
    auto Ld = dyn_cast<LoadInst>(MemUseInstr);
    if (Ld->getType()->isPointerTy())
      continue;
    auto UseFunc = MemUseInstr->getFunction();
    unsigned UsePragmaOmpSeq = 0;
    // Depending on which function the Use instruction belongs to, we can
    // classify if its executed on device or host.
    if (EXISTSinMap(FuncEnvMap, UseFunc)) {
      if (FuncEnvMap[UseFunc] != nullptr &&
          EXISTSinMap(CalledFuncLocationMap, UseFunc)) {
        // Get the line number of the original parent RTL call, that is the
        // pragma omp line number.
        UsePragmaOmpSeq = CalledFuncLocationMap[FuncEnvMap[UseFunc]];
        LLVM_DEBUG(dbgs() << "\n Use Target env Use Func Called at line :"
                          << MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(
                                 UsePragmaOmpSeq)
                          << " and seq :" << UsePragmaOmpSeq);
        UseOnDevice = true;
      } else {
        LLVM_DEBUG(dbgs() << "Use outside target");
      }
    } else
      continue;
    LLVM_DEBUG(dbgs() << "\n ==== Use:" << *MemUseInstr << " At: "
                      << MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(
                             *MemUseInstr)
                      << " of : " << UseArrayName
                      << ", ontarget ?:" << UseOnDevice << " ==========");
    bool PersistentIn = false, CopyIn = false;
    if (UseOnDevice) {
      if (EXISTSinMap(HostToDeviceMap[UsePragmaOmpSeq], UseArrayName))
        CopyIn = true;
      else if (EXISTSinMap(PersistentInMap[UsePragmaOmpSeq], UseArrayName))
        PersistentIn = true;
    }

    for (auto MemDefInstr : Iter.second) {
      bool DefOnDevice = false;
      if (MemUseInstr == MemDefInstr)
        continue;
      if (!isa<StoreInst>(MemDefInstr))
        continue;
      auto St = dyn_cast<StoreInst>(MemDefInstr);
      if (St->getType()->isPointerTy())
        continue;
      auto DefArrayName = MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(
          MemInfo.getMemoryForLdSt(MemDefInstr));

      LLVM_DEBUG(dbgs() << "\n Def :" << *MemDefInstr << " At: "
                        << MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(
                               *MemDefInstr));
      LLVM_DEBUG(dbgs() << "\n DEf Array::" << DefArrayName);
      // if (isa<CallInst>(MemDefInstr)) continue;
      auto DefFunc = MemDefInstr->getFunction();
      unsigned DefPragmaOmpSeq = 0;

      if (EXISTSinMap(FuncEnvMap, DefFunc)) {
        // LLVM_DEBUG(dbgs()<<"\n ENV ::"<<FuncEnvMap[DefFunc]);
        if (FuncEnvMap[DefFunc] &&
            EXISTSinMap(CalledFuncLocationMap, DefFunc)) {
          DefPragmaOmpSeq = CalledFuncLocationMap[FuncEnvMap[DefFunc]];
          LLVM_DEBUG(dbgs()
                     << "Def Target env DeFunc Called at line :"
                     << MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(
                            DefPragmaOmpSeq)
                     << " and seq: " << DefPragmaOmpSeq);
          DefOnDevice = true;
        } else {
          LLVM_DEBUG(dbgs() << "Def outside target");
        }
        // If both use and def on host or both in the same target region, then
        // nothing to check.
        if ((!UseOnDevice && !DefOnDevice) ||
            (DefPragmaOmpSeq == UsePragmaOmpSeq))
          continue;
        bool PersistentOut = false, CopyOut = false;
        if (DefOnDevice) {
          if (EXISTSinMap(DeviceToHostMap[DefPragmaOmpSeq], DefArrayName))
            CopyOut = true;
          else if (EXISTSinMap(PersistentOutMap[DefPragmaOmpSeq], DefArrayName))
            PersistentOut = true;
        }
        auto DefPragmaLocation =
            MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(DefPragmaOmpSeq);
        auto UsePragmaLocation =
            MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(UsePragmaOmpSeq);
        std::string ErrorMessage = "";
        auto DefLocation =
            MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(*MemDefInstr);
        auto UseLocation =
            MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(*MemUseInstr);
        if (UseOnDevice && (!PersistentIn && !CopyIn)) {
          ErrorMessage =
              " 'to:" + UseArrayName + "' at line: " + UsePragmaLocation;
        }
        if (DefOnDevice && (!PersistentOut && !CopyOut)) {
          ErrorMessage =
              " 'from:" + DefArrayName + "' at line: " + UsePragmaLocation;
        }
        if (!ErrorMessage.empty()) {
          errs() << "\n === Error detected in usage of omp map clauses === ";
          errs() << "\n Definition of variable:'" << DefArrayName
                 << "' on Line: " << DefLocation
                 << " does not reach the use of variable:'" << UseArrayName
                 << "' on Line: " << UseLocation;
          errs() << "\n Possible Fix, Add omp map clause :" << ErrorMessage
                 << "\n\n";

          // OMP COMPOUND EDIT
          BugInfo bug(MemInfo, MemInfo.getMemoryForLdSt(MemDefInstr), MemDefInstr, MemUseInstr, DefArrayName, ErrorMessage);
          bugs.insert(bug);
        }
        if (UseOnDevice && DefOnDevice) {
          // TODO: Check if within same region.
          // If not within same target region, then persistent out and in.
          if (UsePragmaOmpSeq == DefPragmaOmpSeq)
            continue;
          // copy out or persistent out
          if (EXISTSinMap(DeviceToHostMap[DefPragmaOmpSeq], DefArrayName) ||
              EXISTSinMap(DeviceToHostMap[DefPragmaOmpSeq], UseArrayName)) {
            LLVM_DEBUG(dbgs()
                       << "\n exists in device to host copy:" << UseArrayName);
          } else{
            LLVM_DEBUG(dbgs() << "\n Error not copied out" << UseArrayName);
          }
          if (EXISTSinMap(HostToDeviceMap[UsePragmaOmpSeq], DefArrayName) ||
              EXISTSinMap(HostToDeviceMap[UsePragmaOmpSeq], UseArrayName)) {
            LLVM_DEBUG(dbgs() << "\n exists in device to host copy:"
                              << UseArrayName << "\n");
          } else {
            LLVM_DEBUG(dbgs()
                       << "\n Error not copied in:" << UseArrayName << "\n");
          }
        } else if (DefOnDevice && !UseOnDevice) {
          // If defined in target but used on host, then
          // Must be copy out or persistent out from target.
          if (EXISTSinMap(DeviceToHostMap[DefPragmaOmpSeq], DefArrayName) ||
              EXISTSinMap(DeviceToHostMap[DefPragmaOmpSeq], UseArrayName)) {
            LLVM_DEBUG(dbgs()
                       << "\n exists in device to host copy:" << UseArrayName);
          } else {
            LLVM_DEBUG(dbgs() << "\n Error not copied out" << UseArrayName);

            
          }
        } else if (!DefOnDevice && UseOnDevice) {
          // Must be Copy in or persistent in to the target.
          if (EXISTSinMap(HostToDeviceMap[UsePragmaOmpSeq], DefArrayName) ||
              EXISTSinMap(HostToDeviceMap[UsePragmaOmpSeq], UseArrayName)) {
            LLVM_DEBUG(dbgs() << "\n exists in device to host copy:"
                              << UseArrayName << "\n");
          } else {
            LLVM_DEBUG(dbgs()
                       << "\n Error not copied in:" << UseArrayName << "\n");
          }
        }

      } else
        continue;
      //<<"At :"<<OmpDiagnosticsLocationInfo.getVarNameLocStr(MemDefInstr);
    }
  }

  for (auto Iter : HostToDeviceMap) {
    LLVM_DEBUG(dbgs() << "\n Host to Device at line:" << Iter.first << " str: "
                      << MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(
                             Iter.first));
    for (auto V : Iter.second) {
      LLVM_DEBUG(dbgs() << "\n Val:" << V);
    }
  }
  for (auto Iter : DeviceToHostMap) {
    LLVM_DEBUG(dbgs() << "\n Device to host at line:" << Iter.first << " str: "
                      << MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(
                             Iter.first));
    for (auto V : Iter.second) {
      LLVM_DEBUG(dbgs() << "\n Val:" << V);
    }
  }

  // ── LLVM ≥14 fallback: map-type-based bug detection ───────────────────────
  // In LLVM ≥14 the device kernel is compiled into a separate module, so it
  // never appears in the host IR.  The reaching-def analysis above therefore
  // never finds a "def on device / use on host" pair.
  //
  // Fallback: use the data already in OmpInfo.getHostDeviceCopy() (variables
  // mapped to: → device) and OmpInfo.getDeviceHostCopy() (variables mapped
  // from: → host).  Any variable that is mapped to: but NOT from: was written
  // by the kernel but the changes were never returned.  We flag the function
  // containing the begin-mapper call as a suspect for Arbalest to instrument.
  {
    // Collect Value* of all variables that DO have a 'from' mapping anywhere.
    std::set<const Value *> FromMappedVals;
    for (auto &DHEntry : OmpInfo.getDeviceHostCopy())
      for (auto &M : DHEntry.second)
        FromMappedVals.insert(M.MappedValue);

    for (auto &HDEntry : OmpInfo.getHostDeviceCopy()) {
      const Instruction *BeginCall = dyn_cast<Instruction>(HDEntry.first);
      if (!BeginCall)
        continue;

      for (auto &Mapping : HDEntry.second) {
        const Value *V = Mapping.MappedValue;
        if (FromMappedVals.count(V))
          continue; // has a 'from' mapping — not a bug

        std::string VarName =
            MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(V);
        std::string Msg = "'from:" + VarName +
                          "' missing — variable mapped to: device but never "
                          "copied back (detected via map-type analysis)";

        errs() << "\n === OMPSan (map-type) error: " << Msg << "\n";

        // Use BeginCall as both the def (device write) and use (host read)
        // representative instruction so that getIncludeList() resolves to
        // the function containing the pragma.
        bugs.insert(BugInfo(MemInfo, V, BeginCall, BeginCall, VarName, Msg));
      }
    }
  }
}

AnalysisKey OmpSanitizerGlobalAnalysis::Key;

OMPSanInfo OmpSanitizerGlobalAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
  bugs.clear(); // Reset bugs
  
  OMPSanInfo result;
  ThisModule = &M;
  LLVM_DEBUG(dbgs() << "\n Analysis module:");
  AnalysisManager = &AM;
  OmpDiagnosticsInfo OmpInfo = AM.getResult<OmpDiagnosticsGlobalAnalysis>(M);
  auto MemInfo = AM.getResult<MemUseDefGlobalAnalysis>(M);
  MemUseToReachingDefsMapType MemUseToReachingDefsMap;
  MemInfo.getMemUseToReachingDefsMap(MemUseToReachingDefsMap);
  ValidateOmpReachingDefs ValidateObj(MemInfo,
                                      OmpInfo); //, MemUseToReachingDefsMap);
  ValidateObj.analyzeModule(M);
  generateOMPSanReport(); // OMPCOMPOUND
  generateAllowlist(); // OMPCOMPOUND

  result.setBugs(bugs);
  return result;
}

PreservedAnalyses
OmpSanitizerGlobalPrinterPass::run(Module &M, ModuleAnalysisManager &AM) {
  OS << "'omp diagnostics Analysis' for module '" << M.getName() << "'\n";
  AM.getResult<OmpSanitizerGlobalAnalysis>(M);
  // Res.print(OS);
  return PreservedAnalyses::all();
}

static const char LocalPassArg[] = "omp-diagnostics-local";
static const char LocalPassName[] = "omp diagnostics Local Analysis";
static const char GlobalPassName[] = "omp diagnostics Analysis";
