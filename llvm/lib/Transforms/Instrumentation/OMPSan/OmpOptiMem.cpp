//===- OmpOptiMem.cpp - Stack memory safety analysis
//-------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/OMPSan/OmpOptiMem.h"
using namespace llvm;

#define DEBUG_TYPE "omp-mem-optimizer"

// OMP COMPOUND
///////////////////////////////////////////////////
///////////////////////////////////////////////////
///////////////////////////////////////////////////
///////////////////////////////////////////////////
///////////////////////////////////////////////////

#include <fstream>
#include <string>
#include <sstream>

std::stringstream suggestStream;

void generateOMPOptiMemReport(){
  std::ofstream output("/home/mdavis/OMPOptiMemReport.txt");
  if(output.is_open()){
    output << suggestStream.str();
    output.close();
  }
  else{
    //Error
  }
}

///////////////////////////////////////////////////
///////////////////////////////////////////////////
///////////////////////////////////////////////////
///////////////////////////////////////////////////
///////////////////////////////////////////////////

void ClassifyHostDevice::analyzeBasicBlock(const BasicBlock &BB) {
  for (auto &I : BB) {
    if (const CallInst *Call = dyn_cast<CallInst>(&I)) {
      const Function *CalledFunc = Call->getCalledFunction();
      for (auto &A : Call->args()) {
        if (auto F =
            dyn_cast<Function>(A->stripPointerCastsAndAliases())) {
          //LLVM_DEBUG(dbgs() << " Arg ::" << A << ":"
          //    << *(A->stripPointerCastsAndAliases()));
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
        auto TargetBegin  = MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(Call);
        unsigned TargetEnd = 0;
      analyzeFunction(*CalledFunc);
      if (CalledFunc->getName().find(HostOffloadingCallName) != std::string::npos){
        auto SuccHostBB = BB.getUniqueSuccessor();
        // Make sure that we are checking the basic block for offload fail only
        // this BB has exactly 2 instructions, one for call the host seq version and 
        // second the jump to the region outside target.
        if (SuccHostBB != nullptr ) {
          auto FirstI = SuccHostBB->getFirstNonPHIOrDbgOrLifetime();
          TargetEnd = MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(FirstI);
        }
      }
      CalledFuncLocationMap[CalledFunc] = std::make_pair(TargetBegin, TargetEnd);

    }
  }
}
std::string getCallStackStr(const std::vector<const Function *> CallStack){
  std::string CallStackStr = "";
  for (auto F : CallStack){
    CallStackStr =CallStackStr + ";"+std::string(F->getName());
  }
  return CallStackStr;
}

void ClassifyHostDevice::analyzeFunction(const Function &F) {
  //CallStack.push_back(&F);
  //FuncToCallContext[&F].insert(CallStack);
  if (EXISTSinMap(FuncEnvMap, &F)) {
   // auto SetOfContexts = FuncToCallContext[&F];
    //CallStack.pop_back();
    return;
  }
  bool TargetRegBeginCall = false;
  LLVM_DEBUG(dbgs()<<"\n Func::"<<F.getName()<<" InsideOMPCall::"<<InsideOMPCall);
  if (F.getName().find(HostOffloadingCallName) != std::string::npos &&
      InsideOMPCall == nullptr) {
    TargetRegBeginCall = true;
    InsideOMPCall = &F;
  }
  if (InsideOMPCall != nullptr) {
    //LLVM_DEBUG(dbgs() << "\n Target Called at Line:"
    //                  << CalledFuncLocationMap[InsideOMPCall]);
  }
  FuncEnvMap[&F] = InsideOMPCall;
  //LLVM_DEBUG(dbgs() << "\n AnalyzeFunction : " << F.getName()
  //                  << " is Target region?" << InsideOMPCall << "\n");

  // Traverse the CFG in BFS
  for (auto VisitBB : breadth_first(&F.getEntryBlock())){
    analyzeBasicBlock(*VisitBB);
  }
  // Once we return from the function that was the target region begin, we are
  // out of the Target environment and back to host environment.
  if (TargetRegBeginCall)
    InsideOMPCall = nullptr;
  CallStack.pop_back();
}

void ClassifyHostDevice::run(){
  const StringRef MainFuncName = "main";
  LLVM_DEBUG(dbgs() << "\n Analysis module:");
  // Find the "main" function, and start analysis from that.
  for (Function &Func : ThisModule) {
    if (!Func.hasName() || Func.isIntrinsic() || Func.isDeclaration())
      continue;
    if (Func.getName() == MainFuncName) {
      analyzeFunction(Func);
      break;
    }
  }
}

const Function *ClassifyHostDevice::isDevice(const Instruction &I) const{
  auto F = I.getFunction();
  LLVM_DEBUG(dbgs()<<"\n is func device:"<<F->getName());
  if (EXISTSinMap(FuncEnvMap, F) && FuncEnvMap.at(F) != nullptr){
    LLVM_DEBUG(dbgs()<<"\n YES");
    return FuncEnvMap.at(F);
    //return CalledFuncLocationMap[FuncEnvMap[F]];
  }
    LLVM_DEBUG(dbgs()<<"\n NO ");
  return nullptr ;
}

bool FindOptiMem::isInEquivalenceSet(const BasicBlock* BB, const SetOfInstructions &RDefs){
  if (BB == nullptr) return false;
  LLVM_DEBUG(dbgs()<<"\n Checking Eq at :"<<*BB->getFirstNonPHI());
  auto ReachingDefsAtBB = MemInfo.getReachingDefsAt(*BB);
  bool EquivalenceBB = true;
  for (auto Rdef : RDefs){
    LLVM_DEBUG(dbgs()<<"\n Does this exist in the rd of bb:"<<*Rdef);
    if (!EXISTSinMap(ReachingDefsAtBB, Rdef)){
      LLVM_DEBUG(dbgs()<<"\n NO  ");
      EquivalenceBB = false;
      break;
    }
  }
  return EquivalenceBB;
}

unsigned FindOptiMem::findEquivalnceSet(const BasicBlock *UserBB, const SetOfInstructions &ReachingDefsAtUser, const LoopInfo &LI ){
  LLVM_DEBUG(dbgs()<<"\n Find Equivalence set::");
  unsigned OptimalBBLine = getBBLineNum(UserBB);
  if (!isInEquivalenceSet(UserBB, ReachingDefsAtUser))
    return OptimalBBLine;
  std::stack<const BasicBlock*> BBStack;
  std::set<const BasicBlock*> VisitedSet;
  BBStack.push(UserBB);
  unsigned MinLoopDepth = 100;
  while(!BBStack.empty()) {
    auto BB = BBStack.top();
    BBStack.pop();
    if (EXISTSinMap(VisitedSet, BB))
      continue;
    VisitedSet.insert(BB);
    if (!isInEquivalenceSet(BB, ReachingDefsAtUser))
      continue;
    if (LI.getLoopDepth(BB) < MinLoopDepth) {
      MinLoopDepth = LI.getLoopDepth(BB);
      OptimalBBLine = getBBLineNum(BB);
      LLVM_DEBUG(dbgs()<<"\n Loop Depth:"<< MinLoopDepth << "\n At Line::"<< OptimalBBLine);
      if (MinLoopDepth == 0) return OptimalBBLine;
    }
    //errs()<<"\n in Eq Set::"<< *BB;
    for (auto Iter = pred_begin(BB), End = pred_end(BB) ; Iter  != End ; Iter++){
      BBStack.push(*Iter);
    }
  }
  return OptimalBBLine;
}

unsigned FindOptiMem::getBBLineNum(const BasicBlock *BB)const{
  for (const Instruction &I : *BB){
    if (I.getDebugLoc())
      return MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(&I);//.getDebugLoc().getLine();
  }
  LLVM_DEBUG(dbgs()<<"\n BB line not found:\n: ");
  LLVM_DEBUG(BB->dump());
  return 0;
}

unsigned FindOptiMem::searchOptimalBB(BasicBlockListTy &EquivalenceBBQueue){
  unsigned MinLoopDepth = 2000;
  unsigned OptimalBBLine = 0;
  for (auto BB : EquivalenceBBQueue) {
    auto F = BB->getParent();
    auto Iterator = FuncToLoopInfoMap.find(F);
    if (Iterator == FuncToLoopInfoMap.end())
      continue;
    auto LI = Iterator->second;

  LLVM_DEBUG(dbgs()<<"\n Eq Set \n: ");
  LLVM_DEBUG(BB->dump());
    unsigned LDepth = LI->getLoopDepth(BB);
    LLVM_DEBUG(dbgs()<<"\n Loop Depth:"<<LDepth);
    auto BBLine = getBBLineNum(BB);
    if ( LDepth< MinLoopDepth  && BBLine != 0) {
      MinLoopDepth = LDepth;
      OptimalBBLine = BBLine;
      LLVM_DEBUG(dbgs()<<"\n Loop Depth:"<< LDepth << "\n At Line::"<< OptimalBBLine);
    }
  }
  return OptimalBBLine;
}

bool FindOptiMem::filterReachingDefs(const SetOfInstructions &RDefs, SetOfInstructions &FilteredSet, const ClassifyHostDevice &CHD, const Function *OmpFuncUser){
  bool IsDef = false;
  std::map<const Value *, std::set<const BasicBlock*>> VisitedArrayDefs;
  for (const Instruction* DefInstr : RDefs){
    if (DefInstr == nullptr) continue;
    auto DefArray = MemInfo.getMemoryForLdSt(DefInstr);
    if (!VisitedArrayDefs[DefArray].insert(DefInstr->getParent()).second)
      continue;
    auto OmpFuncDef = CHD.isDevice(*DefInstr);
    if ( OmpFuncDef == OmpFuncUser ) continue;
    if ( !IsDef && OmpFuncDef != nullptr)
      IsDef = true;
    LLVM_DEBUG(dbgs()<<"\n Def:"<<*DefInstr);
    //if (DefInstr->getDebugLoc())
    //  DefInstr->getDebugLoc().dump();
    FilteredSet.insert(DefInstr);
  }
  LLVM_DEBUG(dbgs()<<"\n Number of defs::"<<FilteredSet.size());
  return IsDef;
}
bool FindOptiMem::isValidUseInstruction(const Instruction *I){
  if (I == nullptr) 
    return false;
  if (!isa<LoadInst>(I))
    return false;
  auto Ld = dyn_cast<LoadInst>(I);
  if (Ld->getType()->isPointerTy())
    return false;
  auto Array = MemInfo.getMemoryForLdSt(I);
  if (Array == nullptr) 
    return false;
  auto UseArrayName = MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(Array);
  //  errs()<<"\n "<<UseArrayName <<" Type::"<<*Array->getType();
  LLVM_DEBUG(dbgs()<<"\n TYpe:"<<*Array->getType()<<"  Name:"<<UseArrayName);
  //if (auto PtrTy = dyn_cast<PointerType>(Array->getType())){
  //  if (!(PtrTy->getElementType()->isPointerTy() || PtrTy->getElementType()->isArrayTy()))
  //    return false;
  //}
  if (UseArrayName == "" || !UseArrayName.find(".omp.") )
    return false;
  return true;
}
std::string FindOptiMem::getArrayName(const Instruction *I) const {
  auto Array = MemInfo.getMemoryForLdSt(I);
  if (Array == nullptr) return "";
  auto UseArrayName = MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(Array);
  return UseArrayName;
}

unsigned FindOptiMem::getBestEquivalaneceBB(const BasicBlock *UseBB, const Instruction &ReachingDef){
  LLVM_DEBUG(dbgs()<<"\n get best eq::");
  unsigned BestBBLine =  getBBLineNum(UseBB);
  auto Func = UseBB->getParent();
  auto LI = FuncToLoopInfoMap.find(Func);
  if (LI == FuncToLoopInfoMap.end())
    return BestBBLine;
  auto UseBBinLoop = LI->second->getLoopFor(UseBB);
  auto LoopsInPre = LI->second->getLoopsInPreorder();
  if (LoopsInPre.empty())
    return BestBBLine;
  unsigned MinLoopDepth = 100;
  auto BestEqL = LoopsInPre[0];
  for (auto L : LoopsInPre ){
    if (L->getLoopDepth() > MinLoopDepth) continue;
    auto LPredBB = L->getLoopPredecessor();
    LLVM_DEBUG(dbgs()<<"\n Checking Loop:"<<*L->getLocRange().getStart());
    if (EXISTSinMap(MemInfo.getReachingDefsAt(*LPredBB), &ReachingDef)){
      LLVM_DEBUG(dbgs()<<"\n Loop pre header candidate Eq at depth:"<< LI->second->getLoopDepth( LPredBB)<<"\n LIne:"<<*L->getStartLoc());
      LLVM_DEBUG(LPredBB->dump());

      if (MinLoopDepth > LI->second->getLoopDepth( LPredBB)){
        MinLoopDepth = LI->second->getLoopDepth( LPredBB);
        BestEqL = L;
        BestBBLine = MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(L->getStartLoc());//.getLine();

      }
    }
    if (UseBBinLoop == L)
      break;
    SmallVector<BasicBlock*, 5> ExitBBs;
    L->getExitBlocks(ExitBBs);
    auto LSuccBB =ExitBBs[0];
    if (EXISTSinMap(MemInfo.getReachingDefsAt(*LSuccBB), &ReachingDef)){
      LLVM_DEBUG(dbgs()<<"\n Loop pre header candidate Eq at depth:"<< LI->second->getLoopDepth( LSuccBB)<<"\n LIne:"<<*L->getStartLoc());
      LLVM_DEBUG(LSuccBB->dump());

      if (MinLoopDepth > LI->second->getLoopDepth( LSuccBB)){
        MinLoopDepth = LI->second->getLoopDepth( LSuccBB);
        BestEqL = L;
        BestBBLine = MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(L->getLocRange().getEnd());//.getLine()+1;

      }
    }
  }
 // if (BestBBLine == getBBLineNum(UseBB) && LI->second->getLoopDepth(UseBB) != 0){
 //   BestBBLine = findEquivalnceSet( LI->second->getLoopFor(UseBB)->getHeader(), ReachingDefs, *LI->second);
 // }
  if (BestBBLine == getBBLineNum(UseBB)){
    SetOfInstructions RD;
    RD.insert(&ReachingDef);
    BestBBLine = findEquivalnceSet( UseBB,RD , *LI->second);
  }
  return BestBBLine;
}
unsigned FindOptiMem::getBestEquivalaneceBB(const BasicBlock *UseBB, const SetOfInstructions &ReachingDefs){
//  LLVM_DEBUG(dbgs()<<"\n get best eq::");
//  unsigned BestBBLine =  getBBLineNum(UseBB);
//  auto Func = UseBB->getParent();
//  auto LI = FuncToLoopInfoMap.find(Func);
//  if (LI == FuncToLoopInfoMap.end())
//    return BestBBLine;
//  auto LoopsInPre = LI->second->getLoopsInPreorder();
//  if (LoopsInPre.empty())
//    return BestBBLine;
//  unsigned MinLoopDepth = 100;
//  auto BestEqL = LoopsInPre[0];
//  for (auto L : LoopsInPre ){
//    if (L->getLoopDepth() > MinLoopDepth) continue;
//    auto LPredBB = L->getLoopPredecessor();
//    LLVM_DEBUG(dbgs()<<"\n Checking Loop:"<<*L->getLocRange().getStart());
//    if(isInEquivalenceSet( LPredBB, ReachingDefs)){
//      LLVM_DEBUG(dbgs()<<"\n Loop pre header candidate Eq at depth:"<< LI->second->getLoopDepth( LPredBB)<<"\n LIne:"<<*L->getStartLoc());
//      LLVM_DEBUG(LPredBB->dump());
//
//      if (MinLoopDepth > LI->second->getLoopDepth( LPredBB)){
//        MinLoopDepth = LI->second->getLoopDepth( LPredBB);
//        BestEqL = L;
//        BestBBLine = MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(L->getStartLoc());//.getLine();
//
//      }
//    }
//    SmallVector<BasicBlock*, 5> ExitBBs;
//    L->getExitBlocks(ExitBBs);
//    auto LSuccBB =ExitBBs[0];
//    if(isInEquivalenceSet( LSuccBB, ReachingDefs)){
//      LLVM_DEBUG(dbgs()<<"\n Loop pre header candidate Eq at depth:"<< LI->second->getLoopDepth( LSuccBB)<<"\n LIne:"<<*L->getStartLoc());
//      LLVM_DEBUG(LSuccBB->dump());
//
//      if (MinLoopDepth > LI->second->getLoopDepth( LSuccBB)){
//        MinLoopDepth = LI->second->getLoopDepth( LSuccBB);
//        BestEqL = L;
//        BestBBLine = MemInfo.OmpDiagnosticsLocationInfo.getDebugLocSeq(L->getLocRange().getEnd());//.getLine()+1;
//
//      }
//    }
//  }
//  if (BestBBLine == getBBLineNum(UseBB) && LI->second->getLoopDepth(UseBB) != 0){
//    BestBBLine = findEquivalnceSet( LI->second->getLoopFor(UseBB)->getHeader(), ReachingDefs, *LI->second);
//  }
//  if (BestBBLine == getBBLineNum(UseBB)){
//    BestBBLine = findEquivalnceSet( UseBB, ReachingDefs, *LI->second);
//  }
//  return BestBBLine;
  return 0;
}

void FindOptiMem::getCallStack(Function &Func, std::set<std::string> SetOfCallStacks){

  // Get the instructions which can call Func directly or indirectly.
  for (const Use &U : Func.uses()) {
    if (auto C = dyn_cast<CallInst>(U.getUser())){
      //errs()<<"\n call Use:"<<*C;
//      UserBB = C->getParent();
    }
  }
}

const BasicBlock *FindOptiMem::getParentThatGeneratesDef(const Instruction &I, const Instruction &ReachingDef){

  const Function *Func = I.getFunction();
  if (EXISTSinMap(MemInfo.getFuncGeneratingDefs(Func), &ReachingDef)){
      LLVM_DEBUG(dbgs()<<"\n Ancestor::"<<Func->getName()
        <<"\n Has the def:: ");
    return I.getParent();
  }
  for (const Use &U : Func->uses()) {
    //U.getUser()->dump();
    User *UR = U.getUser();
    auto CI = dyn_cast<Instruction>(UR);
    if (CI == nullptr)
      for (auto &UseofUser : UR->uses()) {
        //UseofUser->dump();
        CI = dyn_cast<Instruction>(UseofUser.getUser());
        if (CI != nullptr)  break;
      }
    if (CI == nullptr) continue;
    auto ParentFunc = CI->getFunction();
    LLVM_DEBUG(dbgs()<<"\n Ancestor::"<<ParentFunc->getName());
    //auto ParentFuncGenDefs = MemInfo.getFuncGeneratingDefs(ParentFunc);
    //if (EXISTSinMap(ParentFuncGenDefs, &ReachingDef)){
    //  errs()<<"\n Ancestor::"<<ParentFunc->getName()
    //    <<"\n Has the def:: ";
    //  return CI->getParent();
    //}
    return getParentThatGeneratesDef(*CI, ReachingDef);
  }
  return nullptr;
}

void FindOptiMem::run(){
  ClassifyHostDevice CHD(ThisModule, MemInfo);
  CHD.run();
  std::map<unsigned, std::set<std::string>> LineToPragmaMap;
  std::map<const Value *, std::set<const BasicBlock*>> VisitedArrayUses;


  MemUseToReachingDefsMapType MemUseToReachingDefsMap;
  MemInfo.getMemUseToReachingDefsMap(MemUseToReachingDefsMap);
  for (auto Iter : MemUseToReachingDefsMap){
    auto UseInstr = Iter.first;
    if (!isValidUseInstruction(UseInstr))
      continue;
    auto OrigMemName  = MemInfo.getMemoryForLdSt(UseInstr);
    // If the element was already inserted, then it returns flase, 
    // No need to check, since we already checked the use instr for this bb.
    if (!VisitedArrayUses[OrigMemName].insert(UseInstr->getParent()).second)
      continue;
    bool UseOnDevice = false, DefOnDevice = false;
    const Function *OmpFuncUser = nullptr; 
    OmpFuncUser = CHD.isDevice(*UseInstr);
    if (OmpFuncUser != nullptr )
      UseOnDevice = true;
    SetOfInstructions ReachingDefsAtUser;
    DefOnDevice = filterReachingDefs(Iter.second, ReachingDefsAtUser, CHD, OmpFuncUser);
    if (!UseOnDevice && ! DefOnDevice) continue;
    //const BasicBlock *UserBB = UseInstr->getParent();
    // If user in device, then the userinstr inside a kernel function, so the basic block of the call instruction is required.
    errs()<<"\n Use:"<<*UseInstr << " in Function ::"<< UseInstr->getFunction()->getName();
    errs()<<"\n Line:"<<MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(*UseInstr);
      std::set<const Function *> VisitedFuncs;
      for (auto ReachingDef : ReachingDefsAtUser){
        errs()<<"\n Def::"<<*ReachingDef << " in function:"<<ReachingDef->getFunction()->getName();
        errs()<<"\n Line:"<<MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(*ReachingDef);
        if (EXISTSinMap(VisitedFuncs, ReachingDef->getFunction()))
          continue;
        VisitedFuncs.insert(ReachingDef->getFunction());
        auto ActualUserBB = getParentThatGeneratesDef(*UseInstr, *ReachingDef);
        if (ActualUserBB == nullptr) continue;
        //errs() << "\n userbb:"<< *ActualUserBB;
        unsigned OptimalBBLine =getBestEquivalaneceBB(ActualUserBB, *ReachingDef);
        auto OmpFuncDef = CHD.isDevice(*ReachingDef);
        if (OmpFuncDef == nullptr) 
          DefOnDevice = false;
        else 
          DefOnDevice = true;
        errs()<<"\nStart Pragma =======";
        //errs()<<"\n Optimal::"<<OptimalBBLine;
        errs()<<"\n Line::"<<MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(OptimalBBLine) ;
        auto WrittenArray = MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(ReachingDef);
        errs()<<"\n Written :"<<WrittenArray ;
        auto UsedArray = MemInfo.OmpDiagnosticsLocationInfo.getSymbolName(UseInstr);
        errs()<<"\n Used:"<< UsedArray;
        errs()<<"\n Defondevice:"<<DefOnDevice<<", UseOnDevice:"<<UseOnDevice;
        errs()<<"\n=======";
        if (UseOnDevice && !DefOnDevice){
          auto TargetPos = CHD.getFunctionPos(OmpFuncUser);
          errs() << "\n enter data map(alloc:"<<UsedArray;
          errs() << "\n enter data map(to:"<<  getArrayName(UseInstr);
          //errs()<<"\n #pragma target enter data map(to:";
          if (TargetPos != OptimalBBLine)
          LineToPragmaMap[TargetPos].insert( "data map(alloc:"+UsedArray);
          LineToPragmaMap[OptimalBBLine].insert( "enter data map(to:"+ getArrayName(UseInstr));
        }else if (!UseOnDevice && DefOnDevice){
          errs() << "\n enter data map(alloc:"<<WrittenArray;
          errs() << "\n exit data map(from:"<<  getArrayName(UseInstr);
          auto TargetPos = CHD.getFunctionPos(OmpFuncDef);
          LineToPragmaMap[TargetPos].insert( "data map(alloc:"+WrittenArray);
          LineToPragmaMap[OptimalBBLine].insert( "update map(from:"+ getArrayName(UseInstr));
          //errs()<<"\n Line:"<<TargetPos;
          //errs()<<"\n #pragma target enter data map(alloc:";
          //errs()<<"\n #pragma target exit data map(from:";
        }
        errs()<<"\n End Pragma =======";
        //break;
      }
    continue;
  }
  errs()<<"\n\n";
  for (auto Iter : LineToPragmaMap){
    auto LineNumStr =MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(Iter.first);
    suggestStream << "\n Location: "<<LineNumStr; //OMPCOMPOUND
    errs()<<"\n Line Number::"<<LineNumStr;
    for (auto Pragma : Iter.second){
      errs()<<"\n pragma omp target " <<Pragma;
      suggestStream<<"\n" <<Pragma;//OMPCOMPOUND
    }
  }
}

//void FindOptiMem::run(){
//  ClassifyHostDevice CHD(ThisModule, MemInfo);
//  CHD.run();
//  std::map<unsigned, std::set<std::string>> LineToPragmaMap;
//  std::map<const Value *, std::set<const BasicBlock*>> VisitedArrayUses;
//
//
//  MemUseToReachingDefsMapType MemUseToReachingDefsMap;
//  MemInfo.getMemUseToReachingDefsMap(MemUseToReachingDefsMap);
//  for (auto Iter : MemUseToReachingDefsMap){
//    auto UseInstr = Iter.first;
//    if (!isValidUseInstruction(UseInstr))
//      continue;
//    auto UsedArray = MemInfo.getMemoryForLdSt(UseInstr);
//    // If the element was already inserted, then it returns flase, 
//    // No need to check, since we already checked the use instr for this bb.
//    if (!VisitedArrayUses[UsedArray].insert(UseInstr->getParent()).second)
//      continue;
//    bool UseOnDevice = false, DefOnDevice = false;
//    const Function *OmpFuncUser = nullptr; 
//    OmpFuncUser = CHD.isDevice(*UseInstr);
//    if (OmpFuncUser != nullptr )
//      UseOnDevice = true;
//    SetOfInstructions ReachingDefsAtUser;
//    DefOnDevice = filterReachingDefs(Iter.second, ReachingDefsAtUser, CHD, OmpFuncUser);
//    if (!UseOnDevice && ! DefOnDevice) continue;
//    const BasicBlock *UserBB = UseInstr->getParent();
//    // If user in device, then the userinstr inside a kernel function, so the basic block of the call instruction is required.
//    errs()<<"\n Use:"<<*UseInstr << " in Function ::"<< UseInstr->getFunction()->getName();
//    errs()<<"\n Line:"<<MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(*UseInstr);
//      std::set<const Function *> VisitedFuncs;
//      for (auto ReachingDef : ReachingDefsAtUser){
//        errs()<<"\n Def::"<<*ReachingDef << " in function:"<<ReachingDef->getFunction()->getName();
//        errs()<<"\n Line:"<<MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(*ReachingDef);
//        if (EXISTSinMap(VisitedFuncs, ReachingDef->getFunction()))
//          continue;
//        VisitedFuncs.insert(ReachingDef->getFunction());
//        auto ActualUserBB = getParentThatGeneratesDef(*UseInstr, *ReachingDef);
//        if (ActualUserBB == nullptr) continue;
//        //errs() << "\n userbb:"<< *ActualUserBB;
//        unsigned OptimalBBLine =getBestEquivalaneceBB(ActualUserBB, *ReachingDef);
//        if (UseInstr->getDebugLoc()) 
//          errs()<<"\n Use on line:"<<UseInstr->getDebugLoc().getLine();
//        errs()<<"\n=======";
//        errs()<<"\n Optimal::"<<OptimalBBLine;
//        errs()<<"\n Line::"<<MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(OptimalBBLine) ;
//        //break;
//      }
//    continue;
//    //for (auto Def : ReachingDefsAtUser){
//    //  auto DevFunc = CHD.isDevice(*Def);
//    //  if (DevFunc == nullptr) continue;
//    //  errs()<<"\n Def::"<<*Def << " in function:"<<Def->getFunction()->getName();
//    //  errs()<<"\n Device:"<<DevFunc->getName();
//    //  errs()<<"\n Line:"<<MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(*Def);
//    //}
//    //continue;
//    if (!ReachingDefsAtUser.empty()){
//      if (OmpFuncUser != nullptr )
//        for (const Use &U : OmpFuncUser->uses())
//          if (auto C = dyn_cast<CallInst>(U.getUser())){
//            //errs()<<"\n call Use:"<<*C;
//            UserBB = C->getParent();
//          }
//      unsigned OptimalBBLine =getBestEquivalaneceBB(UserBB, ReachingDefsAtUser);
//      if (UseInstr->getDebugLoc()) 
//        errs()<<"\n Use on line:"<<UseInstr->getDebugLoc().getLine();
//      errs()<<"\n=======";
//      errs()<<"\n Optimal::"<<OptimalBBLine;
//      if (OptimalBBLine == getBBLineNum(UserBB) || OptimalBBLine == 0){
//        // Handling the case when we are not yet able to identify the optimal basic block.
//        if (!UseOnDevice){
//          // Use is on host
//          std::set<const BasicBlock*> VisitedBBs;
//          for (auto Def : ReachingDefsAtUser){
//            const BasicBlock *DefBB = Def->getParent();
//            if (EXISTSinMap(VisitedBBs, DefBB)) continue;
//            VisitedBBs.insert(DefBB);
//            auto DevFunc = CHD.isDevice(*Def);
//            if (DevFunc != nullptr){
//              // Def in device, so exit required
//              OptimalBBLine = CHD.getTargetEndPos(DevFunc)+1;
//              //getParentThatGeneratesDef
//
//              errs()<<"\n Line::"<<MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(OptimalBBLine) ;
//              errs()<<"\n1. #pragma omp target exit data map(from:"<<getArrayName(UseInstr);
//              LineToPragmaMap[OptimalBBLine].insert( "exit data map(from:"+ getArrayName(UseInstr));
//            }
//          }
//        }else if (UseOnDevice && !DefOnDevice) {
//          // host to device copy.
//          OptimalBBLine = CHD.getFunctionPos(OmpFuncUser );
//          errs()<<"\n Line::"<< MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(OptimalBBLine);
//          errs()<<"\n2. #pragma omp target enter data map(to:"<<getArrayName(UseInstr);
//          LineToPragmaMap[OptimalBBLine].insert( "enter data map(from:"+ getArrayName(UseInstr));
//        }
//      } else {
//        errs()<<"\n Line::"<< MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(OptimalBBLine);
//        if (DefOnDevice && !UseOnDevice){
//          // device to host.
//          errs()<<"\n3. #pragma omp target exit data map(from:"<<getArrayName(UseInstr);
//          LineToPragmaMap[OptimalBBLine].insert( "exit data map(from:"+ getArrayName(UseInstr));
//        }
//        else if (UseOnDevice && !DefOnDevice) {
//          LineToPragmaMap[OptimalBBLine].insert("enter data map(to:"+getArrayName(UseInstr));
//          errs()<<"\n4. #pragma omp target enter data map(to:"<<getArrayName(UseInstr);
//        }
//      }
//    } else {
//      auto OptimalBBLine = CHD.getFunctionPos(OmpFuncUser );
//      LineToPragmaMap[OptimalBBLine].insert(
//          "enter data map(alloc:"+getArrayName(UseInstr));
//
//      errs()<<"\n Line::"<< MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(OptimalBBLine);
//      errs()<<"\n5. #pragma omp target enter data map(alloc:"<<getArrayName(UseInstr);
//    }
//    errs()<<"\n=======";
//  }
//  for (auto Iter : LineToPragmaMap){
//    auto LineNumStr =MemInfo.OmpDiagnosticsLocationInfo.getDebugLocStr(Iter.first);
//    errs()<<"\n LIne Number::"<<LineNumStr;
//    for (auto Pragma : Iter.second){
//      errs()<<"\n pragma target " <<Pragma;
//    }
//  }
//}

AnalysisKey OmpOptiMemGlobalAnalysis::Key;
OmpOptiMemGlobalAnalysis::Result
OmpOptiMemGlobalAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
  // OmpDiagnosticsGlobalInfo result;
  ThisModule = &M;
  LLVM_DEBUG(dbgs() << "\n Analysis module:");
  AnalysisManager = &AM;
  FunctionAM =
      &AnalysisManager->getResult<FunctionAnalysisManagerModuleProxy>(M)
           .getManager();
  FuncToLoopInfoTy FuncToLoopInfoMap;
  for (Function &Func : M) {
    if (Func.isDeclaration() || Func.isIntrinsic())
      continue;
    auto &LI = FunctionAM->getResult<LoopAnalysis>(Func);
    FuncToLoopInfoMap[&Func] = &LI;
  }
  OmpDiagnosticsInfo OmpInfo = AM.getResult<OmpDiagnosticsGlobalAnalysis>(M);
  MemoryLdStMapClass memInfo = AM.getResult<MemUseDefGlobalAnalysis>(M);
  MemUseToReachingDefsMapType MemUseToReachingDefsMap;
  memInfo.getMemUseToReachingDefsMap(MemUseToReachingDefsMap);
  FindOptiMem F(M, memInfo, *FunctionAM, FuncToLoopInfoMap );
  F.run();
  generateOMPOptiMemReport(); // OMPCOMPOUND
  return PreservedAnalyses::all();
}

PreservedAnalyses
OmpOptiMemGlobalPrinterPass::run(Module &M, ModuleAnalysisManager &AM) {
  OS << "'omp memory optimizer Analysis' for module '" << M.getName() << "'\n";
  AM.getResult<OmpOptiMemGlobalAnalysis>(M);
  // Res.print(OS);
  return PreservedAnalyses::all();
}

