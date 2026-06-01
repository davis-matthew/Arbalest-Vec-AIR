//===- MemUseDefAnalysis.cpp - Mem Use-Def Analysis based on MemorySSA---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/OMPSan/MemUseDefAnalysis.h"

using namespace llvm;

#define DEBUG_TYPE "mem-usedef-analysis"
static StringRef TargetRTLToken = "__tgt_target_";

Function *getCalledFunction(const CallInst &CI) {
  auto CalledF = CI.getCalledFunction();
  if (EXISTSinMap(MemUseDefGlobalAnalysis::IndirectCallsMap, &CI)) {
    // If this call is an indirect call, then get the actual function called.
    auto Iter = MemUseDefGlobalAnalysis::IndirectCallsMap[&CI].begin();
    CalledF = Iter->second->getParent();
  }
  return CalledF;
}
void DebugLocation::printDebugLocation() {
  for (auto Iter : FileLinesMap) {
    LLVM_DEBUG(dbgs() << "\n file:" << Iter.first);
    // errs()<<"\n file:"<<iter.first;
    for (auto Lines : Iter.second) {
      LLVM_DEBUG(dbgs() << ":" << Lines << ",");
      // errs()<<":"<<lines<<",";
    }
  }
}

void DebugLocation::recordDebugLoc(const LocationStringType &LocStrBegin,
                                   const LocationStringType &LocStrEnd,
                                   LocationSequenceNumType &BeginSeq,
                                   LocationSequenceNumType &EndSeq) {
  FileNameType FileNameBegin, FileNameEnd;
  LineNumberType LineBegin = 0, LineEnd = 0;
  LineNumberType BaseLineNumBegin = -1, BaseLineNumEnd = -1;
  getFile_Line_fromStr(LocStrBegin, FileNameBegin, LineBegin);
  getFile_Line_fromStr(LocStrEnd, FileNameEnd, LineEnd);
  auto IterBegin = FileLinesMap.find(FileNameBegin);
  if (IterBegin == FileLinesMap.end()) {
    errs() << "\n ERROR OMP file :" << FileNameBegin
           << "\n Not compiled with non omp";
  }
  auto SetLines = IterBegin->second;
  auto IterGreaterEqual = SetLines.upper_bound(LineBegin);
  if (IterGreaterEqual != SetLines.end()) {
    BaseLineNumBegin = *IterGreaterEqual;
  }
  LLVM_DEBUG(dbgs() << "\n searching for :" << LineBegin
                    << " and Found:" << BaseLineNumBegin);
  LocationStringType baseStrBegin =
      FileNameBegin + ":" + std::to_string(BaseLineNumBegin);
  BeginSeq = DebugLoc2SeqNumMap[baseStrBegin];
  while (IterGreaterEqual != SetLines.end()) {
    if (*IterGreaterEqual >= LineEnd)
      break;
    LLVM_DEBUG(dbgs() << "," << *IterGreaterEqual);
    IterGreaterEqual++;
  }
  if (IterGreaterEqual-- != SetLines.end()) {
    BaseLineNumEnd = *IterGreaterEqual;
  }
  LLVM_DEBUG(dbgs() << "\n searching for :" << LineEnd
                    << " and Found:" << BaseLineNumEnd);
  LocationStringType baseStrEnd =
      FileNameEnd + ":" + std::to_string(BaseLineNumEnd);
  EndSeq = DebugLoc2SeqNumMap[baseStrEnd] + 1;
  // return
}

LocationSequenceNumType
DebugLocation::recordDebugLoc(LocationStringType &locStr) {
  FileNameType fileName;
  LineNumberType line = 0;
  LineNumberType baseLineNum = -1;
  getFile_Line_fromStr(locStr, fileName, line);
  auto iter = FileLinesMap.find(fileName);
  if (iter == FileLinesMap.end()) {
    errs() << "\n ERROR OMP file :" << fileName
           << "\n Not compiled with non omp";
  }
  auto setLines = iter->second;
  auto iterGreaterEqual = setLines.upper_bound(line);
  if (iterGreaterEqual != setLines.end()) {
    baseLineNum = *iterGreaterEqual;
  }
  LLVM_DEBUG(dbgs() << "\n searching for :" << line
                    << " and Found:" << baseLineNum);
  LocationStringType baseStr = fileName + ":" + std::to_string(baseLineNum);
  return DebugLoc2SeqNumMap[baseStr];
}

void DebugLocation::recordDebugLoc(const DebugLoc &Loc) {
  static LocationStringType lastLocationStr = "";
  unsigned seperatorPosition;
  LocationStringType locStr =
      getDebugLocStr(&Loc, seperatorPosition);
  if (DebugLoc2SeqNumMap.find(locStr) != DebugLoc2SeqNumMap.end())
    return; // already added this location to map
  LLVM_DEBUG(dbgs() << "\n position:" << locStr
                    << "\n seq:" << DebugLoc2SeqNumMap.size());
  FileNameType fileName;
  LineNumberType line = 0;
  getFile_Line_fromStr(locStr, fileName, line, seperatorPosition);
  DebugLoc2SeqNumMap[locStr] = DebugLoc2SeqNumMap.size();
  FileLinesMap[fileName].insert(line);
  lastLocationStr = locStr;
  printDebugLocation();
}

void DebugLocation::recordDebugLoc(const Instruction &inst) {
  if (!inst.getDebugLoc())
    return;
  static LocationStringType lastLocationStr = "";
  unsigned seperatorPosition;
  LocationStringType locStr =
      getDebugLocStr(&inst.getDebugLoc(), seperatorPosition);
  if (DebugLoc2SeqNumMap.find(locStr) != DebugLoc2SeqNumMap.end())
    return; // already added this location to map
  LLVM_DEBUG(dbgs() << "\n position:" << locStr
                    << "\n seq:" << DebugLoc2SeqNumMap.size());
  FileNameType fileName;
  LineNumberType line = 0;
  getFile_Line_fromStr(locStr, fileName, line, seperatorPosition);
  DebugLoc2SeqNumMap[locStr] = DebugLoc2SeqNumMap.size();
  FileLinesMap[fileName].insert(line);
  lastLocationStr = locStr;
  printDebugLocation();
}
void DebugLocation::getFile_Line_fromStr(const LocationStringType &location,
                                         FileNameType &fileName,
                                         LineNumberType &lineNum,
                                         const unsigned seperatorPosition) {
  auto sepAt = seperatorPosition;
  fileName = location.substr(0, sepAt);
  // errs()<<"\n 1. filename:"<<fileName;
  // errs()<<"\n 2. linenum:"<<location.substr(sepAt+1, location.size() -
  // sepAt);
  lineNum = std::stoul(location.substr(sepAt + 1, location.size() - sepAt));
}
void DebugLocation::getFile_Line_fromStr(const LocationStringType &location,
                                         FileNameType &fileName,
                                         LineNumberType &lineNum) {
  const std::string seperator = ":";
  auto sepAt = location.find(seperator);
  fileName = location.substr(0, sepAt);
  // errs()<<"\n filename:"<<fileName;
  // errs()<<"\n linenum:"<<location.substr(sepAt+1, location.size() - sepAt);
  lineNum = std::stoul(location.substr(sepAt + 1, location.size() - sepAt));
}
bool DebugLocation::hasDebugLoc(const Instruction *instr) {
  if (instr->getDebugLoc())
    return true;
  return false;
}

LocationSequenceNumType DebugLocation::getDebugLocSeq(const Instruction *inst) {
  LocationSequenceNumType invalidRet = (LocationSequenceNumType)(-1);
  if (!inst->getDebugLoc())
    return invalidRet;
  // const DebugLoc *locStr = &inst->getDebugLoc();
  unsigned dummySep;
  auto locStr = getDebugLocStr(&inst->getDebugLoc(), dummySep);
  auto iter = DebugLoc2SeqNumMap.find(locStr);
  if (iter == DebugLoc2SeqNumMap.end()) {
    // if doesnot exist, then new instruction, insert in map
    recordDebugLoc(*inst);
    iter = DebugLoc2SeqNumMap.find(locStr);
    // return invalidRet ;
  }
  return iter->second;
}

LocationSequenceNumType DebugLocation::getDebugLocSeq(const DebugLoc &Loc) {
//  LocationSequenceNumType invalidRet = (LocationSequenceNumType)(-1);
  // const DebugLoc *locStr = &inst->getDebugLoc();
  unsigned dummySep;
  auto locStr = getDebugLocStr(&Loc, dummySep);
  auto iter = DebugLoc2SeqNumMap.find(locStr);
  if (iter == DebugLoc2SeqNumMap.end()) {
    // if doesnot exist, then new instruction, insert in map
    recordDebugLoc(Loc);
    iter = DebugLoc2SeqNumMap.find(locStr);
    // return invalidRet ;
  }
  return iter->second;
}

const std::string DebugLocation::getDebugLocStr(const Instruction &inst) {
  const std::string invalidRet = ""; //(unsigned)(-1);
  if (!inst.getDebugLoc())
    return invalidRet;
  const DebugLoc &loc = inst.getDebugLoc();
  unsigned dummySep;
  return getDebugLocStr(&loc, dummySep);
}
const std::string DebugLocation::getDebugLocStr(const DebugLoc *loc,
                                                unsigned &seperatorPosition) {
  const std::string invalidRet = ""; //(unsigned)(-1);
  if (loc != nullptr)
    if (auto *scope = dyn_cast<DIScope>(loc->getScope())) {
      auto directory = scope->getDirectory(); //OMPCOMPOUND
      auto filename = scope->getFilename();
      seperatorPosition = directory.size() + 1 + filename.size(); //OMPCOMPOUND
      return directory.str() + "/" + filename.str() + ":" + std::to_string(loc->getLine()); //OMPCOMPOUND
    }
  return invalidRet;
}
const std::string DebugLocation::getDebugLocStr(const unsigned seq) {
  const std::string invalidRet = ""; //(unsigned)(-1);
  for (auto iter : DebugLoc2SeqNumMap) {
    if (iter.second == seq)
      return (iter.first);
  }
  return invalidRet;
}

bool DebugLocation::isPosX_greater_PosY(const unsigned seqX,
                                        const unsigned seqY) {
  if (seqX > seqY)
    return true;
  auto locX = getDebugLocStr(seqX);
  auto locY = getDebugLocStr(seqY);
  LineNumberType lineX, lineY;
  FileNameType fileX, fileY;
  getFile_Line_fromStr(locX, fileX, lineX);
  getFile_Line_fromStr(locY, fileY, lineY);
  LLVM_DEBUG(dbgs() << "\n is :" << lineX << " > " << lineY);
  if (lineX > lineY)
    return true;
  return false;
}

const std::string DebugLocation::getOrigVarName(const Value *irVal) {
  // DIFlagArtificial need to be ignored
  // TODO if global variable !0 = !DIGlobalVariableExpression(var: !1, expr:
  // !DIExpression()) distinct !DIGlobalVariable(name: "p", linkageName:
  // "_ZL1p", scope:
  if (auto Ld = dyn_cast<LoadInst>(irVal)) {
    return getOrigVarName(Ld->getPointerOperand());
  }
  if (auto St = dyn_cast<StoreInst>(irVal)) {
    return getOrigVarName(St->getPointerOperand());
  }
  if (IrToSrcLocMap.find(irVal) != IrToSrcLocMap.end()) {
    return IrToSrcLocMap[irVal];
  }
  if (irVal->hasName()) {
    auto strName = irVal->getName();
    if (strName.find(".casted") != std::string::npos) {
      strName = strName.substr(0, strName.find(".casted"));
    }
    return strName.str();
  }
  if (const Instruction *vI = dyn_cast<Instruction>(irVal)) {
    if (hasDebugLoc(vI) &&
        (LineNum2VarName.find(getDebugLocSeq(vI)) != LineNum2VarName.end())) {
      LLVM_DEBUG(dbgs() << "\n location name found:"
                        << LineNum2VarName[getDebugLocSeq(vI)]);
      return LineNum2VarName[getDebugLocSeq(vI)];
    }
  }
  const Instruction *storedI = dyn_cast<Instruction>(irVal);
  if (storedI == NULL) {
    return "";
  }
  for (unsigned i = 0; i < storedI->getNumOperands(); i++) {
    Value *op = storedI->getOperand(i);
    LLVM_DEBUG(dbgs() << "\n get name for :" << *op);
    auto Vname = getOrigVarName(op);
    if (Vname != "") {
      LLVM_DEBUG(dbgs() << "\n name is  :" << Vname);
      return Vname;
    }
  }
  if (storedI->getNumOperands() != 1)
    return "";
  return storedI->getOperand(0)->getName().str();
}
void DebugLocation::handleLLVMir2Srcdebug(const BitCastInst *&dbgI) {
  // call void @llvm.dbg.declare(metadata i32* %vla, metadata !16, metadata
  // !DIExpression()), !dbg !25 errs()<<"\n mapping :"<<*dbgI->getValue();
  // errs()<<"\n with exp:"<<*dbgI->getExpression();
  const Value *v = dbgI->getOperand(0);
  if (!v->hasName())
    return;
  IrToSrcLocMap[dbgI] = v->getName().str();
  if (hasDebugLoc(dbgI) &&
      (LineNum2VarName.find(getDebugLocSeq(dbgI)) == LineNum2VarName.end())) {
    LLVM_DEBUG(dbgs() << "\n Line:" << dbgI->getDebugLoc().getLine()
                      << "has name :" << v->getName());
    LineNum2VarName[getDebugLocSeq(dbgI)] = v->getName().str();
  }
}
void DebugLocation::handleLLVMir2Srcdebug(const DbgValueInst *&dbgI) {
  // call void @llvm.dbg.declare(metadata i32* %vla, metadata !16, metadata
  // !DIExpression()), !dbg !25 errs()<<"\n mapping :"<<*dbgI->getValue();
  // errs()<<"\n with exp:"<<*dbgI->getExpression();
  LLVM_DEBUG(dbgs() << "\n Line:"
                    << (*dbgI)
                    // dbgI->getDebugLoc().getLine()
                    << "has name :" << dbgI->getVariable()->getName());
  if (dbgI->getValue() != nullptr) {
    IrToSrcLocMap[dbgI->getValue()] = dbgI->getVariable()->getName().str();
  }
  IrToSrcLocMap[dbgI->getVariableLocationOp(0)] = dbgI->getVariable()->getName().str();
  LineNum2VarName[getDebugLocSeq(dbgI)] = dbgI->getVariable()->getName().str();
}
void DebugLocation::handleLLVMir2Srcdebug(const GetElementPtrInst *&Gep) {
  auto Ptr = Gep->getPointerOperand();
  if (IrToSrcLocMap.find(Ptr) != IrToSrcLocMap.end())
    IrToSrcLocMap[Gep] = IrToSrcLocMap[Ptr];
}
void DebugLocation::handleLLVMir2Srcdebug(const DbgDeclareInst *&dbgI) {
  // call void @llvm.dbg.declare(metadata i32* %vla, metadata !16, metadata
  // !DIExpression()), !dbg !25
  LLVM_DEBUG(dbgs() << "\n Location:" << *dbgI
                    << " \n Name:" << dbgI->getVariable()->getName());
  IrToSrcLocMap[dbgI->getVariableLocationOp(0)] = dbgI->getVariable()->getName().str();
}
void DebugLocation::initFunc(Function &F) {
  for (inst_iterator I = inst_begin(&F), E = inst_end(&F); I != E; ++I) {
    const Instruction &Inst = *I;
    recordDebugLoc(Inst);
    if (const BitCastInst *BitI = dyn_cast<BitCastInst>(&Inst)) {
      handleLLVMir2Srcdebug(
          BitI); // hack to record name of orignial source variable
    } else if (const DbgDeclareInst *dbgI = dyn_cast<DbgDeclareInst>(&Inst)) {
      handleLLVMir2Srcdebug(dbgI);
    } else if (const DbgValueInst *dbgI = dyn_cast<DbgValueInst>(&Inst)) {
      handleLLVMir2Srcdebug(dbgI);
    } else if (const GetElementPtrInst *Gep =
                   dyn_cast<GetElementPtrInst>(&Inst)) {
      handleLLVMir2Srcdebug(Gep);
    }
  }
}

DebugLocation MemoryLdStMapClass::OmpDiagnosticsLocationInfo;
const std::string DebugLocation::getVarNameLocStr(const Instruction *Inst) {
  const unsigned seq = getDebugLocSeq(Inst);
  std::string NameLocationStr;
  NameLocationStr = getSymbolName(Inst);
  // getOrigVarName(Inst);//
  NameLocationStr += " At location:" + getDebugLocStr(seq);
  return NameLocationStr;
}

void MemoryLdStMapClass::FuncParamInfoClass::print() {
  for (auto Iter : ValueToIdMap) {
    LLVM_DEBUG(dbgs() << "\n " << *Iter.first << "= " << Iter.second);
    LLVM_DEBUG(dbgs() << "\n " << Iter.first);
  }
}

bool MemoryLdStMapClass::FuncParamInfoClass::doValuesAlias(
    ConstValuePtr Arg, ConstValuePtr Param) const {
  if (Arg == nullptr || Param == nullptr)
    return false;
  LLVM_DEBUG(dbgs() << "\n lookionf for " << *Arg << "," << *Param);
  LLVM_DEBUG(dbgs() << "\n lookionf for " << Arg << "," << Param);
  if (EXISTSinMap(ValueToIdMap, Arg) && EXISTSinMap(ValueToIdMap, Param)) {
    unsigned ArgID = ValueToIdMap.at(Arg);
    unsigned ParamID = ValueToIdMap.at(Param);
    LLVM_DEBUG(dbgs() << "\n DO Alias ?" << ParamID << "==" << ArgID);
    if (ArgID == ParamID)
      return true;
  }
  if (Arg == Param)
    return true;
  return false;
}

void MemoryLdStMapClass::FuncParamInfoClass::handleFuncCall(
    const CallInst &CI, const MemoryLdStMapClass &MemInfo) {
  auto F = CI.getCalledFunction();
  LLVM_DEBUG(dbgs() << "\n handle func alias: " << F->getName()
                    << " CI:" << CI);
  auto IndirectCallIter = MemUseDefGlobalAnalysis::IndirectCallsMap.find(&CI);
  if (IndirectCallIter != MemUseDefGlobalAnalysis::IndirectCallsMap.end()) {
    for (auto Iter : IndirectCallIter->second) {
      auto Arg = Iter.first;
      auto Param = Iter.second;
      setValuesAlias(Arg, Param);
    }
  } else {
    SmallVector<ConstValuePtr, 5> FParams;
    for (auto &A : F->args()) {
      FParams.push_back(&A);
    }
    for (unsigned ArgNum = 0; ArgNum < CI.getNumOperands(); ArgNum++) {
      ConstValuePtr Arg = CI.getArgOperand(ArgNum);
      if (Arg == nullptr)
        continue;
      if (!MemInfo.isMemory(Arg))
        Arg = MemInfo.getMemoryForLdSt((Arg));
      if (Arg == nullptr)
        continue;
      LLVM_DEBUG(dbgs() << "\n Arg :" << *Arg);
      auto Param = FParams[ArgNum];
      if (Param == nullptr)
        continue;
      setValuesAlias(Arg, Param);
    }
  }
}
void MemoryLdStMapClass::FuncParamInfoClass::setValuesAlias(
    ConstValuePtr Arg, ConstValuePtr Param) {
  if (!EXISTSinMap(ValueToIdMap, Arg) && !EXISTSinMap(ValueToIdMap, Param))
    ValueToIdMap[Arg] = ++UniqueIdCounter;

  if (EXISTSinMap(ValueToIdMap, Arg) && EXISTSinMap(ValueToIdMap, Param)) {
    LLVM_DEBUG(dbgs() << "\n ALready Exists Alias::" << *Arg << "="
                      << ValueToIdMap[Arg] << " with \n " << *Param << "="
                      << ValueToIdMap[Param]);
    auto ArgID = ValueToIdMap[Arg];
    auto ParamID = ValueToIdMap[Param];
    // Now, set every value with ParamID to ArgID, so that we know they alias.
    for (auto &Iter : ValueToIdMap) {
      if (Iter.second == ParamID)
        Iter.second = ArgID;
      LLVM_DEBUG(dbgs() << "\n Alias::" << *Iter.first << "="
                        << ValueToIdMap[Iter.first]);
    }
  } else if (EXISTSinMap(ValueToIdMap, Arg))
    ValueToIdMap[Param] = ValueToIdMap[Arg];
  else if
    EXISTSinMap(ValueToIdMap, Param) ValueToIdMap[Arg] = ValueToIdMap[Param];

  LLVM_DEBUG(dbgs() << "\n Alias::" << *Arg << " with \n " << *Param);
}

std::map<ConstInstrPtr, const CallInst *> MemoryLdStMapClass::MemdefToCall;
MemoryLdStMapClass::FuncParamInfoClass MemoryLdStMapClass::FuncParamInfo;

void MemoryLdStMapClass::insertEntry(ConstInstrPtr LdSt, ConstValuePtr Mem) {
  if (LdSt == nullptr || Mem == nullptr)
    return;
  LdStPointsToMap[LdSt] = Mem;
}

bool MemoryLdStMapClass::isMemory(ConstValuePtr Val) const {
  if (isa<GlobalVariable>(Val)) {
    return true;
  }
  if (isa<AllocaInst>(Val)) {
    return true;
  }
  if (isa<Argument>(Val))
    return true;

 // if (auto C = dyn_cast<CallInst>(Val)) {
 //   if (auto F = C->getCalledFunction()){
 //     if (F->hasName() && F->getName().contains("malloc"))
 //       return true;
 //   }
 // }
  return false;
}

void MemoryLdStMapClass::handleCallArguments(const CallInst &CI) {
  FuncParamInfo.handleFuncCall(CI, *this);
}

bool MemoryLdStMapClass::backtrackFindMemoryInstr(ConstValuePtr Instr,
                                                  ConstValuePtr &Mem) const {
  LLVM_DEBUG(dbgs() << "\n get memory for :" << *Instr);
  if (isMemory(Instr)) {
    Mem = Instr;
    return true;
  }
  if (const GetElementPtrInst *Gep = dyn_cast<GetElementPtrInst>(Instr)) {
    auto Ptr = Gep->getPointerOperand();
    return backtrackFindMemoryInstr(Ptr, Mem);
  }
  if (auto Gop = dyn_cast<GEPOperator>(Instr)) {
    return backtrackFindMemoryInstr(Gop->getPointerOperand(), Mem);
  }
  if (const LoadInst *Ld = dyn_cast<LoadInst>(Instr)) {
    auto Ptr = Ld->getPointerOperand();
    return backtrackFindMemoryInstr(Ptr, Mem);
  }
  if (const StoreInst *St = dyn_cast<StoreInst>(Instr)) {
    auto Ptr = St->getPointerOperand();
    return backtrackFindMemoryInstr(Ptr, Mem);
  }
  // if (auto indirectPtr = dyn_cast<Instruction>(instr)){
  //  return backtrackFindMemoryInstr(indirectPtr->getOperand(0) , mem);
  //}
  LLVM_DEBUG(dbgs() << "\n Warning: Memory op for instruction not found"
                    << *Instr);
  // errs()<<"\n Warning: Memory op for instruction not found"<<*Instr;
  Mem = nullptr;
  return false;
}

bool MemoryLdStMapClass::isMalloc(ConstInstrPtr LdSt) const {
  static std::set<LocationSequenceNumType> MallocLines;
  if (auto C = dyn_cast<CallInst>(LdSt)) {
    auto CalledF = C->getCalledFunction();
    if (CalledF != nullptr &&
        CalledF->getName().find("malloc") != std::string::npos) {
      auto Loc = OmpDiagnosticsLocationInfo.getDebugLocSeq(LdSt);
      LLVM_DEBUG(dbgs() << "\n malloc:" << *LdSt << "at:" << Loc);
      MallocLines.insert(Loc);
      return true;
    }
  } else if (MallocLines.find(OmpDiagnosticsLocationInfo.getDebugLocSeq(
                 LdSt)) != MallocLines.end()) {
    auto Loc = OmpDiagnosticsLocationInfo.getDebugLocSeq(LdSt);
    LLVM_DEBUG(dbgs() << "\n malloc:" << *LdSt << "at:" << Loc);
    return true;
  }
  return false;
}

bool MemoryLdStMapClass::filterLoadOfAddress(ConstInstrPtr Ld) const {
  if (auto L = dyn_cast<LoadInst>(Ld)) {
    if (L->getType()->isPointerTy()) {
      auto LoadLineNum = OmpDiagnosticsLocationInfo.getDebugLocSeq(L);
      std::queue<const Value *> UsersQ;
      UsersQ.push(L);
      // Keep scanning the users of this load of pointer, to find if this is used
      // in a function call. We keep scanning till the line numbers are same,
      // that is the load and the call are at the same line.
      while (!UsersQ.empty()) {
        auto Def = UsersQ.front();
        UsersQ.pop();
        for (auto U : Def->users()) {
          if (auto I = dyn_cast<Instruction>(U)) {
            // If the address load is used in the call, then preserve the load
            // as memuse.
            if (isa<CallInst>(I))
              return false;
            // If the user instruction is still on the same line, then add to
            // Queue.
            if (OmpDiagnosticsLocationInfo.getDebugLocSeq(I) == LoadLineNum) {
              UsersQ.push(I);
            }
          }
        }
      }
      // If we could not find a call instr that uses the loaded address, then
      // ignore the mem use. TODO: Other than a store, which other instruction
      // can use the address?
      return true;
    }
  }
  return false;
}

ConstValuePtr MemoryLdStMapClass::getMemoryForLdSt(ConstValuePtr LdSt) const {
  if (LdSt == nullptr)
    return nullptr;
  ConstValuePtr Mem = nullptr;
  if (const auto Gop = dyn_cast<GEPOperator>(LdSt))
    backtrackFindMemoryInstr(Gop->getPointerOperand(), Mem);
  if (auto I = dyn_cast<Instruction>(LdSt))
    Mem = getMemoryForLdSt(I);
  return Mem;
}

ConstValuePtr MemoryLdStMapClass::getMemoryForLdSt(ConstInstrPtr LdSt) const {
  if (LdSt == nullptr || filterLoadOfAddress(LdSt))
    return nullptr;
  LLVM_DEBUG(dbgs() << "\n Looking for memory for instr:" << *LdSt);
  isMalloc(LdSt);
  auto EntryIter = LdStPointsToMap.find(LdSt);
  if (EntryIter == LdStPointsToMap.end()) {
    LLVM_DEBUG(dbgs() << "\n Returning nullptr:");
    return nullptr;
  }
  if (auto Arg = dyn_cast<Argument>(EntryIter->second)) {
    LLVM_DEBUG(dbgs() << "\n Is an argument number :" << Arg->getArgNo());
  }
  LLVM_DEBUG(dbgs() << "\n Returning Mem::" << *EntryIter->second);
  return EntryIter->second;
}

void DebugLocation::setSymbolName(const Instruction *Instr, const Value *Val) {
  LLVM_DEBUG(dbgs() << "\n set name : " << *Instr
                    << " \n from ::" << getSymbolName(Val));
  IrToSrcLocMap[Instr] = getSymbolName(Val);
  // LineNum2VarName[getDebugLocSeq(Instr)] = getSymbolName(Val);
  return;
}
/// Get the source level name for this Value.
std::string DebugLocation::getSymbolName(const Value *Val) {
  if (Val == nullptr)
    return "";
  if (MemoryLdStMapClass::OmpDiagnosticsLocationInfo.IrToSrcLocMap.find(Val) !=
      IrToSrcLocMap.end())
    return IrToSrcLocMap[Val];
  if (Val->hasName()) {
    return Val->getName().str();
  }
  // if (auto I = dyn_cast<Instruction>(Val)){
  //  //auto Seq = OmpDiagnosticsLocationInfo.getDebugLocSeq(I);
  //  //if (LineNum2VarName.find(Seq) != LineNum2VarName.end()) {
  //  //  LLVM_DEBUG(dbgs()<<"\n line num var name:" <<  LineNum2VarName[Seq]);
  //  //  return LineNum2VarName[Seq];
  //  //}
  //}
  return "";
}

std::string DebugLocation::getSymbolName(const Instruction *LdSt) {
  if (LdSt == nullptr)
    return "";
  if (MemoryLdStMapClass::OmpDiagnosticsLocationInfo.IrToSrcLocMap.find(LdSt) !=
      IrToSrcLocMap.end())
    return IrToSrcLocMap[LdSt];
  if (auto StoreI = dyn_cast<StoreInst>(LdSt)) {
    auto Ptr = StoreI->getPointerOperand();
    if (IrToSrcLocMap.find(Ptr) != IrToSrcLocMap.end())
      return IrToSrcLocMap[Ptr];
  }
  if (auto LoadI = dyn_cast<LoadInst>(LdSt)) {
    auto Ptr = LoadI->getPointerOperand();
    if (IrToSrcLocMap.find(Ptr) != IrToSrcLocMap.end())
      return IrToSrcLocMap[Ptr];
  }
  // auto seq = OmpDiagnosticsLocationInfo.getDebugLocSeq(ldst);
  // if (LineNum2VarName.find(seq) != LineNum2VarName.end()){
  //  errs()<<"\n Line:"<<OmpDiagnosticsLocationInfo.getDebugLocStr(seq)
  //    <<" Name:"<<LineNum2VarName[seq];
  //  return LineNum2VarName[seq];
  //}
  return "";
}

void MemoryLdStMapClass::setSymbolName(ConstInstrPtr LdSt) {
  auto SymName =
      MemoryLdStMapClass::OmpDiagnosticsLocationInfo.getSymbolName(LdSt);
  if (SymName != "")
    return;
  if (LdStPointsToMap.find(LdSt) != LdStPointsToMap.end()) {
    return OmpDiagnosticsLocationInfo.setSymbolName(LdSt,
                                                    LdStPointsToMap[LdSt]);
  }
  return;
}

/// Find the memory that this instruction accesses, then record it in the map
/// and also return the memory.
ConstValuePtr MemoryLdStMapClass::insertLoadStore(ConstInstrPtr LdSt) {
  if (LdSt == nullptr)
    return nullptr;
  if (auto C = dyn_cast<CallInst>(LdSt)) {
    for (auto &A : C->args()) {
      insertLoadStore(dyn_cast<Instruction>(&A));
    }
  }
  ConstValuePtr Mem = getMemoryForLdSt(LdSt);
  if (Mem == nullptr) {
    if (backtrackFindMemoryInstr(LdSt, Mem)) {
      insertEntry(LdSt, Mem);
      setSymbolName(LdSt);
    }
  }
  return getMemoryForLdSt(LdSt);
}

void MemoryLdStMapClass::addReachingCall(ConstInstrPtr User,
                                         ConstInstrPtr Call) {
  auto C = dyn_cast<CallInst>(Call);
  auto F = getCalledFunction(*C); // C->getCalledFunction();
  // No need to track intrinsic functions.
  if (F->isIntrinsic() /*|| F->isDeclaration()*/)
    return;
  CallInstToUsersMap[Call].insert(User);
}

void MemoryLdStMapClass::addLiveOnEntryUse(ConstInstrPtr I) {
  LiveInUses.insert(I);
}

void MemoryLdStMapClass::addReachingCall(Instruction &User,
                                         SetOfInstructions &CallInstructions) {
  for (auto Call : CallInstructions) {
    LLVM_DEBUG(dbgs() << "\n User::" << User << "\n Call: " << *Call);
    CallInstToUsersMap[Call].insert(&User);
  }
}
SetOfInstructions &MemoryLdStMapClass::getUsersOfCall(const Instruction &Call) {
  return CallInstToUsersMap[&Call];
}

bool MemoryLdStMapClass::propagateReachingDefsIntoFunc(
    SetOfInstructions &ReachingDefs, const int ArgNum) {
  bool NewDefsAdded = false;
  LLVM_DEBUG(dbgs() << "\n Propagating reaching defs into function::");
  for (auto Def : ReachingDefs) {
    LLVM_DEBUG(dbgs() << "\n def :"
                      << OmpDiagnosticsLocationInfo.getDebugLocStr(*Def));
  }
  // Iterate over all the basic blocks and add  the def to its reaching defs.
  // The reaching defs at the entry to the function reach all the basic blocks
  // also.
  for (auto Iter : BBtoReachingDefsMap) {
    // This loop maynot execute, since the BB maynot exist in the map. So,
    // ignore this loop.
    LLVM_DEBUG(dbgs() << "\n propagate into BB:" << *Iter.first);
    auto BeforeSize = Iter.second.size();
    Iter.second.insert(ReachingDefs.begin(), ReachingDefs.end());
    if (BeforeSize != Iter.second.size())
      NewDefsAdded = true;
  }
  LLVM_DEBUG(dbgs() << "\n NewDefsAdded::" << NewDefsAdded);
  // Now iterate over all the users, and update the reaching defs, if the memory
  // accessed are the same. We donot set the NewDefsAdded here, since the above
  // is a superset of the following loop.
  for (auto Iter : MemUseToReachingDefsMap) {
    auto MemUser = Iter.first;
    bool CheckAlias = true;
    // if (ArgNum != -1 ){
    //  // If the Memuser has an argument as the memory, then check the arg
    //  number and match it with the arg number fo th e memdef then set the
    //  reaching def. auto UsedMem = getMemoryForLdSt(MemUser); if (UsedMem !=
    //  nullptr) if (auto Param = dyn_cast<Argument>(UsedMem)){
    //    int ParamNum = Param->getArgNo();
    //    LLVM_DEBUG(dbgs() << "\n Is an argument number :"<<ParamNum);
    //    if (ParamNum == ArgNum)
    //      CheckAlias = false;
    //  }
    //}
    // Here the Iter.second is empty for the users that have a live on entry
    // reaching def.
    LLVM_DEBUG(dbgs() << "\n Memuser :"
                      << OmpDiagnosticsLocationInfo.getDebugLocStr(*MemUser)
                      << " \n check alias::" << CheckAlias);
    // TODO: Set NewDefsAdded here
    NewDefsAdded |= insertReachingDef(MemUser, ReachingDefs, CheckAlias);
  }
  LLVM_DEBUG(dbgs() << "\n NewDefsAdded::" << NewDefsAdded);
  return NewDefsAdded;
}

bool MemoryLdStMapClass::existsReachingDefAt(const Instruction &AtInstr) {
  return (EXISTSinMap(MemUseToReachingDefsMap, &AtInstr));
}

SetOfInstructions &
MemoryLdStMapClass::getReachingDefsAt(const Instruction &AtInstr) {
  assert(EXISTSinMap(MemUseToReachingDefsMap, &AtInstr));
  return MemUseToReachingDefsMap[&AtInstr];
}

const SetOfInstructions &MemoryLdStMapClass::getReachingDefsAt(const BasicBlock &BB) const{
  assert(EXISTSinMap(BBtoReachingDefsMap, &BB) &&
         "Reaching Defs for basic block does not exit ");
  return BBtoReachingDefsMap.at(&BB);
}

void MemoryLdStMapClass::insertReachingDef(ConstInstrPtr MemUse) {
  MemUseToReachingDefsMap[MemUse] = {};
}

bool MemoryLdStMapClass::insertReachingDef(ConstInstrPtr MemUse,
                                           ConstInstrPtr MemDef,
                                           bool SingleReachingDef) {
  auto BeforeSize = MemUseToReachingDefsMap.size();
  // TOD: Update instructions should not be left out.
  // If use and def same instruction ignore it.
  // if (OmpDiagnosticsLocationInfo.getDebugLocSeq(MemUse) ==
  //    OmpDiagnosticsLocationInfo.getDebugLocSeq(MemDef))
  //  return false;
  if (MemUse == MemDef)
    return false;
  if (auto CI = dyn_cast<CallInst>(MemUse)) {
    MemdefToCall[MemDef] = CI;
  }
  if (SingleReachingDef)
    MemUseToReachingDefsMap[MemUse] = {MemDef};
  else
    MemUseToReachingDefsMap[MemUse].insert(MemDef);
  if (isa<CallInst>(MemDef)) {
    addReachingCall(MemUse, MemDef);
  }

  LLVM_DEBUG(dbgs() << "\n User::"
                    << OmpDiagnosticsLocationInfo.getDebugLocStr(*MemUse)
                    << "\n Def:"
                    << OmpDiagnosticsLocationInfo.getDebugLocStr(*MemDef));
  // If the map changed then return true, else no new element added.
  if (BeforeSize != MemUseToReachingDefsMap.size())
    return true;
  return false;
}

bool MemoryLdStMapClass::areValuesSame(ConstInstrPtr MemDef, ConstValuePtr V) {
  // Check if the Memo written by the MemDef and the value V are the same.
  if (MemDef == nullptr || V == nullptr)
    return false;
  auto WrittenMem = getMemoryForLdSt(MemDef);
  if (WrittenMem == nullptr)
    return false;
  LLVM_DEBUG(dbgs() << "\n Written mem:" << *WrittenMem);
  if (MemDef == V)
    return true;
  if (!EXISTSinMap(MemdefToCall, MemDef))
    return false;
  if (auto Param = dyn_cast<Argument>(V)) {
    auto ParamNum = Param->getArgNo();
    auto CI = MemdefToCall[MemDef];
    LLVM_DEBUG(dbgs() << "\n param number:" << ParamNum
                      << " of value: " << *Param << "\n call:" << *CI);
    unsigned ArgIndex = 0;
    // Search the arguments of the call instruction, to check if the memdef is
    // for one of the arguments, then get the argument number.
    for (auto &Arg : CI->args()) {
      const Value *ArgMem = Arg;
      if (auto ArgI = dyn_cast<Instruction>(Arg))
        ArgMem = getMemoryForLdSt(ArgI);
      LLVM_DEBUG(dbgs() << "\n arg number:" << ArgIndex << " arg:" << *ArgMem);
      if (ArgMem == WrittenMem) {
        if (ParamNum == ArgIndex)
          return true;
        break;
      }
      ArgIndex++;
    }
  }
  return false;
}

bool MemoryLdStMapClass::insertReachingDef(ConstInstrPtr MemUseInstr,
                                           SetOfInstructions MemDefSet,
                                           bool CheckAlias) {
  bool NewElemInserted = false;
  if (auto C = dyn_cast<CallInst>(MemUseInstr)) {
    auto F = getCalledFunction(*C); // C->getCalledFunction();
    if (F->isIntrinsic() /*|| F->isDeclaration()*/)
      return NewElemInserted;
    CheckAlias = false;
  }
  // auto UserLocation = OmpDiagnosticsLocationInfo.getDebugLocSeq(MemUse);
  if (!CheckAlias) {
    for (auto DefInstr : MemDefSet) {
      // If use and def same instruction ignore it.
      // if (UserLocation ==
      // OmpDiagnosticsLocationInfo.getDebugLocSeq(DefInstr)) if (DefInstr ==
      // MemUse)
      //  continue;
      NewElemInserted = insertReachingDef(MemUseInstr, DefInstr);
    }
  } else {
    auto UsedMem = getMemoryForLdSt(MemUseInstr);
    for (auto DefInstr : MemDefSet) {
      // If use and def same instruction ignore it.
      if (DefInstr == MemUseInstr)
        continue;
      // if (UserLocation ==
      // OmpDiagnosticsLocationInfo.getDebugLocSeq(DefInstr))
      //  continue;
      auto WrittenMem = getMemoryForLdSt(DefInstr);
      // if (UsedMem == WrittenMem )
      // if (areValuesSame(DefInstr, UsedMem))
      if (FuncParamInfo.doValuesAlias(UsedMem, WrittenMem))
        NewElemInserted = insertReachingDef(MemUseInstr, DefInstr);
    }
  }
  return NewElemInserted;
}

bool MemoryLdStMapClass::insertReachingDef(const BasicBlock *BB,
    SetOfInstructions &MemDefSet) {
  BBtoReachingDefsMap[BB].insert(MemDefSet.begin(), MemDefSet.end());
  return false;
}
bool MemoryLdStMapClass::insertReachingDef(const BasicBlock *BB,
                                           ConstInstrPtr MemDef) {
  return BBtoReachingDefsMap[BB].insert(MemDef).second;
}

bool MemoryLdStMapClass::isDoublePointer(ConstInstrPtr Instr) {
  if (!isa<LoadInst>(Instr))
    assert(1);
  auto *Ld = dyn_cast<LoadInst>(Instr);
  auto LoadedVal = Ld->getPointerOperand();
  auto PtrType = LoadedVal->getType();
  if (PtrType->isPointerTy()) {
    if (dyn_cast<PointerType>(PtrType)->getArrayElementType()->isPointerTy()) //OMPCOMPOUND??
      return true;
  }
  return false;
}

void MemoryLdStMapClass::print(raw_ostream &O) {
  for (auto Iter : MemUseToReachingDefsMap) {
    auto MemUseInstr = Iter.first;
    if (Iter.second.empty())
      continue;
    if (!EXISTSinMap(LdStPointsToMap, MemUseInstr))
      continue;
    auto PointsToMem = LdStPointsToMap[MemUseInstr];
    O << "\n Use::" << OmpDiagnosticsLocationInfo.getOrigVarName(PointsToMem)
      << " At:: " << OmpDiagnosticsLocationInfo.getDebugLocStr(*MemUseInstr)
      << " Defined at: ";
    for (auto MemDefInstr : Iter.second) {
      if (MemUseInstr == MemDefInstr)
        continue;
      if (isa<CallInst>(MemDefInstr))
        continue;
      O << OmpDiagnosticsLocationInfo.getDebugLocStr(*MemDefInstr) << ", ";
      //<<"At :"<<OmpDiagnosticsLocationInfo.getVarNameLocStr(MemDefInstr);
    }
  }
  for (auto Iter : CallInstToUsersMap) {
    auto Call = Iter.first;
    O << "\n Call :" << *Call;
    for (auto User : Iter.second) {
      O << "::" << *User << ",";
    }
  }
}

const SetOfInstructions &
MemoryLdStMapClass::getFuncGeneratingDefs(const Function *F) const{
  if (EXISTSinMap(FuncGeneratingDefs, F))
    return FuncGeneratingDefs.at(F);
  SetOfInstructions *Temp = new SetOfInstructions;
  return *Temp;
  // TODO Memory Leak
}

SetOfInstructions &
MemoryLdStMapClass::getFuncGeneratingDefs(const Function *F) {
  if (!EXISTSinMap(FuncGeneratingDefs, F)) {
    SetOfInstructions *Temp = new SetOfInstructions;
    FuncGeneratingDefs[F] = *Temp ;
  }
  return FuncGeneratingDefs.at(F);
}
bool MemoryLdStMapClass::addFuncGeneratingDefs(SetOfInstructions &ReachingDefs,const BasicBlock *ReachableBlock,
                                               const Function *F) {
  if (ReachingDefs.empty())
    return false;
  auto PrevSize = FuncGeneratingDefs[F];
  auto I = ReachingDefs.begin();
  if (F == nullptr)
    F = (*I)->getFunction();
  // errs()<<"\n addFuncGeneratingDefs :"<<F->getName();
  SetOfInstructions GlobalDefsOnly;
  for (auto D : ReachingDefs) {
    auto Mem = getMemoryForLdSt(D);
    if (isa<CallInst>(D) ||
        (Mem != nullptr && (isa<Argument>(Mem) || isa<GlobalVariable>(Mem))))
      GlobalDefsOnly.insert(D);
    else
      continue;
    // errs()<<"\n Def:"<<*D;
  }
  FuncGeneratingDefs[F].insert(GlobalDefsOnly.begin(), GlobalDefsOnly.end());
  if (ReachableBlock != nullptr) {
    std::set<const BasicBlock*> Visited;
    std::queue<const BasicBlock*> BBQueue; 
    BBQueue.push(ReachableBlock);

    while (!BBQueue.empty()){
      auto ReachBB = BBQueue.back(); BBQueue.pop();
      if (EXISTSinMap(Visited, ReachBB)) continue;
      Visited.insert(ReachBB);
      for (auto SuccBB =succ_begin(ReachBB) ; SuccBB != succ_end(ReachBB) ; SuccBB++){
        if (EXISTSinMap(Visited, *SuccBB))
          continue;
        BBQueue.push(*SuccBB);
        BBtoReachingDefsMap[*SuccBB].insert(GlobalDefsOnly.begin(), GlobalDefsOnly.end());
      }
    }
  }

  // Return true, if the after size is greater.
  return (PrevSize < FuncGeneratingDefs[F]);
}
Type *MemoryLdStMapClass::getType(ConstInstrPtr LdSt) {
  if (auto St = dyn_cast<StoreInst>(LdSt)) {
    return St->getValueOperand()->getType();
  }
  return LdSt->getType();
}
void MemoryLdStMapClass::print() {
  LLVM_DEBUG(dbgs() << "\n Printing usedef::\n");
  FuncParamInfo.print();
  for (auto Iter : MemUseToReachingDefsMap) {
    auto MemUseInstr = Iter.first;
    if (Iter.second.empty())
      continue;
    if (EXISTSinMap(LdStPointsToMap, MemUseInstr)) {
      auto PointsToMem = LdStPointsToMap[MemUseInstr];
      auto UseType = getType(MemUseInstr);
      LLVM_DEBUG(
          dbgs() << "\n Use::" << *MemUseInstr << " AT::"
                 << OmpDiagnosticsLocationInfo.getOrigVarName(PointsToMem)
                 << " Type:: " << *UseType);
    }

    LLVM_DEBUG(dbgs() << " At:: "
                      << OmpDiagnosticsLocationInfo.getDebugLocStr(*MemUseInstr)
                      << " Defined at: ");
    for (auto MemDefInstr : Iter.second) {
      // if ( MemUseInstr == MemDefInstr ) continue;
      // if (isa<CallInst>(MemDefInstr) || UseType != getType(MemDefInstr))
      // continue; errs() <<
      // OmpDiagnosticsLocationInfo.getDebugLocStr(*MemDefInstr) << ", ";
      LLVM_DEBUG(
          dbgs() << " " << *MemDefInstr << " At"
                 << OmpDiagnosticsLocationInfo.getDebugLocStr(*MemDefInstr)
                 << ", ");
      //<<"At :"<<OmpDiagnosticsLocationInfo.getVarNameLocStr(MemDefInstr);
    }
  }
  for (auto Iter : FuncGeneratingDefs) {
    LLVM_DEBUG(dbgs() << "\n Func generating::" << Iter.first->getName());
    for (auto D : Iter.second)
      LLVM_DEBUG(dbgs() << "\n "
                        << OmpDiagnosticsLocationInfo.getDebugLocStr(*D));
  }
  for (auto Iter : CallInstToUsersMap) {
    auto Call = Iter.first;
    LLVM_DEBUG(dbgs() << "\n Call :" << *Call);
    for (auto User : Iter.second) {
      LLVM_DEBUG(dbgs() << "::" << *User << ",");
    }
  }
}

void MemorySSAUseDefWalker::insertDefs(
    MemoryToDefAccessMapType &S,
    const MemoryToDefAccessMapType::iterator &begin,
    const MemoryToDefAccessMapType::iterator &end) {
  auto beforeInsertSize = S.size();
  S.insert(begin, end);
  LLVM_DEBUG(dbgs() << "\t before:" << beforeInsertSize
                    << " after:" << S.size());
  if (beforeInsertSize != S.size())
    HasConvergedFlagged = false;
}
void MemorySSAUseDefWalker::insertDefs(
    SetOfMemoryDefsType &S, const SetOfMemoryDefsType::iterator &begin,
    const SetOfMemoryDefsType::iterator &end) {
  auto beforeInsertSize = S.size();
  S.insert(begin, end);
  LLVM_DEBUG(dbgs() << "\t before:" << beforeInsertSize
                    << " after:" << S.size());
  if (beforeInsertSize != S.size())
    HasConvergedFlagged = false;
}
void MemorySSAUseDefWalker::addToGeneratingDefs(
    DefiningMemoryAccessType MemDef, BBtoMemoryGenMapType &BBGeneratingDefs,
    BBToGenCallsType &BBGeneratingCalls) {
  // 1. Add the memdef to the generating defs of the function,
  // 2. If this is a function argument, then update the aliasing information,
  // 3. If this is a call instr, then update the generating calls of the func,
  // 4. If this is a store, then get the memory being written and record the gen
  // def of the BB.
  auto Instr = MemDef->getMemoryInst();
  assert(Instr != nullptr);
  const BasicBlock *parentBB = Instr->getParent();
  const Function *parentF = parentBB->getParent();
  SetOfInstructions SingleDef = {MemDef->getMemoryInst()};
  // Every mem def is added to the generating defs of a function, since we
  // assume conservatively no def is killed. No need for iterative convergence
  // analysis here, we can add every memdef here once, and be done with it.
  LdStToMem.addFuncGeneratingDefs(SingleDef,nullptr, parentF);

  // If this mem def is a function call then, record the
  if (CallInst *Call = dyn_cast<CallInst>(MemDef->getMemoryInst())) {
    LLVM_DEBUG(dbgs() << "\n Adding reaching Call instruction::" << *Call);
    // No need to track intrinsic functions, since they cannot generate any
    // defs.
    if (!Call->getCalledFunction()->isIntrinsic())
      BBGeneratingCalls[parentBB].insert(Call);
  }
  auto memVal = LdStToMem.insertLoadStore(Instr);
  if (memVal == nullptr)
    return;
  if (auto St = dyn_cast<StoreInst>(Instr)) {
    auto StoredVal = St->getValueOperand();
    if (isa<Argument>(StoredVal)) {
      LdStToMem.FuncParamInfo.setValuesAlias(StoredVal, memVal);
    }
  }
  assert(memVal != nullptr);
  assert(parentBB != nullptr);
  // bb_defAccess_memory_map[parentBB][memDef] = memVal;//TODO remove this ?
  BBGeneratingDefs[parentBB][memVal] =
      MemDef; // only one entry for every memval, later accesses clobber/kill
  // previous defs
  LLVM_DEBUG(dbgs() << "\n BasicBlock:" << parentBB << "\n Writes to:"
                    << *memVal << "\n by memdef:" << *MemDef);
}

void MemorySSAUseDefWalker::print() {
  LLVM_DEBUG(dbgs() << "\n===============================");
  //  for (auto iter1 : BBGeneratingDefs) {
  //    auto bb = iter1.first;
  //    LLVM_DEBUG(dbgs() << "\n BB:" << bb);
  //    auto mem2access = iter1.second;
  //    for (auto iter2 : mem2access) {
  //      LLVM_DEBUG(dbgs() << "\n Memdef:" << *iter2.second
  //                        << "\n Writes to :" << *iter2.first);
  //    }
  //  }
  for (auto iter1 : BBReachingDefs) {
    auto memPhi = iter1.first; // MSSA->getMemoryAccess(bb);
    if (memPhi == nullptr)
      continue;
    LLVM_DEBUG(dbgs() << "\n Mem Phi:" << *memPhi);
    auto reachingDefs = BBReachingDefs[memPhi];
    for (auto iter3 : reachingDefs) {
      auto writtenToVal = iter3.first;
      LLVM_DEBUG(dbgs() << "\n Write to :" << *writtenToVal);
      auto setOfDefs = iter3.second;
      for (auto memDef : setOfDefs) {
        LLVM_DEBUG(dbgs() << "\n Memdef:" << *memDef);
      }
    }
  }
  LLVM_DEBUG(dbgs() << "\n===============================\n");
}

/// PredBB is the predecessor block of BB, this function updates the reaching
/// defs of the BB, based on the following equation,
//  ReachingDef[BB] = (ReachingDef[PredBB] - Kill[PredBB]) + Gen[BB]
void MemorySSAUseDefWalker::updateReachingDefsOfBB(const BasicBlock *PredBB,
                                                   const BasicBlock *BB) {
  assert(PredBB != nullptr && BB != nullptr);
  LLVM_DEBUG(dbgs() << " \n phi :"
                    << "\n incoming bb:" << *PredBB);

  // Add all the reaching call instructions from the pred to this block also.
  LdStToMem.BBReachingCalls[BB].insert(
      LdStToMem.BBReachingCalls[PredBB].begin(),
      LdStToMem.BBReachingCalls[PredBB].end());

  // All the reaching defs of the predecessor, also reach the BB, unless they
  // are killed. This implements the union operator at a phi node
  if (BBReachingDefs.find(PredBB) != BBReachingDefs.end())
    for (auto PredPhiIter : BBReachingDefs[PredBB]) {
      // forall defs transitively passed by the phis of the incoming block
      auto MemWritten = PredPhiIter.first;
      auto MemDefSet = PredPhiIter.second;
      insertDefs(BBReachingDefs[BB][MemWritten], MemDefSet.begin(),
                 MemDefSet.end());
      for (auto MDef : MemDefSet) {
        LdStToMem.insertReachingDef(BB, MDef->getMemoryInst());
      }
    }
}

/// The join operator for the reaching defs at BB. Udpates the reaching defs of
/// BB, based on its predecessors.
void MemorySSAUseDefWalker::reachingDefJoinOp(const BasicBlock *BB) {
  if (BB == nullptr)
    return;
  MemoryPhi *BBPhi = MSSA->getMemoryAccess(BB);
  if (BBPhi == nullptr) {
    // BBPhi is null means basic block has a single predecessor. Since there is
    // no phi here, we need to make sure to propagate all the reaching defs from
    // its predecessor.
    auto Pred = BB->getSinglePredecessor();
    if (Pred == nullptr)
      return;
    updateReachingDefsOfBB(Pred, BB);
    return;
  }
  // Forall predecessor blocks for the Phi Node, perform the join operator for
  // the reaching defs data flow analysis.
  for (unsigned IncomingIndex = 0;
       IncomingIndex < BBPhi->getNumIncomingValues(); IncomingIndex++) {
    auto IncomingValue = BBPhi->getIncomingValue(IncomingIndex);
    auto IncomingBB = IncomingValue->getBlock();
    updateReachingDefsOfBB(IncomingBB, BB);
  }
  // auto afterUpdateSize = sizeof(BBReachingDefs);
  // LLVM_DEBUG(dbgs()<<"\n Size of :"<<afterUpdateSize);
  // if (afterUpdateSize != beforeUpdateSize) {
  //  HasConvergedFlagged = false;
  //}
}

/// Keep traversing the reaching definition to this \p MemAccss, until we reach
/// a Phi Node. This is a recursive function, traversing up reaching defs, till
/// we find a MemPhi, or the LiveOnEntry Node.
const MemoryPhi *
MemorySSAUseDefWalker::getParentPhi(const MemoryAccess *MemAccss) {
  if (auto MemPhi = dyn_cast<MemoryPhi>(MemAccss))
    return MemPhi;
  if (MSSA->isLiveOnEntryDef(MemAccss))
    return nullptr;
  return (
      getParentPhi(dyn_cast<MemoryUseOrDef>(MemAccss)->getDefiningAccess()));
}

/// Keep traversing the reaching definition to this \p MemAccss, until we reach
/// a Phi Node. This is a recursive function, traversing up reaching defs, till
/// we find a MemPhi, or the LiveOnEntry Node.
// bool
// MemorySSAUseDefWalker::isLiveOnEntry(const MemoryAccess *MemAccss) {
//  if (auto MemPhi = dyn_cast<MemoryPhi>(MemAccss))
//    return MemPhi;
//  if (MSSA->isLiveOnEntryDef(MemAccss))
//    return nullptr;
//  return (
//      getParentPhi(dyn_cast<MemoryUseOrDef>(MemAccss)->getDefiningAccess()));
//}
/// Update the reaching definitions for this \p MemUse. Update the record in
/// "MemUseToReachingDefsMap", which maps every mem use to a set of mem defs.
void MemorySSAUseDefWalker::updateClobberingAccess(const MemoryUse *MemUse) {
  if (MemUse == nullptr)
    return;
  auto DefAccss = MemUse->getDefiningAccess();
  auto UseBB = MemUse->getBlock();
  // auto DefBB = DefAccss->getBlock();
  assert(UseBB != nullptr);
  // auto MemDefsInBB = BBGeneratingDefs[BB];
  auto UseInstr = MemUse->getMemoryInst();
  // Returns the Memory Written to by this instruction.
  auto UsedMem = LdStToMem.insertLoadStore(UseInstr);
  while (DefAccss->getBlock() == UseBB) {
    // Iterate while we are in the same basic block, over all the def accesses
    // withint he basic block to find if any of them write the same memory.
    if (MSSA->isLiveOnEntryDef(DefAccss))
      return;
    if (auto MDef = dyn_cast<MemoryDef>(DefAccss)) {
      auto DefInstr = MDef->getMemoryInst();
      if (!isa<CallInst>(DefInstr))
        if (LdStToMem.areValuesSame(DefInstr, UsedMem)) {
          LdStToMem.insertReachingDef(UseInstr, DefInstr, true);
          break;
        }
      DefAccss = MDef->getDefiningAccess();
    } else
      break;
  }
  // If this memory was defined in BB, then get the last instruction in the BB,
  // that defines this memory, by looking up the Defs generated in the BB.
  // Otherwise, it could be a reaching def, from some previous block, use the
  // phi node to track that. Last case, we give up since we could not find any
  // reaching def for this "WrittenMem".
  // if (MemDefsInBB.find(WrittenMem) != MemDefsInBB.end()) {
  //  // found a write
  //  auto MemDef = MemDefsInBB[WrittenMem];
  //  auto DefinedInstr = MemDef->getMemoryInst();
  //  LdStToMem.insertReachingDef(Instr, DefinedInstr, true);
  //  // OmpInfo.insertReachingDef(Instr, DefinedInstr, WrittenMem, true);
  //  // MemUseToReachingDefsMap[MemUse]={MemDef};//single reaching def
  //  LLVM_DEBUG(dbgs() << "\n Same BB Def Instr :" << *DefinedInstr
  //                    << "\n Use Instr:" << *Instr);
  //}
  if (auto MemPhi = getParentPhi(DefAccss)) {
    // Get all the reaching defs at the Phi node, then find out all the
    // instructions that write to the memory "WrittenMem". These are all the
    // definitions that reach this MemUse.
    auto ReachingDefsAtPhi = BBReachingDefs[MemPhi->getBlock()];
    if (ReachingDefsAtPhi.find(UsedMem) != ReachingDefsAtPhi.end()) {
      auto MemDefSet = ReachingDefsAtPhi[UsedMem];
      for (auto MemDef : MemDefSet) {
        auto DefinedInstr = MemDef->getMemoryInst();
        LdStToMem.insertReachingDef(UseInstr, DefinedInstr, false);
        // MemUseToReachingDefsMap[MemUse].insert(MemDef);
        LLVM_DEBUG(dbgs() << "\n Through PHI Def Instr :" << *DefinedInstr
                          << "\n Use Instr:" << *UseInstr);
      }
    } else {
      // Our analysis failed to find any reaching def for this Instr.
      LLVM_DEBUG(dbgs() << "\n NO DEF FOUND for :" << *UseInstr);
      // Just create an empty entry for the memuse, so that during
      // interprocedural analysis, we can add reaching defs if they are found.
      // Without this, we loose track of the Memory uses.
      LdStToMem.addLiveOnEntryUse(UseInstr);
      LdStToMem.insertReachingDef(UseInstr);
      // errs()<<"\n WARNING: No reaching defs found for this use::"<< *Instr;
    }
  }
}

void MemorySSAUseDefWalker::getReachingDefs(const BasicBlock *BB,
    SetOfInstructions &ReachingDefs) const{
  if (EXISTSinMap(BBReachingDefs, BB))
    for (auto Iter : BBReachingDefs.at(BB)) 
      for (auto Def : Iter.second) 
        ReachingDefs.insert(Def->getMemoryInst());


  // for (auto Def : BBGeneratingDefs[BB]){
  //  ReachingDefs.insert(Def.second->getMemoryInst());
  //}
}

/// PredBB is the predecessor block of BB, this function initializes the
/// reaching defs of the BB,
//  ReachingDef[BB] =  Gen[BB]
void MemorySSAUseDefWalker::initReachingDefs(
    const BasicBlock *PredBB, const BasicBlock *BB,
    BBtoMemoryGenMapType &BBGeneratingDefs,
    BBToGenCallsType &BBGeneratingCalls) {
  assert(PredBB != nullptr && BB != nullptr);
  LLVM_DEBUG(dbgs() << "\n incoming bb:" << *PredBB);

  auto IterGenCalls = BBGeneratingCalls.find(PredBB);
  if (IterGenCalls != BBGeneratingCalls.end())
    LdStToMem.BBReachingCalls[BB].insert(IterGenCalls->second.begin(),
                                         IterGenCalls->second.end());
  // If this PredBB generates any defs, then
  // then add all the genrated defs from predecessor to the reaching defs of the
  // BB. Also, record all the defs killed by the predecessor.
  if (BBGeneratingDefs.find(PredBB) != BBGeneratingDefs.end())
    for (auto MapIter : BBGeneratingDefs[PredBB]) {
      // Forall the defs generated in this PredBB.
      auto MemWritten = MapIter.first;
      auto DefAccss = MapIter.second;
      BBReachingDefs[BB][MemWritten].insert(DefAccss);
      LdStToMem.insertReachingDef(BB, DefAccss->getMemoryInst());
      // If new element inserted, then the analysis has not converged.
      // Keep track of all the defs killed in this BB.
      LLVM_DEBUG(dbgs() << "\n phi BB:"
                        << " \n REaching def:" << *MemWritten
                        << "\n def acces:" << *DefAccss);
    }
}

void MemorySSAUseDefWalker::initReachingDefs(const BasicBlock *EntryBlock) {
  BBtoMemoryGenMapType BBGeneratingDefs;
  BBToGenCallsType BBGeneratingCalls;
  // Traverse all the BBs, and compute the defs generated by the BB.
  for (const BasicBlock *BB : breadth_first(EntryBlock)) {
    // Updates the BBGeneratingDefs and BBGeneratingCalls.
    computeGenDefs(BB, BBGeneratingDefs, BBGeneratingCalls);
  }
  // Now, initialize the reaching defs with the gen defs of its predecessors.
  for (const BasicBlock *BB : breadth_first(EntryBlock)) {
    MemoryPhi *BBPhi = MSSA->getMemoryAccess(BB);
    if (BBPhi == nullptr) {
      // BBPhi is null means basic block has a single predecessor. Since there
      // is no phi here, we need to make sure to propagate all the reaching defs
      // from its predecessor.
      auto Pred = BB->getSinglePredecessor();
      if (Pred != nullptr)
        initReachingDefs(Pred, BB, BBGeneratingDefs, BBGeneratingCalls);
      continue;
    }
    // Forall predecessor blocks for the Phi Node, perform the join operator for
    // the reaching defs data flow analysis.
    for (unsigned IncomingIndex = 0;
         IncomingIndex < BBPhi->getNumIncomingValues(); IncomingIndex++) {
      auto IncomingValue = BBPhi->getIncomingValue(IncomingIndex);
      auto IncomingBB = IncomingValue->getBlock();
      initReachingDefs(IncomingBB, BB, BBGeneratingDefs, BBGeneratingCalls);
    }
  }
}

void MemorySSAUseDefWalker::computeGenDefs(
    const BasicBlock *BB, BBtoMemoryGenMapType &BBGeneratingDefs,
    BBToGenCallsType &BBGeneratingCalls) {
  // For interprocedural analysis, the reaching defs at the return instruction
  // need to be tracked. Since it will be propagated back to the calling
  // function. So, find out if there is a return instruction in this basic
  // block, and compute the reaching defs for the return instruction. List of
  // all memory accesses within the BB.
  auto MAList = MSSA->getBlockAccesses(BB);
  bool ReturnBlock = false;
  // Is there a return instruction at the end of this BB, then the reaching defs
  // at the return instruction is basically the generated defs of the entire
  // Function.
  if (isa<ReturnInst>(BB->getTerminator()))
    ReturnBlock = true;
  if (MAList == nullptr && !ReturnBlock)
    return; // No Memory access in this BB;
  if (MAList != nullptr)
    for (auto &MA : *MAList) {
      Instruction *MemInstr = nullptr;
      if (auto MemDef = dyn_cast<MemoryDef>(&MA)) {
        MemInstr = MemDef->getMemoryInst();
        if (MemInstr == nullptr)
          continue;
        if (isa<CallInst>(MemInstr) &&
            (dyn_cast<CallInst>(MemInstr))->getCalledFunction()->isIntrinsic())
          continue;
        LdStToMem.insertLoadStore(MemInstr);
        LLVM_DEBUG(dbgs() << "\n Memdef:" << *MemDef);
        addToGeneratingDefs(MemDef, BBGeneratingDefs, BBGeneratingCalls);
      }
    }
}

void MemorySSAUseDefWalker::updateBasicBlock(const BasicBlock *BB) {
  // For interprocedural analysis, the reaching defs at the return instruction
  // need to be tracked. Since it will be propagated back to the calling
  // function. So, find out if there is a return instruction in this basic
  // block, and compute the reaching defs for the return instruction.
  SetOfInstructions ReachingDefsATBB, ReachingCalls;
  getReachingDefs(BB, ReachingDefsATBB);
  LdStToMem.insertReachingDef(BB, ReachingDefsATBB);
  auto MAList = MSSA->getBlockAccesses(BB);
  if (MAList == nullptr)
    return; // no Memory access in this BB;
  for (auto &MA : *MAList) {
    // Assumption is the order of iterating over the list is the execution
    // order.
    Instruction *MemInstr = nullptr;
    if (MSSA->isLiveOnEntryDef(&MA))
      continue;
    if (auto MemDef = dyn_cast<MemoryDef>(&MA)) {
      MemInstr = MemDef->getMemoryInst();
      if (isa<CallInst>(MemInstr) &&
          (dyn_cast<CallInst>(MemInstr))->getCalledFunction()->isIntrinsic())
        continue;
      LLVM_DEBUG(dbgs() << "\n Memdef:" << *MemDef);
      if (isa<CallInst>(MemInstr)) {
        LdStToMem.insertReachingDef(MemInstr, ReachingDefsATBB,
                                    /*Donot check for aliasing, since the memory
                                       written is not known for call inst.*/
                                    false);
        ReachingCalls.insert(MemInstr);
      }
      ReachingDefsATBB.insert(MemInstr);
    } else if (auto MemUse = dyn_cast<MemoryUse>(&MA)) {
      MemInstr = MemUse->getMemoryInst();
      LLVM_DEBUG(dbgs() << "\n Memuse:" << *MemUse);
      auto UserInstr = MemUse->getMemoryInst();
      if (isa<CallInst>(UserInstr)) {
        if ((dyn_cast<CallInst>(UserInstr))->getCalledFunction()->isIntrinsic())
          continue;
        ReachingCalls.insert(MemInstr);
        LdStToMem.insertReachingDef(UserInstr, ReachingDefsATBB,
                                    /*Donot check for aliasing, since the memory
                                       written is not known for call inst.*/
                                    false);
      }
      updateClobberingAccess(MemUse);
    }
    if (MemInstr != nullptr && EXISTSinMap(LdStToMem.BBReachingCalls, BB)) {
      LLVM_DEBUG(dbgs() << "\n user::" << *MemInstr);
      // If there are call instructions that reach the BB, then record it.
      LdStToMem.addReachingCall(*MemInstr, LdStToMem.BBReachingCalls[BB]);
    }
  }
}
// Entry Point for Reaching definitions data flow analysis. After this function,
// we have a record of all the reaching definitions for every memory use. Do BFS
// traversal of the Basic Blocks starting from the LiveOnEntry Access basic
// block. At the entry of every basic block, perform the following update,
// ReachingDef[BB] = \Union_{P \in Pred(BB)} ((ReachingDef[P] - Kill[P])+Gen[P])
// Then iterate over all the memory uses in the BB, and find all the reaching
// defs that may clobber the memory being read by the memory use. Following 3
// cases can occuer,
// 1. There is a single MemDef for the memory, that occurs in the same basic
// block as the use.
// 2. Among all the reaching defs at the MemPhi, that dominates this MemUse, add
// all MemDefs that clobber the coresponding memory.
// 3. Cannot find any reaching defs, analysis failed.
void MemorySSAUseDefWalker::reachingDefAnalysis() {
  // Get the first memory def of the function
  MemoryAccess *LiveOnEntry = MSSA->getLiveOnEntryDef();
  assert(LiveOnEntry != nullptr &&
         "Memory SSA does Must have LiveOnEntry Definition ");
  const BasicBlock *EntryBlock = LiveOnEntry->getBlock();
  initReachingDefs(EntryBlock);
  assert(EntryBlock != nullptr && "The entry block cannot be Null ");
  unsigned IterationCount = 0;
  // This do while loop, runs until the analysis converges, that is no new
  // reaching defs are added.
  do {
    HasConvergedFlagged = true;
    for (const BasicBlock *BB : breadth_first(EntryBlock)) {
      // Perform the join operator at the entry of this basicblock.
      reachingDefJoinOp(BB);
    }
    LLVM_DEBUG(dbgs() << "\n Analysis Iteration " << IterationCount);
    IterationCount++;
  } while (!HasConvergedFlagged && IterationCount < 100);
  // Now the reaching defs at the entry of every basic block has converged. Next
  // we look at all the mem uses and associate all the defs that reach the use.

  for (const BasicBlock *BB : breadth_first(EntryBlock)) {
    // Perform the join operator at the entry of this basicblock.
    updateBasicBlock(BB);
  }
  LLVM_DEBUG(dbgs() << "\n Printing Def Use Chains:\n");
  LdStToMem.print();
  // for (auto Iter : MemUseToReachingDefsMap){
  //  auto MemUse = Iter.first;
  //  auto UseInstr = MemUse->getMemoryInst();
  //  auto LoadedValue = LdStToMem.getMemoryForLdSt(UseInstr);
  //  if (LdStToMem.isDoublePointer(UseInstr))
  //    continue;
  //  LLVM_DEBUG(dbgs()<<"\n Use :"<<*UseInstr
  //      <<"At :"<<OmpDiagnosticsLocationInfo.getDebugLocStr(*UseInstr)
  //      <<" Loaded Val:"<<*LoadedValue);
  //  for (auto MemDef : Iter.second) {
  //    auto instr = MemDef->getMemoryInst();
  //    LLVM_DEBUG(dbgs()<<"\n Def :"<<*instr
  //        <<"At :"<<OmpDiagnosticsLocationInfo.getDebugLocStr(*instr));
  //  }
  //}
}

AnalysisKey MemUseDefLocalAnalysis::Key;

MemUseDefLocalAnalysis::Result
MemUseDefLocalAnalysis::run(Function &F, FunctionAnalysisManager &AM) {
  MemorySSA &MSSA = AM.getResult<MemorySSAAnalysis>(F).getMSSA();
  LLVM_DEBUG(MSSA.dump());
  MemoryLdStMapClass::OmpDiagnosticsLocationInfo.initFunc(F);
  MemoryLdStMapClass LdStToMem;
  MemorySSAUseDefWalker useDefWalker(MSSA, LdStToMem);
  useDefWalker.reachingDefAnalysis();
  return LdStToMem;
}

PreservedAnalyses
MemUseDefLocalAnalysisPrinterPass::run(Function &F,
                                       FunctionAnalysisManager &AM) {
  OS << "\n 'omp diagnostics Local Analysis' for function '" << F.getName()
     << "'\n";
  AM.getResult<MemUseDefLocalAnalysis>(F).print(OS);
  return PreservedAnalyses::all();
}

void MemUseDefGlobalAnalysis::setIndirectCallMap(Function &Func) {
  // Iterate over the instructions that use the address of the Func, that means
  // they may call this function. This is used to track indirect calls, and
  // their parameter mappings.
  for (Use &U : Func.uses()) {
    User *UR = U.getUser();
    for (auto &UseofUser : UR->uses()) {
      if (auto IndirectCall = dyn_cast<CallInst>(UseofUser.getUser())) {
        LLVM_DEBUG(dbgs() << "u of u ::" << *IndirectCall
                          << "\n op number: " << UseofUser.getOperandNo()
                          << "\n parent op number: " << U.getOperandNo());

        SmallVector<Argument *, 5> FParams;
        for (auto &A : Func.args()) {
          FParams.push_back(&A);
        }
        for (unsigned ArgNum = IndirectCall->getNumOperands() - 1,
                      ParamNum = FParams.size() - 1;
             ArgNum > UseofUser.getOperandNo(); ArgNum--, ParamNum--) {
          auto Arg = IndirectCall->getArgOperand(ArgNum);
          auto Param = FParams[ParamNum];
          LLVM_DEBUG(dbgs() << "\n Arg::" << *Arg << "\n Param:: " << *Param);
          IndirectCallsMap[IndirectCall][Arg] = Param;
          if (ParamNum == 0)
            break;
        }
      }
    }
    LLVM_DEBUG(dbgs() << "\n user:" << *UR << "\n use::" << *U.get());
    // Ignore blockaddress uses.
    // if (isa<BlockAddress>(UR)) continue;
    // If no abstract call site was created we did not understand the use, bail.
    AbstractCallSite ACS(&U);
    if (!ACS)
      continue;
    LLVM_DEBUG(dbgs() << "\n ACS is indirect:: " << ACS.isIndirectCall()
                      << " is callback: " << ACS.isCallbackCall()
                      << " num ops: " << ACS.getNumArgOperands() << "\n ACS: ");
  }
}
AnalysisKey MemUseDefGlobalAnalysis::Key;

std::map<const CallInst *, std::map<ConstValuePtr, Argument *>>
    MemUseDefGlobalAnalysis::IndirectCallsMap;
void MemUseDefGlobalAnalysis::analyzeModule(Result &MemUseDefInfo) {
  LLVM_DEBUG(dbgs() << "\n Analysis module:");
  // FunctionAnalysisManager &FAM =
  //    AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  // FuncToMemInfoType FuncToMemInfo;
  // Forall Functions in the module.
  for (Function &Func : *ThisModule) {
    LLVM_DEBUG(dbgs() << "\n Func:" << Func.getName());
    if (!Func.hasName() || Func.isIntrinsic() || Func.isDeclaration())
      continue;
    // Record all the users of this function, to keep track of indirect function
    // calls.
    setIndirectCallMap(Func);
    LLVM_DEBUG(dbgs() << "\n Function Map::" << Func.getName());
    FuncNameMap[Func.getName().str()] = &Func;
    // Intraprocedural mem-dataflow analysis, after this we have the record of
    // all the definitions reaching each basic block in the Func.
    analyzeFunction(Func, MemUseDefInfo);
    MemUseDefInfo.print();
    // FuncToMemInfo[&Func] = &MemUseDefInfo;
    // FuncToMemInfo[&Func] =
    // &FunctionAM->getResult<MemUseDefLocalAnalysis>(Func);
    // FuncToMemInfo[&Func]->print();
  }
  LLVM_DEBUG(dbgs() << "\n Printing done:");
  InterproceduralMemDFA IPA(MemUseDefInfo);
  const std::string MainFuncName = "main";
  if (EXISTSinMap(FuncNameMap, MainFuncName)) {
    IPA.run(*FuncNameMap[MainFuncName]);
  }
  // for (auto Iter : FuncToMemInfo){
  //   LLVM_DEBUG(dbgs() <<"\n Usedef for   "<<Iter.first->getName()
  //       <<" \n ");
  //   Iter.second->print();
  // }
}

void InterproceduralMemDFA::getParamNumber(const CallInst &CI,
                                           SetOfInstructions &ReachingDefs,
                                           MemoryLdStMapClass &LdStToMemFunc) {
  for (auto I : ReachingDefs) {
    auto WrittenMem = LdStToMemFunc.getMemoryForLdSt(I);
    LLVM_DEBUG(dbgs() << "\n Writtenmem::" << *WrittenMem);
    for (unsigned ArgNum = 0; ArgNum < CI.getNumOperands(); ArgNum++) {
      auto ArgV = CI.getArgOperand(ArgNum);
      auto PassedArgMem = LdStToMemFunc.getMemoryForLdSt(ArgV);

      LLVM_DEBUG(dbgs() << "\n arg:" << ArgNum << "\n is ::" << *ArgV);
      if (WrittenMem == ArgV) {
        LLVM_DEBUG(dbgs() << "\n Memdef is an argument here");
      } else if (PassedArgMem != nullptr) {
        LLVM_DEBUG(dbgs() << "\n arg:" << ArgNum
                          << "\n is ::" << *PassedArgMem);
        if (WrittenMem == PassedArgMem) {
          LLVM_DEBUG(dbgs() << "\n Memdef is an argument here");
        }
      }
    }
  }
}
bool InterproceduralMemDFA::propagateReachingDefsIntoFunc(
    SetOfInstructions &ReachingDefs, Function &CalledF, const int argNum) {
  // Set the "has converged" properly, else infinite loop.
  return LdStToMem.propagateReachingDefsIntoFunc(ReachingDefs, argNum);
}
void InterproceduralMemDFA::propagateReachingDefsOfCItoBBs(const CallInst &CI){

  Function *CalledF = getCalledFunction(CI);
  // Get the defs generated after the func call.
  SetOfInstructions &FuncGen = getFuncGeneratingDefs(*CalledF);
  SetOfBasicBlocks VisitedSet;
  ListOfBasicBlocks BBQueue;
  BBQueue.push_back(CI.getParent());
  while (!BBQueue.empty()){
    const BasicBlock *BB = BBQueue.back(); BBQueue.pop_back();
    if (EXISTSinMap(VisitedSet, BB)) continue;
    VisitedSet.insert(BB);
    LdStToMem.insertReachingDef(BB, FuncGen);
    for (auto SuccBB =succ_begin(BB) ; SuccBB != succ_end(BB) ; SuccBB++){
      if (!EXISTSinMap(VisitedSet, *SuccBB))
        BBQueue.push_back(*SuccBB);
    }
  }
}
void InterproceduralMemDFA::handleReachingDefsFromCall(const CallInst &CI,
                                                       bool &NewDefsAdded) {
  propagateReachingDefsOfCItoBBs(CI);
  auto ThisFunc = CI.getFunction();
  Function *CalledF = getCalledFunction(CI);
  // Get the defs generated after the func call.
  SetOfInstructions &FuncGen = getFuncGeneratingDefs(*CalledF);
  // Add the FuncGen to ThisFunc's gen defs.
  NewDefsAdded |= LdStToMem.addFuncGeneratingDefs(FuncGen,CI.getParent(), ThisFunc);
  // Now add the defs generated by the func, to its users.
  for (auto User : LdStToMem.getUsersOfCall(CI)) {
    // This adds reaching defs to instructions outside this current basic block
    // also.
    // TODO: need to set NewDefsAdded
    // How to handle call instructions here, so user of a call is another call,
    LLVM_DEBUG(
        dbgs() << "\n User of call :" << *User << " At:"
               << MemoryLdStMapClass::OmpDiagnosticsLocationInfo.getDebugLocStr(
                      *User)
               << "\n FuncGen defs are : ");
    for (auto D : FuncGen) {
      LLVM_DEBUG(dbgs() << "\n "
                        << MemoryLdStMapClass::OmpDiagnosticsLocationInfo
                               .getDebugLocStr(*D));
    }
    LLVM_DEBUG(dbgs() << "\n DONE \n   Inserting the funcgen into the user \n");
    bool CheckAlias = true;
    if (isa<CallInst>(User))
      CheckAlias = false;
    NewDefsAdded |= LdStToMem.insertReachingDef(User, FuncGen, CheckAlias);
    LLVM_DEBUG(
        dbgs() << "\n NewDefsAdded from the funcgen to the user of call inst::"
               << NewDefsAdded);
  }
}

void InterproceduralMemDFA::handleReachingDefsIntoCall(const CallInst &CI,
                                                       bool &NewDefsAdded) {

  // auto ThisFunc = CI.getFunction();
  // auto LdStToMemFunc = FuncToMemInfo[ThisFunc];
  Function *CalledF = getCalledFunction(CI); // nullptr ;
  assert(CalledF != nullptr && "Cannot get the called function ");
  if (CalledF->isDeclaration())
    return;
  LLVM_DEBUG(dbgs() << "\n Called F ::" << CalledF->getName());
  // Record the aliasing information of the actual parameter and the formal
  // parameter.
  LdStToMem.handleCallArguments(CI);
  // Now propagate the reaching defs into the body of the called function. The
  // function returns true if a new reaching def is added, that means analysis
  // has not converged.
  if (LdStToMem.existsReachingDefAt(CI)) {
    SetOfInstructions &ReachingDefs = LdStToMem.getReachingDefsAt(CI);
    // getParamNumber(*CI, ReachingDefs, *LdStToMemFunc);
    NewDefsAdded |= propagateReachingDefsIntoFunc(ReachingDefs, *CalledF);
    LLVM_DEBUG(
        dbgs()
        << "\n NewDefsAdded after propageting reaching defs into the function::"
        << NewDefsAdded);
  }
  unsigned CArgNum = 0;
  // Now iterate over the arguments of the call instruction, and get the
  // reaching defs for each of the arguments, then propagate it.
  for (auto &CArg : CI.args()) {
    if (auto CArgI = dyn_cast<Instruction>(&CArg)) {
      if (LdStToMem.existsReachingDefAt(*CArgI)) {
        SetOfInstructions &ReachingDefs = LdStToMem.getReachingDefsAt(*CArgI);
        // CArgNum not required here.
        NewDefsAdded |=
            propagateReachingDefsIntoFunc(ReachingDefs, *CalledF, CArgNum);
        LLVM_DEBUG(dbgs() << "\n NewDefsAdded after propageting reaching defs "
                             "into the function::"
                          << NewDefsAdded);
      }
    }
    CArgNum++;
  }
}

void InterproceduralMemDFA::updateReachingDefsDueToCall(
    const ListOfInstructions &CallInstructions) {

  // Iterate over all the call instructions, then propagate the reaching defs of
  // the call instruction into the function body. Also, the called function will
  // generate some reaching defs, add the generated reaching defs to the
  // following instructions after the call instruction.
  if (CallInstructions.empty())
    return;
  bool NewDefsAdded = false;
  ConstInstrPtr First = *CallInstructions.begin();
  const BasicBlock &BB = *First->getParent();
  auto ThisFunc = BB.getParent();
  // auto LdStToMemFunc = FuncToMemInfo[ThisFunc];
  // LdStToMem.print();
  for (auto I : CallInstructions) {
    // IF there are new defs generated by the called function, then add it to
    // the instruction.
    assert(isa<CallInst>(I));
    const CallInst *CallInstr = dyn_cast<CallInst>(I);
    Function *CalledF = getCalledFunction(*CallInstr); // nullptr ;
    // if (EXISTSinMap(MemUseDefGlobalAnalysis::IndirectCallsMap, CallInstr)){
    //  // If this call is an indirect call, then get the actual function
    //  called. auto Iter =
    //  MemUseDefGlobalAnalysis::IndirectCallsMap[CallInstr].begin(); CalledF =
    //  Iter->second->getParent();
    //} else
    //  CalledF = CallInstr->getCalledFunction();
    assert(CalledF != nullptr && "Cannot get the called function ");
    if (CalledF->isDeclaration())
      continue;
    // TODO: no def generated, but we need to propagate into the func, cannot
    // continue here. if (!EXISTSinMap(FuncToMemInfo, CalledF)) continue;
    LLVM_DEBUG(dbgs() << "\n Called F ::" << CalledF->getName());
    LdStToMem.handleCallArguments(*CallInstr);
    // Now propagate the reaching defs into the body of the called function. The
    // function returns true if a new reaching def is added, that means analysis
    // has not converged.
    if (LdStToMem.existsReachingDefAt(*I)) {
      SetOfInstructions &ReachingDefs = LdStToMem.getReachingDefsAt(*I);
      // getParamNumber(*CallInstr, ReachingDefs, *LdStToMemFunc);
      NewDefsAdded |= propagateReachingDefsIntoFunc(ReachingDefs, *CalledF);
      LLVM_DEBUG(dbgs() << "\n NewDefsAdded after propageting reaching defs "
                           "into the function::"
                        << NewDefsAdded);
    }
    unsigned CArgNum = 0;
    // Now iterate over the arguments of the call instruction, and get the
    // reaching defs for each of the arguments, then propagate it.
    for (auto &CArg : CallInstr->args()) {
      if (auto CArgI = dyn_cast<Instruction>(CArg)) {
        if (LdStToMem.existsReachingDefAt(*CArgI)) {
          SetOfInstructions &ReachingDefs = LdStToMem.getReachingDefsAt(*CArgI);
          NewDefsAdded |=
              propagateReachingDefsIntoFunc(ReachingDefs, *CalledF, CArgNum);
          LLVM_DEBUG(dbgs() << "\n NewDefsAdded after propageting reaching "
                               "defs into the function::"
                            << NewDefsAdded);
        }
      }
      CArgNum++;
    }
    // Get the defs generated after the func call.
    SetOfInstructions &FuncGen = getFuncGeneratingDefs(*CalledF);
    LdStToMem.addFuncGeneratingDefs(FuncGen,I->getParent(), ThisFunc);
    // Now add the defs generated by the func, to its users.
    for (auto User : LdStToMem.getUsersOfCall(*CallInstr)) {
      // This adds reaching defs to instructions outside this current basic
      // block also.
      // TODO: need to set NewDefsAdded
      LLVM_DEBUG(dbgs() << "\n User of call :"
                        << MemoryLdStMapClass::OmpDiagnosticsLocationInfo
                               .getDebugLocStr(*User)
                        << "\n FuncGen defs are : ");
      for (auto D : FuncGen) {
        LLVM_DEBUG(dbgs() << "\n "
                          << MemoryLdStMapClass::OmpDiagnosticsLocationInfo
                                 .getDebugLocStr(*D));
      }
      LLVM_DEBUG(
          dbgs() << "\n DONE \n   Inserting the funcgen into the user \n");
      NewDefsAdded |= LdStToMem.insertReachingDef(User, FuncGen);
      LLVM_DEBUG(
          dbgs()
          << "\n NewDefsAdded from the funcgen to the user of call inst::"
          << NewDefsAdded);
    }
  }
}

void InterproceduralMemDFA::updateReachingDefsOfBB(
    BasicBlock &BB, bool &NewDefsAdded, std::queue<Function *> &FuncQueue) {

  // Iterate over all the call instructions, then propagate the reaching defs of
  // the call instruction into the function body. Also, the called function will
  // generate some reaching defs, add the generated reaching defs to the
  // following instructions after the call instruction.
  auto ThisFunc = BB.getParent();
  // LdStToMem.print();
  for (Instruction &I : BB) {
    // IF there are new defs generated by the called function, then add it to
    // the instruction.
    if (const CallInst *CallInstr = dyn_cast<CallInst>(&I)) {
      Function *CalledF = getCalledFunction(*CallInstr); // nullptr ;
      // if (EXISTSinMap(MemUseDefGlobalAnalysis::IndirectCallsMap, CallInstr)){
      //  // If this call is an indirect call, then get the actual function
      //  called. auto Iter =
      //  MemUseDefGlobalAnalysis::IndirectCallsMap[CallInstr].begin(); CalledF
      //  = Iter->second->getParent();
      //} else
      //  CalledF = CallInstr->getCalledFunction();
      assert(CalledF != nullptr && "Cannot get the called function ");
      // if (!EXISTSinMap(FuncToMemInfo, CalledF)) continue;
      LLVM_DEBUG(dbgs() << "\n Called F ::" << CalledF->getName());
      FuncQueue.push(CalledF);
      LdStToMem.handleCallArguments(*CallInstr);
      // Now propagate the reaching defs into the body of the called function.
      // The function returns true if a new reaching def is added, that means
      // analysis has not converged.
      if (LdStToMem.existsReachingDefAt(I)) {
        SetOfInstructions &ReachingDefs = LdStToMem.getReachingDefsAt(I);
        // getParamNumber(*CallInstr, ReachingDefs, *LdStToMemFunc);
        NewDefsAdded |= propagateReachingDefsIntoFunc(ReachingDefs, *CalledF);
        LLVM_DEBUG(dbgs() << "\n NewDefsAdded after propageting reaching defs "
                             "into the function::"
                          << NewDefsAdded);
      }
      unsigned CArgNum = 0;
      // Now iterate over the arguments of the call instruction, and get the
      // reaching defs for each of the arguments, then propagate it.
      for (auto &CArg : CallInstr->args()) {
        if (auto CArgI = dyn_cast<Instruction>(CArg)) {
          if (LdStToMem.existsReachingDefAt(*CArgI)) {
            SetOfInstructions &ReachingDefs =
                LdStToMem.getReachingDefsAt(*CArgI);
            NewDefsAdded |=
                propagateReachingDefsIntoFunc(ReachingDefs, *CalledF, CArgNum);
            LLVM_DEBUG(dbgs() << "\n NewDefsAdded after propageting reaching "
                                 "defs into the function::"
                              << NewDefsAdded);
          }
        }
        CArgNum++;
      }
      // Get the defs generated after the func call.
      SetOfInstructions &FuncGen = getFuncGeneratingDefs(*CalledF);
      LdStToMem.addFuncGeneratingDefs(FuncGen,I.getParent(), ThisFunc);
      // Now add the defs generated by the func, to its users.
      for (auto User : LdStToMem.getUsersOfCall(*CallInstr)) {
        // This adds reaching defs to instructions outside this current basic
        // block also.
        // TODO: need to set NewDefsAdded
        LLVM_DEBUG(dbgs() << "\n User of call :"
                          << MemoryLdStMapClass::OmpDiagnosticsLocationInfo
                                 .getDebugLocStr(*User)
                          << "\n FuncGen defs are : ");
        for (auto D : FuncGen) {
          LLVM_DEBUG(dbgs() << "\n "
                            << MemoryLdStMapClass::OmpDiagnosticsLocationInfo
                                   .getDebugLocStr(*D));
        }
        LLVM_DEBUG(
            dbgs() << "\n DONE \n   Inserting the funcgen into the user \n");
        NewDefsAdded |= LdStToMem.insertReachingDef(User, FuncGen);
        LLVM_DEBUG(
            dbgs()
            << "\n NewDefsAdded from the funcgen to the user of call inst::"
            << NewDefsAdded);
      }
    }
  }
}

SetOfInstructions &
InterproceduralMemDFA::getFuncGeneratingDefs(Function &CalledF) {
  return LdStToMem.getFuncGeneratingDefs(&CalledF);
  // auto MemInfo = FuncToMemInfo[&CalledF];
  // return MemInfo->getFuncGeneratingDefs(&CalledF);
  // return MemInfo->getFuncGeneratingDefs();
}

bool InterproceduralMemDFA::getCallInstructions(
    Function &F, std::queue<Function *> &FuncQueue, bool &NewDefsAdded) {
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; I++) {
    if (auto C = dyn_cast<CallInst>(&*I)) {
      auto CalledF = getCalledFunction(*C);
      if (CalledF == nullptr || CalledF->isIntrinsic() ||
          CalledF->isDeclaration())
        continue;
      handleReachingDefsIntoCall(*C, NewDefsAdded);
      handleReachingDefsFromCall(*C, NewDefsAdded);
      FuncQueue.push(CalledF);
    }
  }
  return NewDefsAdded;
}

void InterproceduralMemDFA::run(Function &F) {
  // 0. Create a Queue of functions to analyze.
  // 1. Do a BFS traversal over all the basicblocks of the function, \p F,
  // 2. At every call instruction,
  //    2.0. Add the called func into the queue.
  //    2.1. Get the reaching defs at the call instruction,
  //    2.2. Propagate those reaching defs over the called function body.
  //      2.2.1. Check every mem use inside the function, and iterate over the
  //      reaching defs, if the memory is the same then add to the reaching defs
  //      of the user. 2.2.2. Get the definitions generated by the function,i.e.
  //      all the functions reaching the return instructions inside the
  //      function.
  //  3. Get the defs generated by the function.
  //  4. Look for all the users where the call instr reaches, and add the defs
  //  generated by  the call.

  // FuncToMemInfo[&F]->print();
  bool NewDefsAdded = false;
  unsigned CountIteration = 0;
  do {
    // This is the Queue of functions, to handle the interprocedurally called
    // funcs. Every call instruction adds the function to this queue. We iterate
    // till the queue is empty.
    std::queue<Function *> FuncQueue;
    FuncQueue.push(&F);
    while (!FuncQueue.empty()) {
      auto AnalyzeFunc = FuncQueue.front();
      FuncQueue.pop();
      LLVM_DEBUG(dbgs() << "\n Analyzing functions:" << AnalyzeFunc->getName());
      ListOfInstructions CallInstructions;
      getCallInstructions(*AnalyzeFunc, FuncQueue, NewDefsAdded);
    }
    // while (!FuncQueue.empty()){
    //  auto AnalyzeFunc = FuncQueue.front(); FuncQueue.pop();
    //  LLVM_DEBUG(dbgs()<<"\n Analyzing functions:"<<AnalyzeFunc->getName());
    //  std::set<BasicBlock *> VisitedBBSet;
    //  std::queue< BasicBlock *> BBQueue;
    //  BBQueue.push(&AnalyzeFunc->getEntryBlock());
    //  // BFS traversal of the function body.
    //  while (!BBQueue.empty()) {
    //    auto BB = BBQueue.front();
    //    BBQueue.pop();
    //    // Ignore if already visited.
    //    if (VisitedBBSet.find(BB) != VisitedBBSet.end())
    //      continue;
    //    VisitedBBSet.insert(BB);
    //    //LLVM_DEBUG(dbgs() << "\n Basic Block:" << *BB);
    //    updateReachingDefsOfBB(*BB, NewDefsAdded, FuncQueue);
    //    // print();
    //    for (auto Succ = succ_begin(BB), End = succ_end(BB); Succ != End;
    //        Succ++) {
    //      // LLVM_DEBUG(dbgs()<<"\n Enqueue block:"<<*succ);
    //      BBQueue.push(*Succ);
    //    }
    //  }
    //}
    LLVM_DEBUG(dbgs() << "\n NewDefs added ?" << NewDefsAdded
                      << " iter:" << CountIteration);
    if (CountIteration++ > 20)
      break;
  } while (NewDefsAdded);
  LdStToMem.print();
}

void MemUseDefGlobalAnalysis::analyzeFunction(Function &F,
                                              Result &MemUseDefInfo) {
  if (F.isDeclaration())
    return;
  MemorySSA &MSSA = FunctionAM->getResult<MemorySSAAnalysis>(F).getMSSA();
  LLVM_DEBUG(MSSA.dump());
  MemoryLdStMapClass::OmpDiagnosticsLocationInfo.initFunc(F);
  MemorySSAUseDefWalker useDefWalker(MSSA, MemUseDefInfo);
  useDefWalker.reachingDefAnalysis();
}

MemUseDefGlobalAnalysis::Result
MemUseDefGlobalAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
  // OmpDiagnosticsGlobalInfo result;
  ThisModule = &M;
  // FunctionAM =
  // &AnalysisManager->getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  AnalysisManager = &AM;
  FunctionAM =
      &AnalysisManager->getResult<FunctionAnalysisManagerModuleProxy>(M)
           .getManager();
  LLVM_DEBUG(dbgs() << "\n Analysis module:");
  Result MemUseDefInfo;
  analyzeModule(MemUseDefInfo);

  return MemUseDefInfo;
}

PreservedAnalyses
MemUseDefGlobalAnalysisPrinterPass::run(Module &M, ModuleAnalysisManager &AM) {
  OS << "'omp diagnostics Analysis' for module '" << M.getName() << "'\n";
  auto Res = AM.getResult<MemUseDefGlobalAnalysis>(M);
  Res.print(OS);
  return PreservedAnalyses::all();
}

GetElementPtrInst *MemAccessRangeAnalysis::getGEP(Instruction *Ptr){
  if (Ptr == nullptr) 
    return nullptr ;
  if (auto G = dyn_cast<GetElementPtrInst>(Ptr))
    return G;
  SetOfInstructions VisitedSet;
  std::queue<Instruction *> InstrQ;
  InstrQ.push(Ptr);
  while (!InstrQ.empty()){
    auto I = InstrQ.front();
    InstrQ.pop();
    if (EXISTSinMap(VisitedSet, I))
      continue;
    VisitedSet.insert(I);
    errs()<<"\n searching gep I:"<<*I;
    for (auto &Op:  I->operands()){
      if (auto OpI = dyn_cast<Instruction>(Op)){
        InstrQ.push(OpI);
        if (auto G = dyn_cast<GetElementPtrInst>(OpI)){
          return G;
        }
      }
    }
  }
  return nullptr;
}

int getSCEVMaxValue(std::vector<const SCEV *> &SimplifySCEVStack, std::string &maxSCEV_string, ScalarEvolution &SE){
  if (SimplifySCEVStack.empty()) return 0;
  const SCEV * inst_scev = SimplifySCEVStack.back();
  SimplifySCEVStack.pop_back();
  static bool foundUnkown = false;
  LLVM_DEBUG(dbgs()<<"\n simplify scev:"<<*inst_scev);
  //<<"\n scev vec size:"<<SimplifySCEVStack.size()
  //<<"\n "<<maxSCEV_string);
  switch (static_cast<SCEVTypes>(inst_scev->getSCEVType())) {
    case scConstant: {
                      //  const ConstantInt *constInt = dyn_cast<SCEVConstant>(inst_scev)->getValue();
                      //  LLVM_DEBUG(dbgs()<<"\n Constant scev:"<<*constInt);
                      //  maxSCEV_string += constInt->getValue().toString(10, true) ;
                      //  SimplifySCEVStack.push_back(inst_scev); //OMPCOMPOUND FIXME
                       /*if constant then itself is the max value,*/
                       return 1;
                     }
    case scTruncate: {
                       LLVM_DEBUG(dbgs()<<"\n Truncate scev:");
                       const SCEVTruncateExpr *Trunc = cast<SCEVTruncateExpr>(inst_scev);
                       const SCEV *Op = Trunc->getOperand();
                       SimplifySCEVStack.push_back(Op);
                       getSCEVMaxValue(SimplifySCEVStack,maxSCEV_string, SE);
                       return -1;
                     }
    case scZeroExtend: {
                         LLVM_DEBUG(dbgs()<<"\n ZeroExnt scev:");
                         const SCEVZeroExtendExpr *ZExt = cast<SCEVZeroExtendExpr>(inst_scev);
                         const SCEV *Op = ZExt->getOperand();
                         SimplifySCEVStack.push_back(Op);
                         getSCEVMaxValue(SimplifySCEVStack, maxSCEV_string, SE);
                         return -1;
                       }
    case scSignExtend: {
                         LLVM_DEBUG(dbgs()<<"\n ZeroExnt scev:");
                         const SCEVSignExtendExpr *SExt = cast<SCEVSignExtendExpr>(inst_scev);
                         const SCEV *Op = SExt->getOperand();
                         SimplifySCEVStack.push_back(Op);
                         getSCEVMaxValue(SimplifySCEVStack,  maxSCEV_string, SE);
                         return -1;
                       }
    case scAddRecExpr: {
                         LLVM_DEBUG(dbgs()<<"\n add rec expr  scev:");
                         const SCEVAddRecExpr *AR = cast<SCEVAddRecExpr>(inst_scev);
                         const Loop *fLoop = AR->getLoop();
                         auto beCountScev = SE.getBackedgeTakenCount(fLoop);
                         LLVM_DEBUG(dbgs()<< "\n TRIP COUNT ::"<<SE.getSmallConstantMaxTripCount(fLoop)
                             <<" \n be count::"<<*beCountScev
                             <<"bescev count: "<<beCountScev->getSCEVType()
                             <<"\n Max BE Count: "<< *SE.getMaxBackedgeTakenCount(fLoop));
                         if (beCountScev->getSCEVType() !=  scUnknown 
                             && beCountScev->getSCEVType() != scCouldNotCompute
                            ) {
                           beCountScev = SE.getMinusSCEV(beCountScev, SE.getOne(beCountScev->getType()));
                           const SCEV *maxVal = AR->evaluateAtIteration(beCountScev, SE);
                           SimplifySCEVStack.push_back(maxVal);
                           getSCEVMaxValue(SimplifySCEVStack, maxSCEV_string, SE);
                         } else 
                           foundUnkown = true;
                         /*for add rec, simply get the maximum possible 
                          * value for the last iteration of the for loop*/
                         return -1;
                       }
    case scAddExpr: {
                      LLVM_DEBUG(dbgs()<<"\n Add  scev:");
                      const SCEVAddExpr *Sadd = cast<SCEVAddExpr>(inst_scev);
                      const SCEV *Op= Sadd->getOperand(0);
                      SimplifySCEVStack.push_back(Op);//simplify first op
                      maxSCEV_string += "(";
                      getSCEVMaxValue(SimplifySCEVStack, maxSCEV_string, SE);
                      if (foundUnkown) {
                        maxSCEV_string += ")";
                        return -1;
                      }
                      maxSCEV_string += "+";
                      const SCEV *lhs = SimplifySCEVStack.back();
                      SimplifySCEVStack.pop_back();
                      Op= Sadd->getOperand(1);
                      SimplifySCEVStack.push_back(Op);//simplify second op
                      getSCEVMaxValue(SimplifySCEVStack, maxSCEV_string, SE);
                      maxSCEV_string += ")";
                      if (!foundUnkown) {
                        const SCEV *rhs = SimplifySCEVStack.back();
                        SimplifySCEVStack.pop_back();
                        auto addSCEV  = SE.getAddExpr(lhs, rhs);
                        SimplifySCEVStack.push_back(addSCEV);
                      }
                      //          getSCEVMaxValue(SimplifySCEVStack, SE, LI);
                      return -1;
                    }
    case scMulExpr: {
                      LLVM_DEBUG(dbgs()<<"\n mul scev:");
                      const SCEVMulExpr *Smul = cast<SCEVMulExpr>(inst_scev);
                      const SCEV *Op= Smul->getOperand(0);
                      SimplifySCEVStack.push_back(Op);//simplify first op
                      maxSCEV_string += "(";
                      getSCEVMaxValue(SimplifySCEVStack, maxSCEV_string, SE);
                      if (foundUnkown) {
                        maxSCEV_string += ")";
                        return -1;
                      }
                      LLVM_DEBUG(dbgs()<<"\n size:"<<SimplifySCEVStack.size()<<"\n");
                      const SCEV *lhs = SimplifySCEVStack.back();
                      SimplifySCEVStack.pop_back();
                      Op= Smul->getOperand(1);
                      SimplifySCEVStack.push_back(Op);//simplify second op
                      maxSCEV_string += "*";
                      getSCEVMaxValue(SimplifySCEVStack, maxSCEV_string, SE);
                      LLVM_DEBUG(dbgs()<<"\n size:"<<SimplifySCEVStack.size()<<"\n");
                      maxSCEV_string += ")";
                      if (!foundUnkown) {
                        const SCEV *rhs = SimplifySCEVStack.back();
                        SimplifySCEVStack.pop_back();
                        auto mulSCEV  = SE.getMulExpr(lhs, rhs);
                        SimplifySCEVStack.push_back(mulSCEV);
                      }
                      //            getSCEVMaxValue(SimplifySCEVStack, SE, LI);
                      return -1;
                    }
    case scUMaxExpr:
    case scSMaxExpr: {
                       LLVM_DEBUG(dbgs()<<"\n max scev:");
                       const SCEVNAryExpr *SExt = cast<SCEVNAryExpr>(inst_scev);
                       //const SCEVSMaxExpr *SExt = cast<SCEVSMaxExpr>(inst_scev);
                       //ignore the first operator in maxexpr, example:(0 smax %p) 
                       //TODO Generalize it
                       LLVM_DEBUG(dbgs()<<"\n Ignoring SMax first op:"<<*SExt);
                       const SCEV *Op = SExt->getOperand(1);
                       SimplifySCEVStack.push_back(Op);
                       getSCEVMaxValue(SimplifySCEVStack, maxSCEV_string, SE);
                       return -1;
                     }
    case scUMinExpr:
    case scSMinExpr:{
                       LLVM_DEBUG(dbgs()<<"\n min scev:");
                       const SCEVNAryExpr *SExt = cast<SCEVNAryExpr>(inst_scev);
                       //const SCEVSMaxExpr *SExt = cast<SCEVSMaxExpr>(inst_scev);
                       //ignore the first operator in maxexpr, example:(0 smax %p) 
                       //TODO Generalize it
                       LLVM_DEBUG(dbgs()<<"\n Ignoring SMin first op:"<<*SExt);
                       const SCEV *Op = SExt->getOperand(1);
                       SimplifySCEVStack.push_back(Op);
                       getSCEVMaxValue(SimplifySCEVStack, maxSCEV_string, SE);
                       return -1;
                    }
    case scUDivExpr: {
                       LLVM_DEBUG(dbgs()<<"\n div scev:");
                       const SCEVUDivExpr *UDiv = cast<SCEVUDivExpr>(inst_scev);
                       const SCEV *Op= UDiv->getLHS() ;
                       SimplifySCEVStack.push_back(Op);//simplify first op
                       maxSCEV_string += "(";
                       getSCEVMaxValue(SimplifySCEVStack, maxSCEV_string, SE);
                       if (foundUnkown) {
                         maxSCEV_string += ")";
                         return -1;
                       }
                       const SCEV *lhs = SimplifySCEVStack.back();
                       SimplifySCEVStack.pop_back();
                       Op= UDiv->getRHS();
                       SimplifySCEVStack.push_back(Op);//simplify second op
                       maxSCEV_string += "/";
                       getSCEVMaxValue(SimplifySCEVStack, maxSCEV_string, SE);
                       maxSCEV_string += ")";
                       if (!foundUnkown) {
                         const SCEV *rhs = SimplifySCEVStack.back();
                         SimplifySCEVStack.pop_back();
                         auto divSCEV  = SE.getUDivExpr(lhs, rhs);
                         SimplifySCEVStack.push_back(divSCEV);
                       }
                       //              getSCEVMaxValue(SimplifySCEVStack, SE, LI);
                       return -1;
                     }
    case scUnknown: {
                      LLVM_DEBUG(dbgs()<<"\n unknown scev:");
                      const SCEVUnknown *U = cast<SCEVUnknown>(inst_scev);
                      SimplifySCEVStack.push_back(inst_scev);
                      const Value *v = U->getValue();
                      maxSCEV_string +=   v->getName().str();
                      foundUnkown = true;
                      //Type *AllocTy;
                      //if (U->isSizeOf(AllocTy)) {
                      //  OS << "sizeof(" << *AllocTy << ")";
                      //  return;
                      //}
                      //if (U->isAlignOf(AllocTy)) {
                      //  OS << "alignof(" << *AllocTy << ")";
                      //  return;
                      //}

                      //Type *CTy;
                      //Constant *FieldNo;
                      //if (U->isOffsetOf(CTy, FieldNo)) {
                      //  OS << "offsetof(" << *CTy << ", ";
                      //  FieldNo->printAsOperand(OS, false);
                      //  OS << ")";
                      //  return;
                      //}

                      // Otherwise just print it normally.

                      return 1;
                    }
    case scCouldNotCompute:
                    return -1;
  }
  llvm_unreachable("Unknown SCEV kind!");
}

void MemAccessRangeAnalysis::handleInstruction(Instruction &Inst, ScalarEvolution &SE){
  Value *Ptr = nullptr;
  // We are only interested in memory load/store instructions, ignore other instructions.
  if (auto St = dyn_cast<StoreInst>(&Inst)){
    Ptr = St->getPointerOperand();
  }else if (auto Ld = dyn_cast<LoadInst>(&Inst) ){
    Ptr = Ld->getPointerOperand();
  } else 
    return;
  assert(Ptr != nullptr);
  // If the address operand is not an instruction, that means it is a const address, ignore it.
  if (!isa<Instruction>(Ptr))
    return;
  GetElementPtrInst *GEP = getGEP(dyn_cast<Instruction>(Ptr));
  if (GEP == nullptr) 
    return;
  // Find the interesting operand of the GEP, which is the index operand.
  errs()<<"\n GEP:"<<*GEP;
  unsigned IndexOperand = 0;
  for (unsigned I= 1, E = GEP->getNumOperands() ; I != E ; ++I){
    if (!isa<ConstantInt>(GEP->getOperand(I))) {
      IndexOperand = I;
      break;
    }
  }
  if (IndexOperand == 0 ) return; //all indices constant, no need of scev analysis
  auto PtrScev = SE.getSCEV(GEP);
  Value *GepIndex = GEP->getOperand(IndexOperand);
  if (isa<Instruction>(GepIndex)){
    PtrScev = SE.getSCEV(GepIndex);
  }
  LLVM_DEBUG(dbgs()<<"\n SCEV of :"<<Inst <<" is:"<<*PtrScev<<"\n");
  std::vector<const SCEV*> SimplifySCEV = {PtrScev};
  std::string MaxSCEVStr = "";
  getSCEVMaxValue(SimplifySCEV, MaxSCEVStr, SE);
  LLVM_DEBUG(dbgs()<<"\n MaxSCEV:"<<MaxSCEVStr <<" ");
}

void MemAccessRangeAnalysis::analyzeFunc(Function &F, ScalarEvolution &SE){

  for (inst_iterator IIter = inst_begin(&F), E = inst_end(&F); IIter != E; ++IIter) {
    Instruction &Inst = *IIter;
    handleInstruction(Inst, SE);
  }
}

MemAccessRangeAnalysis::Result MemAccessRangeAnalysis::run(Function &F, FunctionAnalysisManager &AM){
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  analyzeFunc(F, SE);
  MemoryLdStMapClass R;
  return R;
}

PreservedAnalyses
MemAccessRangeAnalysisPrinterPass::run(Function &F, FunctionAnalysisManager &AM) {

  OS << "\n 'omp diagnostics Local Analysis' for function '" << F.getName()
     << "'\n";
  auto R = AM.getResult<MemAccessRangeAnalysis>(F);
  return PreservedAnalyses::all();
}
AnalysisKey MemAccessRangeAnalysis::Key;
static const char LocalPassArg[] = "omp-diagnostics-local";
static const char LocalPassName[] = "omp diagnostics Local Analysis";
static const char GlobalPassName[] = "omp diagnostics Analysis";
