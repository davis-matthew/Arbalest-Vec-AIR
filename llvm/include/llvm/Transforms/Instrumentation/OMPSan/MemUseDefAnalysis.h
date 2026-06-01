//===- MemUseDefAnalysis.h - Mem Use-Def Analysis based on MemorySSA-------===//
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

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_OMPSAN_MEMUSEDEFANALYSIS_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_OMPSAN_MEMUSEDEFANALYSIS_H

#include "llvm/ADT/BreadthFirstIterator.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/AbstractCallSite.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/IR/Operator.h"

#include <queue>
#include <set>
#include <map>

// TODO: Verify if the analysis converge mechanism works, if we can identify
// when no new defs are added.
namespace llvm {

/// Represents the string type of Omp RTL call names
using ConstCallNameType = const std::string;
using ConstInstrPtr = const Instruction *;
using ConstValuePtr = const Value *;
using SetOfInstructions = std::set<ConstInstrPtr>;
using SetOfValues = std::set<ConstValuePtr>;
using ListOfInstructions = std::vector<ConstInstrPtr>;
using ListOfBasicBlocks = std::vector<const BasicBlock*>;
using SetOfBasicBlocks = std::set<const BasicBlock*>;

#define EXISTSinMap(MAP, ELEM) (MAP.find(ELEM) != MAP.end())

using LocationSequenceNumType = unsigned;
using LineNumberType = unsigned;
using LocationStringType = std::string;
using FileNameType = std::string;
using InstrPointsToMapType = std::map<ConstInstrPtr, ConstValuePtr>;
using MemUseToReachingDefsMapType = std::map<ConstInstrPtr, SetOfInstructions>;

class DebugLocation {
private:
  // std::map<const DebugLoc *, unsigned > DebugLoc2SeqNumMap;
  std::map<const LocationStringType, LocationSequenceNumType>
      DebugLoc2SeqNumMap;
  std::map<const FileNameType, std::set<LineNumberType>> FileLinesMap;
  std::map<const LocationSequenceNumType, std::string> LineNum2VarName;
  std::map<const Value *, std::string> IrToSrcLocMap;

public:
  void printDebugLocation();
  void recordDebugLoc(const LocationStringType &LocStrBegin,
                      const LocationStringType &LocStrEnd,
                      LocationSequenceNumType &BeginSeq,
                      LocationSequenceNumType &EndSeq);

  LocationSequenceNumType recordDebugLoc(LocationStringType &locStr);
  void recordDebugLoc(const Instruction &inst);
  void getFile_Line_fromStr(const LocationStringType &location,
                            FileNameType &fileName, LineNumberType &lineNum,
                            const unsigned seperatorPosition);
  void getFile_Line_fromStr(const LocationStringType &location,
                            FileNameType &fileName, LineNumberType &lineNum);
  bool hasDebugLoc(const Instruction *instr);

  void recordDebugLoc(const DebugLoc &Loc) ;
  LocationSequenceNumType getDebugLocSeq(const Instruction *inst);
  LocationSequenceNumType getDebugLocSeq(const DebugLoc &Loc);
  const std::string getDebugLocStr(const Instruction &inst);
  const std::string getDebugLocStr(const DebugLoc *loc,
                                   unsigned &seperatorPosition);
  const std::string getDebugLocStr(const unsigned seq);
  const std::string getVarNameLocStr(const Instruction *inst);
  bool isPosX_greater_PosY(const unsigned seqX, const unsigned seqY);
  const std::string getOrigVarName(const Value *irVal);
  void handleLLVMir2Srcdebug(const BitCastInst *&dbgI);
  void handleLLVMir2Srcdebug(const DbgValueInst *&dbgI);
  void handleLLVMir2Srcdebug(const GetElementPtrInst *&Gep);
  void handleLLVMir2Srcdebug(const DbgDeclareInst *&dbgI);
  void initFunc(Function &F);
  /// Get the source level name for this Value.
  std::string getSymbolName(const Value *Val);

  /// Get the source level name for the variable this instruction updates.
  std::string getSymbolName(const Instruction *LdSt);

  void setSymbolName(const Instruction *Instr, const Value *Val);
};

class MemoryLdStMapClass {
  /// Map of instruction, to the memory value, that it accesses.
  // std::map<ConstInstrPtr, SetOfValues>;
  using BBtoReachingDefsMapType =
      std::map<const BasicBlock *, SetOfInstructions>;
  using CallInstToUsersMapType = std::map<ConstInstrPtr, SetOfInstructions>;
  using FuncGenDefsType = std::map<const Function *, SetOfInstructions>;
  using BBReachingCallInstrMapType =
      std::map<const BasicBlock *, SetOfInstructions>;

  class FuncParamInfoClass {
    std::map<ConstValuePtr, unsigned> ValueToIdMap;
    unsigned UniqueIdCounter;

  public:
    void setValuesAlias(ConstValuePtr Arg, ConstValuePtr Param);
    FuncParamInfoClass() : UniqueIdCounter(0) {}
    void handleFuncCall(const CallInst &CI, const MemoryLdStMapClass &MemInfo);
    bool doValuesAlias(ConstValuePtr Arg, ConstValuePtr Param) const;
    void print();
  };
  InstrPointsToMapType LdStPointsToMap;
  MemUseToReachingDefsMapType MemUseToReachingDefsMap;
  BBtoReachingDefsMapType BBtoReachingDefsMap;
  FuncGenDefsType FuncGeneratingDefs;
  CallInstToUsersMapType CallInstToUsersMap;
  SetOfInstructions LiveInUses;
  static std::map<ConstInstrPtr, const CallInst *> MemdefToCall;

  /// Record that the Instruction \p LdSt accesses the Memory \p Mem.
  void insertEntry(ConstInstrPtr LdSt, ConstValuePtr Mem);

  /// Keep going up from \p Instr till we find the memory operand, that Instr
  /// accesses, then set \p Mem. Only the limited set of instructions are
  /// currently handled.
  bool backtrackFindMemoryInstr(ConstValuePtr Instr, ConstValuePtr &Mem) const;
  bool filterLoadOfAddress(ConstInstrPtr Ld) const;
  Type *getType(ConstInstrPtr LdSt);

public:
  /// Record the call instructions that reach this basic block. It is updated
  /// whenever a memdef is generated in a basic block that is a call
  /// instruction. And whenever a memdef is added at a phi operator at the join
  /// point.
  BBReachingCallInstrMapType BBReachingCalls;
  static DebugLocation OmpDiagnosticsLocationInfo;
  static FuncParamInfoClass FuncParamInfo;
  void getMemUseToReachingDefsMap(MemUseToReachingDefsMapType &R) const {
    R = MemUseToReachingDefsMap;
  }
  void handleCallArguments(const CallInst &CI);
  /// Check if \p Val is a memory operand.
  bool isMemory(ConstValuePtr Val) const;
  void addReachingCall(ConstInstrPtr User, ConstInstrPtr Call);

  void addLiveOnEntryUse(ConstInstrPtr I);

  SetOfInstructions &getUsersOfCall(const Instruction &Call);
  /// \p CallInstructions is a set of call instructions that reach the user.
  /// This function iterates over the set of call instruction to add the user to
  /// each of the call instruciotns.
  void addReachingCall(Instruction &User, SetOfInstructions &CallInstructions);
  const SetOfInstructions &getFuncGeneratingDefs(const Function *F) const;
  SetOfInstructions &getFuncGeneratingDefs(const Function *F) ;
  bool addFuncGeneratingDefs(SetOfInstructions &ReachingDefs, const BasicBlock *ReachableBlock,
                             const Function *F = nullptr);
  /// Propagate all the defs in the set \p ReachingDefs, to all the BasicBlocks
  /// (within the function).
  bool propagateReachingDefsIntoFunc(SetOfInstructions &ReachingDefs,
                                     const int argNum = -1);

  /// Check if the map has the memory use to reaching defs entry.
  bool existsReachingDefAt(const Instruction &AtInstr);

  /// Get the reaching defs at user \p AtInstr.
  SetOfInstructions &getReachingDefsAt(const Instruction &AtInstr);

  /// Get the reaching defs at entry of basic block \p BB.
  const SetOfInstructions &getReachingDefsAt(const BasicBlock &BB)const;

  /// Check if this instruction belongs to a malloc call in the user code.
  bool isMalloc(ConstInstrPtr LdSt) const;

  /// Return the Memory that this instruction \p LdSt accesses.
  ConstValuePtr getMemoryForLdSt(ConstInstrPtr LdSt) const;
  /// Return the Memory that this instruction \p LdSt accesses.
  ConstValuePtr getMemoryForLdSt(ConstValuePtr LdSt) const;

  /// Get the source level name for the variable this instruction updates.
  void setSymbolName(const Instruction *LdSt);

  /// Find the memory that this instruction accesses, then record it in the map
  /// and also return the memory.
  ConstValuePtr insertLoadStore(ConstInstrPtr LdSt);

  bool areValuesSame(ConstInstrPtr MemDef, ConstValuePtr V);

  /// Add an empty entry for MemUse, that is no memdef inserted yet.
  void insertReachingDef(ConstInstrPtr MemUse);

  /// Add \p MemDef to the set of reaching defs for the instruction \p MemUse,
  /// if SingleReachingDef is true, then update the set to be a single element.
  bool insertReachingDef(ConstInstrPtr MemUse, ConstInstrPtr MemDef,
                         bool SingleReachingDef = false);

  /// Add each def from the set \p MemDefSet to the set of reaching defs for the
  /// instruction \p MemUse, If the CheckAlias is true, then update the reaching
  /// def only if the Memory accessed by user and def are the same. CheckAlias
  /// will be false for some instructions which donot have a single memory
  /// accessed, like call instructions and return instructions.
  bool insertReachingDef(ConstInstrPtr MemUse, SetOfInstructions MemDefSet,
                         bool CheckAlias = true);

  /// Insert the MemDef to the set of reaching defs at the entry of the
  /// basicblock.
  bool insertReachingDef(const BasicBlock *, ConstInstrPtr MemDef);
  bool insertReachingDef(const BasicBlock *, SetOfInstructions &MemDefSet);

  /// Does the instruction load the address of a pointer.
  bool isDoublePointer(ConstInstrPtr Instr);

  //TODO: implement this for direct access into the valueptr -> unsigned ID map
  // unsigned operator[](ConstValuePtr value){
  //   return 
  // }

  void print(raw_ostream &O);
  void print();
};

class MemorySSAUseDefWalker {
public:
  using DefiningMemoryAccessType = const MemoryDef *;
  using MemoryWrittenToType = const Value *;
  using SetOfMemoryDefsType = std::set<DefiningMemoryAccessType>;
  using DefAccessToMemoryType =
      std::map<DefiningMemoryAccessType, MemoryWrittenToType>;
  using MemoryToDefAccessMapType =
      std::map<MemoryWrittenToType, DefiningMemoryAccessType>;
  using BBtoMemoryGenMapType =
      std::map<const BasicBlock *, MemoryToDefAccessMapType>;
  /// Map of memory that is written, to the set of MemDefs that update this
  /// memory.
  using MemToSetofDefsMapType =
      std::map<MemoryWrittenToType, SetOfMemoryDefsType>;
  /// Map of BasicBlock to the Memory that it writes to and the corresponding
  /// MemDefs.
  using BBReachingDefsType =
      std::map<const BasicBlock *, MemToSetofDefsMapType>;
  using BBToGenCallsType = std::map<const BasicBlock *, SetOfInstructions>;
  using MemUseToReachingDefsMapType =
      std::map<const MemoryUse *, SetOfMemoryDefsType>;

private:
  const MemorySSA *MSSA;
  /// This is the Gen set, its a temporary information required for the
  /// analysis. Note, that unlike scalar defs, there can be multiple defs for a
  /// memory inside a basic block, and a later write need not kill a previous
  /// write. But for the purpose of our analysis, we only consider the last
  /// write to a memory, so just like scalars, every basic block can generate a
  /// single def for a memory.

  // std::map<const BasicBlock*, SetOfInstructions > BBGeneratingCalls;

  /// This is the set of reaching defs at the entry of every basic block, final
  /// result of our analysis.
  BBReachingDefsType BBReachingDefs;
  MemoryLdStMapClass &LdStToMem;

  /// Map of a MemoryUse to a set of reaching MemDefs, that clobber the same
  /// memory that the MemoryUse reads. This is the final output of our dataflow
  /// analysis.
  // MemUseToReachingDefsMapType MemUseToReachingDefsMap;

  bool HasConvergedFlagged;

  void getReachingDefs(const BasicBlock *BB, SetOfInstructions &ReachingDefs)const;
  void insertDefs(MemoryToDefAccessMapType &S,
                  const MemoryToDefAccessMapType::iterator &begin,
                  const MemoryToDefAccessMapType::iterator &end);
  void insertDefs(SetOfMemoryDefsType &S,
                  const SetOfMemoryDefsType::iterator &begin,
                  const SetOfMemoryDefsType::iterator &end);

  void initReachingDefs(const BasicBlock *PredBB, const BasicBlock *BB,
                        BBtoMemoryGenMapType &BBGeneratingDefs,
                        BBToGenCallsType &BBGeneratingCalls);
  void initReachingDefs(const BasicBlock *EntryBlock);
  void computeGenDefs(const BasicBlock *BB,
                      BBtoMemoryGenMapType &BBGeneratingDefs,
                      BBToGenCallsType &BBGeneratingCalls);
  void addToGeneratingDefs(DefiningMemoryAccessType memDef,
                           BBtoMemoryGenMapType &BBGeneratingDefs,
                           BBToGenCallsType &BBGeneratingCalls);
  void print();

  /// IncomingBB is the predecessor block of BB, this function updates the
  /// reaching defs of the BB, based on the following equation,
  ///  ReachingDef[BB] = (ReachingDef[IncomingBB] - Kill[IncomingBB]) + Gen[BB]
  /// We can kill defs even though unlike scalars, defs are not killed in
  /// memory, since the Killing def postdominates the reaching def that gets
  /// killed. And here we are only concerned about the edge from a def to a use,
  /// that edge may carry multiple defs. As long as we respect the edge, from
  /// the killing def to the use, it automatically respects the other reaching
  /// def, that got killed. And hence we need not consider all possible defs
  /// through killing defs. This is like an elimination, we are constantly
  /// eliminating mem defs, that donot affect a memuse, we keep going up the mem
  /// ssa. But as soon as we reach a memdef that clobbers the same memory, we
  /// can stop there, and need not consider other clobbering defs reaching this
  /// memdef.
  void updateReachingDefsOfBB(const BasicBlock *IncomingBB,
                              const BasicBlock *BB);

  /// The join operator for the reaching defs at BB. Udpates the reaching defs
  /// of BB, based on its predecessors.
  void reachingDefJoinOp(const BasicBlock *BB);

  /// Keep traversing the reaching definition to this \p MemAccss, until we
  /// reach a Phi Node. This is a recursive function, traversing up reaching
  /// defs, till we find a MemPhi, or the LiveOnEntry Node.
  const MemoryPhi *getParentPhi(const MemoryAccess *MemAccss);

  // Return true if the memory use is live on entry.
  // bool isLiveOnEntry(const MemoryAccess *MemAccss)

  /// Update the reaching definitions for this \p MemUse. Update the record in
  /// "MemUseToReachingDefsMap", which maps every mem use to a set of mem defs.
  void updateClobberingAccess(const MemoryUse *MemUse);

  /// Traverse all the memory accesses within the basic block, then add the defs
  /// to the Gen set of the block, and record the corresponding the reaching
  /// defs for every Mem Use.
  void updateBasicBlock(const BasicBlock *BB);

public:
  MemorySSAUseDefWalker(MemorySSA &M, MemoryLdStMapClass &L)
      : MSSA(&M), LdStToMem(L) {}
  // Entry Point for Reaching definitions data flow analysis. After this
  // function, we have a record of all the reaching definitions for every memory
  // use. Do BFS traversal of the Basic Blocks starting from the LiveOnEntry
  // Access basic block. At the entry of every basic block, perform the
  // following update, ReachingDef[BB] = \Union_{P \in Pred(BB)}
  // ((ReachingDef[P] - Kill[P])+Gen[P]) Then iterate over all the memory uses
  // in the BB, and find all the reaching defs that may clobber the memory being
  // read by the memory use. Following 3 cases can occuer,
  // 1. There is a single MemDef for the memory, that occurs in the same basic
  // block as the use.
  // 2. Among all the reaching defs at the MemPhi, that dominates this MemUse,
  // add all MemDefs that clobber the coresponding memory.
  // 3. Cannot find any reaching defs, analysis failed.
  void reachingDefAnalysis();
};

/// MemUseDefInfo wrapper for the new pass manager.
class MemUseDefLocalAnalysis
    : public AnalysisInfoMixin<MemUseDefLocalAnalysis> {
  friend AnalysisInfoMixin<MemUseDefLocalAnalysis>;
  static AnalysisKey Key;

public:
  using Result = MemoryLdStMapClass;
  Result run(Function &F, FunctionAnalysisManager &AM);
};

/// Printer pass for the \c MemUseDefLocalAnalysis results.
class MemUseDefLocalAnalysisPrinterPass
    : public PassInfoMixin<MemUseDefLocalAnalysisPrinterPass> {
  raw_ostream &OS;

public:
  explicit MemUseDefLocalAnalysisPrinterPass(raw_ostream &OS) : OS(OS) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

// using FuncToMemInfoType = std::map<const Function*, MemoryLdStMapClass*>;
class InterproceduralMemDFA {
  /// Map of function to the intraprocedural reaching defs analysis information.
  // FuncToMemInfoType &FuncToMemInfo;
  MemoryLdStMapClass &LdStToMem;

  void propagateReachingDefsOfCItoBBs(const CallInst &CI);
  /// For all call instructions within the function, propagate the reaching defs
  /// at the function call into the body, and return back the defs generated
  /// within the function body, to be propagated to the blocks following the
  /// call instruction.
  void updateReachingDefsOfBB(BasicBlock &BB, bool &HasConvergedFlagged,
                              std::queue<Function *> &FuncQueue);
  /// Propagate all the reaching defs at the call instruction into the function
  /// body. Return true if any new def was added.
  bool propagateReachingDefsIntoFunc(SetOfInstructions &ReachingDefs,
                                     Function &CalledF, const int argNum = -1);
  SetOfInstructions &getFuncGeneratingDefs(Function &CalledF);

  void getParamNumber(const CallInst &CI, SetOfInstructions &ReachingDefs,
                      MemoryLdStMapClass &LdStToMemFunc);
  void handleCallSite(CallInst &CI);
  bool getCallInstructions(Function &F, std::queue<Function *> &FuncQueue,
                           bool &NewDefsAdded);
  void updateReachingDefsDueToCall(const ListOfInstructions &CallInstructions);
  void handleReachingDefsIntoCall(const CallInst &CallInstr,
                                  bool &NewDefsAdded);
  void handleReachingDefsFromCall(const CallInst &CallInstr,
                                  bool &NewDefsAdded);

public:
  // InterproceduralMemDFA(FuncToMemInfoType &M):FuncToMemInfo(M){}
  InterproceduralMemDFA(MemoryLdStMapClass &M) : LdStToMem(M) {}

  /// Start the interprocedural dataflow analysis from the function \p F.
  void run(Function &F);
};
/// This pass performs the global (interprocedural) Omp Data Mapping Analysis.
/// (New pass manager).
class MemUseDefGlobalAnalysis
    : public AnalysisInfoMixin<MemUseDefGlobalAnalysis> {
  friend AnalysisInfoMixin<MemUseDefGlobalAnalysis>;
  static AnalysisKey Key;

public:
  using Result = MemoryLdStMapClass;

private:
  Module *ThisModule;
  ModuleAnalysisManager *AnalysisManager;
  FunctionAnalysisManager *FunctionAM;

  void analyzeModule(Result &MemUseDefInfo);
  void analyzeFunction(Function &F, Result &MemUseDefInfo);
  void analyzeBasicBlock(const BasicBlock &BB, Result &MemUseDefInfo);
  std::map<std::string, Function *> FuncNameMap;

  void setIndirectCallMap(Function &Func);

public:
  static std::map<const CallInst *, std::map<ConstValuePtr, Argument *>>
      IndirectCallsMap;
  Result run(Module &M, ModuleAnalysisManager &AM);
};

/// Printer pass for the \c MemUseDefGlobalAnalysis results.
class MemUseDefGlobalAnalysisPrinterPass
    : public PassInfoMixin<MemUseDefGlobalAnalysisPrinterPass> {
  raw_ostream &OS;

public:
  using Result = MemoryLdStMapClass;
  explicit MemUseDefGlobalAnalysisPrinterPass(raw_ostream &OS) : OS(OS) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

class MemAccessRangeAnalysis
    : public AnalysisInfoMixin<MemAccessRangeAnalysis> {
  friend AnalysisInfoMixin<MemAccessRangeAnalysis>;
  static AnalysisKey Key;

  GetElementPtrInst *getGEP(Instruction *Ptr);
void handleInstruction(Instruction &Inst, ScalarEvolution &SE);
  void analyzeFunc(Function &F, ScalarEvolution &SE);
public:
  using Result = MemoryLdStMapClass;
  Result run(Function &F, FunctionAnalysisManager &AM);
};

/// Printer pass for the \c MemUseDefLocalAnalysis results.
class MemAccessRangeAnalysisPrinterPass
    : public PassInfoMixin<MemAccessRangeAnalysisPrinterPass> {
  raw_ostream &OS;

public:
  explicit MemAccessRangeAnalysisPrinterPass(raw_ostream &OS) : OS(OS) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
} // end namespace llvm

#endif // LLVM_ANALYSIS_MemUseDefANALYSIS_H