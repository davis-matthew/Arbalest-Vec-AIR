//===- OmpDiagnosticsAnalysis.cpp - Stack memory safety analysis
//-------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/OMPSan/OmpDiagnosticsAnalysis.h"

#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/AbstractCallSite.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/raw_ostream.h"

// Map-type flag bits from openmp/libomptarget/include/omptarget.h.
// Reproduced here to avoid a dependency on the OpenMP runtime headers.
static const unsigned OMP_TGT_MAPTYPE_TO     = 0x001; // copy host → device
static const unsigned OMP_TGT_MAPTYPE_FROM   = 0x002; // copy device → host
static const unsigned OMP_TGT_MAPTYPE_ALWAYS = 0x004; // copy regardless of refcount
static const unsigned OMP_TGT_MAPTYPE_DELETE = 0x008; // force unmapping


#include <iostream> //OMPCOMPOUND

using namespace llvm;

#define DEBUG_TYPE "omp-diagnostics"

const RTLinfoType *getRTLinfo(const CallInst &CI) {
  if (auto CalledFunc = CI.getCalledFunction()) {
    auto CName = CalledFunc->getName();
    auto Iter = TargetRTLMap.find(CName.str());
    if (Iter != TargetRTLMap.end()) {
      const RTLinfoType *Ret = &Iter->second;
      return Ret;
    }
  }
  return nullptr;
}
OmpDiagnosticsInfo::OmpDiagnosticsInfo() {
  DeviceEnvironments.push_back(ItemsRefCountType());
}
// OmpDiagnosticsInfo::OmpDiagnosticsInfo(OmpDiagnosticsInfo &&) = default;
// OmpDiagnosticsInfo &OmpDiagnosticsInfo::operator=(OmpDiagnosticsInfo &&) =
// default;

const std::string getDebugLocStr(const Instruction &inst) {
  const std::string invalidRet = ""; //(unsigned)(-1);
  if (!inst.getDebugLoc())
    return invalidRet;
  const DebugLoc &loc = inst.getDebugLoc();
  if (auto *scope = dyn_cast<DIScope>(loc.getScope())) {
    auto filename = scope->getFilename();
    return filename.str() + ":" + std::to_string(loc->getLine());
  }
  return invalidRet;
}

OmpDiagnosticsInfo::~OmpDiagnosticsInfo() = default;

void OmpDiagnosticsInfo::print() const {

  for (auto Iter : DirectiveToDataMap) {
    LLVM_DEBUG(dbgs() << "\n Callinst::" << *Iter.first);
    for (auto &DataMapping : Iter.second) {
      LLVM_DEBUG(dbgs() << "\n MappedValue: " << *DataMapping.MappedValue
                        << " MapType ::" << DataMapping.MapTypeInt
                        << " MappedSectionSize ::"
                        << DataMapping.MappedSectionSize);
    }
  }
}

void OmpDiagnosticsInfo::printCopies(const InstructionToMemCopyMapType &ItoCopy,
                                     const std::string &CopyKind,
                                     raw_ostream &O) const {
  for (auto Iter : ItoCopy) {
    O << "\n " << getDebugLocStr(*Iter.first);
    std::set<std::string> PrintedValNames;
    for (auto Id : Iter.second) {
      if (filterId(Id.MappedValue))
        continue;
      auto ValName = getName(Id.MappedValue);
      if (EXISTSinMap(PrintedValNames, ValName))
        continue;
      PrintedValNames.insert(ValName);
      O << "\n " << CopyKind << ":: " << ValName
        << "[0:" << (Id.MappedSectionSize > 0 ? Id.MappedSectionSize : -1)
        << "]";
    }
  }
}

void OmpDiagnosticsInfo::print(raw_ostream &O) const {

  for (auto Iter : DirectiveToDataMap) {
    LLVM_DEBUG(dbgs() << "\n Callinst::" << *Iter.first);
    for (auto &DataMapping : Iter.second) {
      LLVM_DEBUG(dbgs() << "\n MappedValue: " << *DataMapping.MappedValue
                        << " MapType ::" << DataMapping.MapTypeInt
                        << " MappedSectionSize ::"
                        << DataMapping.MappedSectionSize);
    }
  }
  if (AllocatedItems.size()) {
    O << "\n =============== ";
    O << "\n Allocated on Device:";
    printCopies(AllocatedItems, "Alloc", O);
    // for (auto Iter : AllocatedItems) {
    //  O << "\n " << getDebugLocStr(*Iter.first);
    //  std::set<std::string> PrintedValNames;
    //  for (auto Id : Iter.second) {
    //    if (filterId(Id.MappedValue)) continue;
    //    auto ValName = getName(Id.MappedValue);
    //    if (EXISTSinMap(PrintedValNames, ValName)) continue;
    //    PrintedValNames.insert(ValName);
    //    O << "\n Alloc:: " << ValName
    //      << "[0:" << Id.MappedSectionSize << "]";
    //  }
    //}
    O << "\n =============== \n ";
  }
  if (HostDeviceCopy.size()) {
    O << "\n =============== ";
    O << "\n Host to Device Copy:";
    printCopies(HostDeviceCopy, "H-D Copy", O);
    // for (auto Iter : HostDeviceCopy) {
    //  O << "\n " << getDebugLocStr(*Iter.first);
    //  for (auto Id : Iter.second) {
    //    if (filterId(Id.MappedValue)) continue;
    //    O << "\n Copy:: " << getName(Id.MappedValue)
    //      << "[0:" << Id.MappedSectionSize << "]";
    //  }
    //}
    O << "\n =============== \n ";
  }
  if (DeviceHostCopy.size()) {
    O << "\n =============== \n ";
    O << "\n Device to Host Copy:";
    printCopies(DeviceHostCopy, "D-H Copy", O);
    // for (auto Iter : DeviceHostCopy) {
    //  O << "\n " << getDebugLocStr(*Iter.first);
    //  for (auto Id : Iter.second) {
    //    if (filterId(Id.MappedValue)) continue;
    //    O << "\n Copy:: " << getName(Id.MappedValue)
    //      << "[0:" << Id.MappedSectionSize << "]";
    //  }
    //}
  }
  if (DevicePersistentIn.size()) {
    O << "\n =============== \n ";
    O << "\n Persistent on Device :";
    printCopies(DevicePersistentIn, "Persistent on Device", O);
  }
  O << "\n =============== \n ";
  O << "\n Done \n";
}

bool OmpDiagnosticsInfo::filterId(const IdType &ID) const {
  auto IdName = getName(ID);
  if (isa<ConstantData>(ID))
    return true;
  if (IdName == "" || IdName.find(".sroa.") != std::string::npos)
    return true;
  return false;
}

std::string OmpDiagnosticsInfo::getName(const IdType &ID) const {
  if (EXISTSinMap(Value2NameMap, ID)) {
    const std::string S = (Value2NameMap.find(ID))->second;
    return S;
  }
  if (auto G = dyn_cast<GlobalVariable>(ID)) {
    SmallVector<DIGlobalVariableExpression *, 1> GVs;
    G->getDebugInfo(GVs);
    for (DIGlobalVariableExpression *DIG : GVs) {
      DIGlobalVariable *Gvariable = DIG->getVariable();
      if (Gvariable != nullptr)
        return Gvariable->getDisplayName().str();
    }
  }
  if (!ID->hasName())
    return "";
  return ID->getName().str();
}
void OmpDiagnosticsInfo::enterDataEnv(const Instruction &OmpCall,
                                      OmpDataMapping &MappedVar,
                                      unsigned DeviceId) {
  IdType ItemId = MappedVar.MappedValue;
  auto MapTypeInt = MappedVar.MapTypeInt;
  // IdType ItemId, unsigned MapTypeInt, unsigned ItemSize,
  // 1. If a corresponding list item of the original list item is not present in
  // the device data environment,then:
  //    a) A new list item with language-specific attributes is derived from the
  //    original list item and23created in the device data environment; b) The
  //    new list item becomes the corresponding list item of the original list
  //    item in the device25data environment; c) The corresponding list item has
  //    a reference count that is initialized to zero; and d) The value of the
  //    corresponding list item is undefined;
  //  2. If the corresponding list item’s reference count was not already
  //  incremented because of the effect of amapclause on the construct then:
  //    a) The corresponding list item’s reference count is incremented by one;
  //  3. If the corresponding list item’s reference count is one or
  //  thealwaysmap-type-modifier is present, and if themap-typeistoortofrom,
  //  then
  //    a) For each part of the list item that is an attached pointer, that part
  //    of the corresponding list3item will have the value that it had
  //    immediately prior to the effect of themapclause; a
  ItemsRefCountType &DEnvMap = DeviceEnvironments[DeviceId];
  // If the item not present in device environment
  if (!EXISTSinMap(DEnvMap, ItemId)) {
    // Initialize reference count to zero
    DEnvMap[ItemId] = 0;
  }
  DEnvMap[ItemId]++;
  if ((DEnvMap[ItemId] == 1 || (MapTypeInt & OMP_TGT_MAPTYPE_ALWAYS)) &&
      (MapTypeInt & OMP_TGT_MAPTYPE_TO)) {
    LLVM_DEBUG(dbgs() << " Copy Host to Device");
    if (!alreadyInserted(AllocatedItems[&OmpCall], MappedVar))
      HostDeviceCopy[&OmpCall].push_back(MappedVar); //.insert(ItemId);
  } else if (DEnvMap[ItemId] == 1) {

    LLVM_DEBUG(dbgs() << " Allocated to Device");
    if (!alreadyInserted(AllocatedItems[&OmpCall], MappedVar))
      AllocatedItems[&OmpCall].push_back(MappedVar); //.insert(ItemId);
  } else if (DEnvMap[ItemId] > 1){
    DevicePersistentIn[&OmpCall].push_back(MappedVar);
    LLVM_DEBUG(dbgs() << " Persistent to Device");
  }

  LLVM_DEBUG(dbgs() << "\n Enter ItemID:" << ItemId
                    << " Count :" << DEnvMap[ItemId]);
}
void OmpDiagnosticsInfo::exitDataEnv(const Instruction &OmpCall,
                                     OmpDataMapping &MappedVar,
                                     unsigned DeviceId) {
  IdType ItemId = MappedVar.MappedValue;
  auto MapTypeInt = MappedVar.MapTypeInt;

  // 1. If themap-type is notdeleteand the corresponding list item’s reference
  // count is finite and was not already decremented because of the effect of
  // amapclause on the construct then:
  //  a) The corresponding list item’s reference count is decremented by one;.
  // 2. If themap-type is delete and the corresponding list item’s reference
  // count is finite then:
  //    a) The corresponding list item’s reference count is set to zero;
  // 3. If themap-typeisfromortofromand if the corresponding list item’s
  // reference count is zero or the alwaysmap-type-modifieris present then:
  //    a) For each part of the list item that is an attached pointer, that part
  //    of the original list item will1have the value that it had immediately
  //    prior to the effect of themapclause;
  // 4. If the corresponding list item’s reference count is zero then the
  // corresponding list item is8removed from the device data environment
  LLVM_DEBUG(dbgs() << "\n IN Exit Data env for " << ItemId << " Maptype::"
                    << MapTypeInt << " Maptype:: " << OMP_TGT_MAPTYPE_FROM
                    << " ismap from::" << (MapTypeInt & OMP_TGT_MAPTYPE_FROM));
  ItemsRefCountType &DEnvMap = DeviceEnvironments[DeviceId];
  // If the item not present in device environment
  if (!EXISTSinMap(DEnvMap, ItemId)) {
    // Initialize reference count to zero
    LLVM_DEBUG(dbgs() << "\n Doesnot Exist in Map:" << ItemId);
    return;
  }
  DEnvMap[ItemId]--;
  LLVM_DEBUG(dbgs() << "\nExit Env Map Count:" << DEnvMap[ItemId]);
  if (MapTypeInt & OMP_TGT_MAPTYPE_DELETE) {
    DEnvMap[ItemId] = 0;
    LLVM_DEBUG(dbgs() << "\n Delete :" << DEnvMap[ItemId]);
  }
  if ((DEnvMap[ItemId] == 0 || (MapTypeInt & OMP_TGT_MAPTYPE_ALWAYS)) &&
      (MapTypeInt & OMP_TGT_MAPTYPE_FROM)) {
    LLVM_DEBUG(dbgs() << " Copy Device to Host");
    if (!alreadyInserted(AllocatedItems[&OmpCall], MappedVar))
      DeviceHostCopy[&OmpCall].push_back(MappedVar); // insert(ItemId);
  }
  if (DEnvMap[ItemId] == 0)
    DeviceEnvironments[DeviceId].erase(ItemId);
  else 
    DevicePersistentOut[&OmpCall].push_back(MappedVar);
}
void OmpDiagnosticsInfo::processOmpRTLCall(const CallInst &CI) {
  if (!EXISTSinMap(DirectiveToDataMap, &CI))
    return;
  auto &DataMappingList = DirectiveToDataMap[&CI];
  auto RTLInfo = getRTLinfo(CI);
  assert(RTLInfo != nullptr && "Not a data mapping RTL call ");
  LLVM_DEBUG(dbgs() << "\n EnterEnv:: " << RTLInfo->IsEnterEnv
                    << " Exit:: " << RTLInfo->IsExitEnv << " Call::" << CI);
  bool isUpdate =
      (CI.getCalledFunction()->getName().find("update") != std::string::npos);
  for (auto &MapInfo : DataMappingList) {
    if (isUpdate)
      MapInfo.MapTypeInt = MapInfo.MapTypeInt & OMP_TGT_MAPTYPE_ALWAYS;
    if (RTLInfo->IsEnterEnv)
      enterDataEnv(CI, MapInfo); //.MappedValue, MapInfo.MapTypeInt,
                                 // MapInfo.MappedSectionSize);
    if (RTLInfo->IsExitEnv)
      exitDataEnv(CI, MapInfo); //.MappedValue, MapInfo.MapTypeInt,
                                // MapInfo.MappedSectionSize);
  }
}
void OmpDiagnosticsInfo::insertNameForVal(const Value *From, const Value *To) {
  if (EXISTSinMap(Value2NameMap, From)) {
    Value2NameMap[To] = Value2NameMap[From];
  }
}
void OmpDiagnosticsInfo::insertNameForVal(const Value *V,
                                          const std::string &Name) {
  LLVM_DEBUG(dbgs() << "\n Val::" << *V << " name::" << Name);
  Value2NameMap[V] = Name;
}
void OmpDiagnosticsInfo::addDirectiveDataMapping(const CallInst &CI,
                                                 const Value *V,
                                                 const unsigned T,
                                                 const unsigned S) {
  DirectiveToDataMap[&CI].push_back(OmpDataMapping(V, T, S));
}
// bool ValueFlowAtInstruction::getIdForValue(IdType &Id, const Value *V,
//                                           const unsigned Index) {
//  if (!EXISTSinMap(Address2ContentMap, V) || (Address2ContentMap[V].size() <=
//  Index))
//    return false;
//  Id = Address2ContentMap[V].at(Index);
//  return true;
//}

ValueFlowAtInstruction::IdType
ValueFlowAtInstruction::getIdForValue(const Value *V, const unsigned Index) {
  if (!EXISTSinMap(Value2IdMap, V) || !EXISTSinMap(Value2IdMap[V], Index)) {

    // Insert a new entry, get the last Id. It is size +1 , to make sure the 0th
    // entry is always empty, for unkown Ids.
    auto NewId = Id2ValueMap.size();
    Value2IdMap[V][Index] = NewId;
    Id2ValueMap.resize(NewId + 1);
    Id2ValueMap[NewId] = V;
  }
  LLVM_DEBUG(dbgs() << "\n Id :" << Value2IdMap[V][Index]
                    << " for value: " << *V);
  return Value2IdMap[V][Index];
}
// static unsigned IdCounter = 0;
// if (EXISTSinMap(Address2ContentMap, V))
//  if (Address2ContentMap[V].size() > Index)
//    return Address2ContentMap[V][Index];
//// Now, create an id for all the entries till Index
//// The following loop is required, so that we can
//// create an entry for Address2ContentMap[V][2] even before the entry for
//// Address2ContentMap[V][0] exists If the Size is less than Index+1, then
///create an / entry for all the elements, Till Index, and assign a new Id to
///them
// while (Index >= Address2ContentMap[V].size() ) {
//  // Last element at [index]
//  Address2ContentMap[V].push_back(IdCounter);
//  Id2ValueMap[IdCounter] = V; // .push_back(V);
//  IdCounter++;
//  //LLVM_DEBUG(dbgs()<<"\n size of value2id:"<<Address2ContentMap[V].size()
//  //<< " size of id2valuemap:"<<Id2ValueMap.size());
//}
//// Now size = Index+1
// return Address2ContentMap[V][Index];

bool ValueFlowAtInstruction::isMemory(ValueType V) {
  if (V == nullptr)
    return false;
  if (isa<GlobalVariable>(V) || isa<AllocaInst>(V)) {
    return true;
  }
  return false;
}

bool ValueFlowAtInstruction::getGep(const Value *GEPI, ValueType &PtrOp,
                                    unsigned &Index) {
  if (GEPI == nullptr)
    return false;
  if (auto GEP = dyn_cast<GetElementPtrInst>(GEPI)) {
    PtrOp = GEP->getPointerOperand();
    // Since we are only concerned about the omp rtl function arguments,
    // We know that they will have constant indices
    // So, we only handle this case
    if (!GEP->hasAllConstantIndices())
      return false;

    auto IndexOpIt = (GEP->idx_end() - 1);
    const Value *IndexOp = *IndexOpIt;
    const unsigned ConstInd =
        dyn_cast<Constant>(IndexOp)->getUniqueInteger().getZExtValue();
    LLVM_DEBUG(dbgs() << "\n PtrOp:" << *PtrOp << "\n IndexOp: " << *IndexOp
                      << "\n Const Index:" << ConstInd);
    Index = ConstInd;
    return true;
  }
  return false;
}
// IdType ValueFlowAtInstruction::getMemory(ValueType V) {
//  if (V == nullptr) return 0;
//  auto Id = getIdForValue(V);
//  if (isMemory(V))
//    return Id;
//  if (EXISTSinMap(ValueFlowMap, V))
//    return ValueFlowMap[Id][0];
//  return 0;
//}

/// Lhs has the same contents as Rhs
/// Depending on their types, it may either point to the same object or
/// Have the same Value
// void ValueFlowAtInstruction::addAlias(const Value *LhsVal, const Value
// *RhsVal,
//    unsigned Index) {
//  if (LhsVal == nullptr) return;
//  auto Lhs = getIdForValue(LhsVal);
//  IdType Rhs = 0;
//  if (RhsVal != nullptr )
//    Rhs = getIdForValue(RhsVal);
//  if (isMemory(LhsVal)){
//    // Rhs is being stored at the address Lhs[Index].
//    //ValueFlowMap[Lhs].resize(Index+1);
//    ValueFlowMap[Lhs][Index] = Rhs;
//  } else if (isMemory(RhsVal)) {
//    // The Rhs is a memory, that means, Lhs = &Rhs[Index]. We can only index
//    into an address. ValueFlowMap[Lhs][0] = getIdForValue(Rhs, Index);
//    //ValueFlowMap[Rhs][Index];
//  } else if (EXISTSinMap(ValueFlowMap, Rhs )){
//    // Recursively track the values to the address.
//    RhsVal = getValueFromId(ValueFlowMap[Rhs][0]);
//    addAlias(LhsVal,RhsVal, Index );
//  } else {
//    // Ignore the index, since it only makes sense in case of memories.
//    ValueFlowMap[Lhs][0] = Rhs;
//  }
//
//}

void ValueFlowAtInstruction::updateInstr(
    const Instruction &ValuesAtInstruction) {
  auto I = &ValuesAtInstruction;
  LLVM_DEBUG(dbgs() << "\n Analyze:" << *I);
  if (auto GEP = dyn_cast<GetElementPtrInst>(I)) {
    // auto PtrOp = GEP->getPointerOperand();
    //// Since we are only concerned about the omp rtl function arguments,
    //// We know that they will have constant indices
    //// So, we only handle this case
    // if (GEP->hasAllConstantIndices()) {
    //  auto IndexOpIt = (GEP->idx_end() - 1);
    //  const Value *IndexOp = *IndexOpIt;
    //  const unsigned ConstInd =
    //    dyn_cast<Constant>(IndexOp)->getUniqueInteger().getZExtValue();
    //  LLVM_DEBUG(dbgs() << "\n PtrOp:" << *PtrOp << "\n IndexOp: " << *IndexOp
    //      << "\n Const Index:" << ConstInd);
    //  // This means that,
    //  // after this instruction, contents of PtrOp same as I
    //  // I = PtrOp[ConstInd]
    //  //addAlias(I, PtrOp, ConstInd);
    //  ValueFlowMap[getIdForValue(I)] = getIdForValue(PtrOp,ConstInd);
    //}
    ValueType PtrOp = nullptr;
    unsigned Index = 0;
    if (getGep(GEP, PtrOp, Index)) {
      ValueFlowMap[getIdForValue(I)] = getIdForValue(PtrOp, Index);
    }
  } else if (auto LoadedValue = dyn_cast<LoadInst>(I)) {
    auto LoadAddress = LoadedValue->getPointerOperand();
    LLVM_DEBUG(dbgs() << "\n load addr:" << *LoadAddress);
    unsigned Index = 0;
    if (auto GEPop = dyn_cast<GEPOperator>(LoadAddress)) {
      LoadAddress = GEPop->getPointerOperand();
    }
    // LoadedValue = LoadAddress[0];
    ValueFlowMap[getIdForValue(I)] = getIdForValue(LoadAddress, Index);
  } else if (auto Store = dyn_cast<StoreInst>(I)) {
    auto Address = (Store->getPointerOperand());
    unsigned Index = 0;
    if (auto GEPop = dyn_cast<GEPOperator>(Address)) {
      Address = GEPop->getPointerOperand();
    }
    if (isa<GetElementPtrInst>(Address)) {
      LLVM_DEBUG(dbgs() << "\n is gep");
      ValueType PtrOp = nullptr;
      if (getGep(Address, PtrOp, Index)) {
        Address = PtrOp;
      }
    }
    auto StoredVal = (Store->getValueOperand());
    // Since the LHS is an address, trace back the entry for the address, and
    // get the proper index. Address[0] = StoredVal
    auto AddrId = getIdForValue(Address, Index);
    std::set<unsigned int> addresses; //OMPCOMPOUND cycle detection
    while (EXISTSinMap(ValueFlowMap, AddrId)) {
      if(addresses.count(ValueFlowMap[AddrId]) != 0){
        break;
      }
      addresses.insert(AddrId);
      AddrId = ValueFlowMap[AddrId];
    }
    ValueType ValAddr = nullptr;
    getValueForId(AddrId, ValAddr, Index);
    if (isMemory(ValAddr)) {
      ValueFlowMap[AddrId] = getIdForValue(StoredVal);
    }
  } else if (I->isCast()) {
    // Only one operand
    auto Src = I->getOperand(0);
    // After this instruction, contents of Src same as I
    // Src = I
    LLVM_DEBUG(dbgs() << "\n bitcast:" << getIdForValue(Src));
    LLVM_DEBUG(dbgs() << "\n bitcast:" << getIdForValue(I));
    ValueFlowMap[getIdForValue(I)] = getIdForValue(Src);
    OmpEnvInfo.insertNameForVal(Src, I);
  } else if (isa<AllocaInst>(I)) {
    // ValueFlowMap[getIdForValue(I)] = 0;
  } else if (dyn_cast<DbgDeclareInst>(I)) {
    //TODO: Find if we need to handle this.
  } else if (const DbgValueInst *DbgI = dyn_cast<DbgValueInst>(I)) {
    if (DbgI->getValue() != nullptr) {
      auto IRValue = DbgI->getValue();
      OmpEnvInfo.insertNameForVal(IRValue, DbgI->getVariable()->getName().str());
      if (auto I = dyn_cast<Instruction>(IRValue)) {
        if (I->isCast()) {
          OmpEnvInfo.insertNameForVal(I->getOperand(0),
                                      DbgI->getVariable()->getName().str());
        }
      }
      // Value2NameMap[DbgI->getValue()] =  DbgI->getVariable()->getName();
    }
  }
  // print();
}

// void ValueFlowAtInstruction::run(const Instruction &ValuesAtInstruction) {
//  for (auto I = &ValuesAtInstruction; I != nullptr;
//       I = I->getPrevNonDebugInstruction()) {
//    if (auto GEP = dyn_cast<GetElementPtrInst>(I)) {
//      auto PtrOp = GEP->getPointerOperand();
//      // Since we are only concerned about the omp rtl function arguments,
//      // We know that they will have constant indices
//      // So, we only handle this case
//      if (GEP->hasAllConstantIndices()) {
//        auto IndexOpIt = (GEP->idx_end() - 1);
//        const Value *IndexOp = *IndexOpIt;
//        const unsigned ConstVal =
//            dyn_cast<Constant>(IndexOp)->getUniqueInteger().getZExtValue();
//        LLVM_DEBUG(dbgs() << "\n PtrOp:" << *PtrOp << "\n IndexOp: " <<
//        *IndexOp
//                          << "\n Const Index:" << ConstVal);
//        // This means that,
//        // after this instruction, contents of PtrOp same as I
//        // PtrOp[ConstVal] = I
//        addAlias(PtrOp, I, ConstVal);
//      }
//    } else if (auto LoadedValue = dyn_cast<LoadInst>(I)) {
//      auto LoadAddress = LoadedValue->getPointerOperand();
//      // LoadedValue = LoadAddress[0];
//      addAlias(LoadedValue, LoadAddress);
//    } else if (auto Store = dyn_cast<StoreInst>(I)) {
//      auto Address = (Store->getPointerOperand());
//      auto StoredVal = (Store->getValueOperand());
//      // Address[0] = StoredVal
//      addAlias(Address, StoredVal);
//      // AddresContent[Address] = StoredVal;
//    } else if (I->isCast()) {
//      // Only one operand
//      auto Src = I->getOperand(0);
//      // After this instruction, contents of Src same as I
//      // Src = I
//      addAlias(Src, I);
//    }
//  }
//}

void ValueFlowAtInstruction::getValueForId(IdType Id, ValueType &Val,
                                           unsigned &Index) {
  Val = nullptr;
  if (Id2ValueMap.size() <= Id)
    return;
  Val = Id2ValueMap[Id];
  for (auto Iter : Value2IdMap[Val]) {
    if (Iter.second == Id) {
      Index = Iter.first;
      break;
    }
  }
  return;
}

void ValueFlowAtInstruction::print() {
  LLVM_DEBUG(dbgs() << "\n ==== PRINTING ======");
  for (auto Iter : ValueFlowMap) {
    auto IdLhs = Iter.first;
    auto IdRhs = Iter.second;
    ValueType ValLhs = nullptr, ValRhs = nullptr;
    unsigned IndexLhs = 0, IndexRhs = 0;
    LLVM_DEBUG(dbgs() << "\n LHS: " << IdLhs << " => Rhs :" << IdRhs);
    getValueForId(IdLhs, ValLhs, IndexLhs);
    getValueForId(IdRhs, ValRhs, IndexRhs);
    if (ValLhs != nullptr)
      LLVM_DEBUG(dbgs() << "\n LHS: " << *ValLhs << " Index:" << IndexLhs);
    if (ValRhs != nullptr)
      LLVM_DEBUG(dbgs() << "\n RHS: " << *ValRhs << " Index:" << IndexRhs);
  }
  // for (auto Iter : Address2ContentMap) {
  //  LLVM_DEBUG(dbgs() << "\n Value::" << *Iter.first);
  //  for (auto Id : Iter.second)
  //    LLVM_DEBUG(dbgs() << "\t Id::" << Id);
  //}
  // LLVM_DEBUG(dbgs() << "\n ==========");
  // for (auto Iter : ValueFlowMap) {
  //  LLVM_DEBUG(dbgs() << " \n  " << Iter.first << "=>" << Iter.second);
  //  LLVM_DEBUG(dbgs() << " \n  " << *Id2ValueMap[Iter.first] << "=>"
  //                    << *Id2ValueMap[Iter.second]);
  //}
  // LLVM_DEBUG(dbgs() << "\n ==========");
}
unsigned
ValueFlowAtInstruction::get_BaseElementTypeSize(const Type *eType) const {
  LLVM_DEBUG(dbgs() << "\n Type:" << *eType);
  if (unsigned s = eType->getPrimitiveSizeInBits()) {
    return s / 8;
  }
  if (eType->isPointerTy()) {
    return get_BaseElementTypeSize(eType->getPointerElementType());
  }
  if (eType->isArrayTy()) {
    return get_BaseElementTypeSize(eType->getArrayElementType());
  }
  return 1;
}

void ValueFlowAtInstruction::parseArguments(const CallInst &OmpCall,
                                            const unsigned NumVars,
                                            const Value *BaseAddrArg,
                                            const Value *SizeAddrArg,
                                            const Value *MaptTypeArg) {
  // %21 = getelementptr inbounds [3 x i8*], [3 x i8*]* %.offload_baseptrs, i32
  // 0, i32 0, !dbg !69 %22 = getelementptr inbounds [3 x i8*], [3 x i8*]*
  // %.offload_ptrs, i32 0, i32 0, !dbg !69 %23 = call i32
  // @__tgt_target_teams(i64 0, i8*
  // @.__omp_offloading_801_15a4795_Mult_l30.region_id, i32 3, i8** %21, i8**
  // %22, i64* getelementptr inbounds ([3 x i64], [3 x i64]* @.offload_sizes,
  // i32 0, i32 0), i64* getelementptr inbounds ([3 x i64], [3 x i64]*
  // @.offload_maptypes, i32 0, i32 0), i32 0, i32 0),
  SmallVector<IdType, 5> ArrayVarsMapped;
  ArrayVarsMapped.resize(NumVars);
  auto BaseAddr = dyn_cast<GetElementPtrInst>(BaseAddrArg);
  LLVM_DEBUG(dbgs() << "\n Map type: " << *MaptTypeArg);

  // Extract the map-type constant array initializer.
  // Old LLVM (≤13): arg is a GEP into a global constant array.
  // New LLVM (≥14): arg is a direct pointer to the global (opaque ptrs).
  const Constant *MapTypeAddr = nullptr;
  if (auto *GEP = dyn_cast<GEPOperator>(MaptTypeArg)) {
    if (auto *GV = dyn_cast<GlobalVariable>(GEP->getPointerOperand()))
      MapTypeAddr = GV->getInitializer();
  } else if (auto *GV =
                 dyn_cast<GlobalVariable>(MaptTypeArg->stripPointerCasts())) {
    MapTypeAddr = GV->getInitializer();
  }
  if (!MapTypeAddr)
    return;

  // Extract the size array, if present as a global constant (old format).
  auto SizeAddrGep = dyn_cast<GEPOperator>(SizeAddrArg);
  const GlobalVariable *ConstSizeAddr = nullptr;
  if (SizeAddrGep != nullptr)
    ConstSizeAddr = dyn_cast<GlobalVariable>(SizeAddrGep->getPointerOperand());

  // Determine the base address array value tracked by VFA.
  // Old format: BaseAddrArg is a GEP into a local alloca array.
  // New format: BaseAddrArg is a direct pointer to the alloca (opaque ptrs).
  const Value *BaseAddrArray =
      (BaseAddr != nullptr) ? BaseAddr->getPointerOperand() : BaseAddrArg;
  if (BaseAddrArray == nullptr)
    return;

  for (unsigned MappedVarNum = 0; MappedVarNum < NumVars; MappedVarNum++) {
    // If VFA never saw this slot it can't help us; skip gracefully.
    if (!EXISTSinMap(Value2IdMap, BaseAddrArray) ||
        !EXISTSinMap(Value2IdMap[BaseAddrArray], MappedVarNum))
      continue;

    auto Index = MappedVarNum;
    IdType BaseAddrId = Value2IdMap[BaseAddrArray][Index];

    // IdType BaseAddrId = getIdForValue(BaseAddr->getPointerOperand(),
    // MappedVarNum); if (!getIdForValue(BaseAddrId,
    // BaseAddr->getPointerOperand(), MappedVarNum))
    assert(BaseAddrId != 0);
    if (BaseAddrId == 0)
      return;
    LLVM_DEBUG(dbgs() << "\n Baseaddr: " << BaseAddrId);
    for (; EXISTSinMap(ValueFlowMap, BaseAddrId);) {
      LLVM_DEBUG(dbgs() << "\n Get alias for Baseaddr: " << BaseAddrId);
      // bool ValIsMem = isMemory(Id2ValueMap[BaseAddrId]);
      BaseAddrId = ValueFlowMap[BaseAddrId];
      LLVM_DEBUG(dbgs() << " == > Baseaddr: " << BaseAddrId);
      LLVM_DEBUG(dbgs() << "\n Alias::" << *(Id2ValueMap[BaseAddrId]));
      if (isMemory(Id2ValueMap[BaseAddrId]))
        break;
    }
    ArrayVarsMapped[MappedVarNum] = BaseAddrId;
    const unsigned MapTypeInt = MapTypeAddr->getAggregateElement(MappedVarNum)
                                    ->getUniqueInteger()
                                    .getSExtValue();
    unsigned MappedSectionSize = 0;
    if (ConstSizeAddr != nullptr) {
      auto V = Id2ValueMap[BaseAddrId];
      // errs() <<"\n TYpe ::"<<*V->getType();
      auto TypeSize = get_BaseElementTypeSize(V->getType());
      MappedSectionSize = ConstSizeAddr->getInitializer()
                              ->getAggregateElement(MappedVarNum)
                              ->getUniqueInteger()
                              .getSExtValue();
      MappedSectionSize = MappedSectionSize / TypeSize;

    } // TODO: Handle when the section size is not a constant
    LLVM_DEBUG(dbgs() << "\n Variable:" << *(Id2ValueMap[BaseAddrId]) << "["
                      << MappedSectionSize << "]"
                      << " Mapped wtih :: " << MapTypeInt);
    auto RTLInfo = getRTLinfo(OmpCall);
    assert(RTLInfo != nullptr && "Not a data mapping RTL call ");
    LLVM_DEBUG(dbgs() << "\n EnterEnv:: " << RTLInfo->IsEnterEnv
                      << " Exit:: " << RTLInfo->IsExitEnv);
    OmpEnvInfo.addDirectiveDataMapping(OmpCall, Id2ValueMap[BaseAddrId],
                                       MapTypeInt, MappedSectionSize);
    // MappedVarSet.push_back(OmpDataMapping(Id2ValueMap[BaseAddrId],
    // MapTypeInt, MappedSectionSize)); if (RTLInfo->IsEnterEnv)
    //  OmpEnvInfo.enterDataEnv(OmpCall, Id2ValueMap[BaseAddrId], MapTypeInt,
    //  MappedSectionSize);
    // if (RTLInfo->IsExitEnv)
    //  OmpEnvInfo.exitDataEnv(OmpCall, Id2ValueMap[BaseAddrId], MapTypeInt,
    //  MappedSectionSize);
  }
}

bool OmpDiagnosticsLocalAnalysis::getRTLArgsPos(const CallInst &CI,
                                                unsigned &NumVarsPos,
                                                unsigned &BaseAddrPos,
                                                unsigned &SizePos,
                                                unsigned &MapTypePos) {
  auto Info = getRTLinfo(CI);
  if (Info != nullptr) {
    NumVarsPos = Info->NumVariablesPosition;
    BaseAddrPos = Info->VariableBaseAddressPosition;
    SizePos = Info->VariableSizePosition;
    MapTypePos = Info->MapTypePosition;
    return true;
  }
  // if (auto CalledFunc = CI.getCalledFunction() ) {
  //  auto CName = CalledFunc->getName();
  //  auto Iter = TargetRTLMap.find(CName);
  //  if ( Iter != TargetRTLMap.end()){
  //    NumVarsPos = Iter->second.NumVariablesPosition;
  //    BaseAddrPos = Iter->second.VariableBaseAddressPosition;
  //    SizePos = Iter->second.VariableSizePosition;
  //    MapTypePos = Iter->second.MapTypePosition;
  //    return true;
  //  }
  //}
  return false;
}

void OmpDiagnosticsLocalAnalysis::analyzeRTLArguments(
    const CallInst &CI, const unsigned NumVarsPos, const unsigned BaseAddrPos,
    const unsigned SizePos, const unsigned MapTypePos,
    ValueFlowAtInstruction &VFA) {
  auto NumVarsArg = CI.getArgOperand(NumVarsPos);
  auto BaseAddrArg = CI.getArgOperand(BaseAddrPos);
  auto SizeArg = CI.getArgOperand(SizePos);
  auto MaptTypeArg = CI.getArgOperand(MapTypePos);
  auto NumVars = dyn_cast<ConstantInt>(NumVarsArg);
  if (NumVars == nullptr) {
    LLVM_DEBUG(dbgs() << " \n Number of Vars not a Constant" << *NumVarsArg);
    return;
  }
  // NumVars->getUniqueInteger();
  LLVM_DEBUG(dbgs() << "\n Num Var: " << NumVars->getSExtValue()
                    << "\n Baseaddr:: " << *BaseAddrArg << "\n SizeArg::"
                    << *SizeArg << "\n Maptype::" << *MaptTypeArg);
  // ValueFlowAtInstruction VFA(CI, OmpEnvInfo);
  // VFA.run();
  VFA.print();
  VFA.parseArguments(CI, NumVars->getSExtValue(), BaseAddrArg, SizeArg,
                     MaptTypeArg);
  // DirectiveToDataMap[&CI] = MappedVarSet;
}

OmpDiagnosticsInfo &OmpDiagnosticsLocalAnalysis::run() {

  LLVM_DEBUG(dbgs() << "\n Run on func:" << Func2Analyze.getName());
  ValueFlowAtInstruction VFA(OmpEnvInfo);
  std::set<const BasicBlock *> VisitedBB;
  for (auto &I : instructions(Func2Analyze)) {
    //const BasicBlock *BB = I.getParent();
    VFA.updateInstr(I);
    // if (!EXISTSinMap(VisitedBB, BB)){
    //  auto LastInstr = BB->getTerminator();
    //  if (LastInstr != nullptr)
    //  VFA.run(*LastInstr);
    //}
    // TODO: Make sure that the order of traversing the call instructions
    // is strictly in the preorder,
    // Visit parents before traversing the child!
    if (const CallInst *OmpCall = dyn_cast<CallInst>(&I)) {
      unsigned NumVarsPos, BaseAddrPos, SizePos, MapTypePos;
      if (getRTLArgsPos(*OmpCall, NumVarsPos, BaseAddrPos, SizePos,
                        MapTypePos)) {
        LLVM_DEBUG(dbgs() << "\n OMP RTL :" << *OmpCall << "\n");
        analyzeRTLArguments(*OmpCall, NumVarsPos, BaseAddrPos, SizePos,
                            MapTypePos, VFA);
      }
    }
  }
  return OmpEnvInfo;
}
void OmpDiagnosticsLocalAnalysis::print() {}

AnalysisKey OmpDiagnosticsGlobalAnalysis::Key;

void OmpDiagnosticsGlobalAnalysis::analyzeModule(
    OmpDiagnosticsInfo &OmpEnvInfo) {
  LLVM_DEBUG(dbgs() << "\n Analysis module:");
  for (Function &Func : *ThisModule) {
    if (!Func.hasName() || Func.isIntrinsic() || Func.isDeclaration())
      continue;
    OmpDiagnosticsLocalAnalysis FuncAnalysis(Func, OmpEnvInfo);
    FuncAnalysis.run();
    LLVM_DEBUG(dbgs() << "\n Function Map::" << Func.getName());
    FuncNameMap[Func.getName().str()] = &Func;
  }
  OmpEnvInfo.print();
  const std::string MainFuncName = "main";
  if (EXISTSinMap(FuncNameMap, MainFuncName)) {
    analyzeFunction(*FuncNameMap[MainFuncName], OmpEnvInfo);
  }
}
void OmpDiagnosticsGlobalAnalysis::analyzeBasicBlock(
    const BasicBlock &BB, OmpDiagnosticsInfo &OmpEnvInfo) {
  for (auto &I : BB) {
    if (const CallInst *Call = dyn_cast<CallInst>(&I)) {
      auto CalledFunc = Call->getCalledFunction();
      if (CalledFunc == nullptr || !CalledFunc->hasName())
        continue;
      // Ignore recursive calls
      if (CalledFunc == BB.getParent())
        continue;
      if (EXISTSinMap(FuncNameMap, CalledFunc->getName().str()))
        analyzeFunction(*CalledFunc, OmpEnvInfo);
      OmpEnvInfo.processOmpRTLCall(*Call);
    }
  }
}
void OmpDiagnosticsGlobalAnalysis::analyzeFunction(
    const Function &F, OmpDiagnosticsInfo &OmpEnvInfo) {
  if (F.getName().find("__omp_offloading") != std::string::npos)
    return;
  // SmallVector<const BasicBlock*, 10> BBVisitQ;
  std::queue<const BasicBlock *> BBVisitQ;
  std::set<const BasicBlock *> BBVisitedSet;
  BBVisitQ.push(&F.getEntryBlock());
  LLVM_DEBUG(dbgs() << "\n AnalyzeFunction : " << F.getName());
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
    analyzeBasicBlock(*VisitBB, OmpEnvInfo);
  }
}
OmpDiagnosticsGlobalAnalysis::Result
OmpDiagnosticsGlobalAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
  // OmpDiagnosticsGlobalInfo result;
  ThisModule = &M;
  AnalysisManager = &AM;
  OmpDiagnosticsInfo OmpEnvInfo; // new OmpDiagnosticsInfo;
  LLVM_DEBUG(dbgs() << "\n Analysis module:");
  analyzeModule(OmpEnvInfo);
  //  OmpDiagnosticsLocalAnalysis
  //  OMPD(F);//.getResult<ScalarEvolutionAnalysis>(F)); OmpEnvInfo =
  //  &OMPD.run();
  // return result;
  // FunctionAnalysisManager &FAM =
  //    AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  // OmpDiagnosticsDataFlowAnalysis SSDFA(
  //    M, [&FAM](Function &F) -> const OmpDiagnosticsInfo & {
  //      return FAM.getResult<OmpDiagnosticsAnalysis>(F);
  //    });
  // return SSDFA.run();
  
  return OmpEnvInfo;
}

PreservedAnalyses
OmpDiagnosticsGlobalPrinterPass::run(Module &M, ModuleAnalysisManager &AM) {
  OS << "'omp diagnostics Analysis' for module '" << M.getName() << "'\n";
  OmpDiagnosticsInfo Res = AM.getResult<OmpDiagnosticsGlobalAnalysis>(M);
  Res.print(OS);
  return PreservedAnalyses::all();
}

// bool OmpDiagnosticsGlobalInfoWrapperPass::runOnModule(Module &M) {
//  //OmpDiagnosticsDataFlowAnalysis SSDFA(
//  //    M, [this](Function &F) -> const OmpDiagnosticsInfo & {
//  //      return getAnalysis<OmpDiagnosticsInfoWrapperPass>(F).getResult();
//  //    });
//  //SSI = SSDFA.run();
////bool OmpDiagnosticsInfoWrapperPass::runOnFunction(Function &F) {
////  OmpDiagnosticsLocalAnalysis
/// OMPD(F);//.getResult<ScalarEvolutionAnalysis>(F)); /  OmpEnvInfo =
///&OMPD.run();
//  std::map<std::string, Function *> FuncNameMap;
//  for (Function &Func : M) {
//    if (!Func.hasName() ||
//        Func.isIntrinsic() ||
//        Func.isDeclaration()) continue;
//    FuncNameMap[Func.getName()] = &Func;
//  }
//  if (EXISTSinMap(FuncNameMap, "main")){
//  }
//  return false;
//}

static const char LocalPassArg[] = "omp-diagnostics-local";
static const char LocalPassName[] = "omp diagnostics Local Analysis";
static const char GlobalPassName[] = "omp diagnostics Analysis";



// pasted the OMP RTL declarations here
// ===========================================================
// int omp_get_num_devices(void);
// int omp_get_initial_device(void);
// void *omp_target_alloc(size_t size, int device_num);
// void omp_target_free(void *device_ptr, int device_num);
// int omp_target_is_present(void *ptr, int device_num);
// int omp_target_memcpy(void *dst, void *src, size_t length, size_t dst_offset,
//    size_t src_offset, int dst_device, int src_device);
// int omp_target_memcpy_rect(void *dst, void *src, size_t element_size,
//    int num_dims, const size_t *volume, const size_t *dst_offsets,
//    const size_t *src_offsets, const size_t *dst_dimensions,
//    const size_t *src_dimensions, int dst_device, int src_device);
// int omp_target_associate_ptr(void *host_ptr, void *device_ptr, size_t size,
//    size_t device_offset, int device_num);
// int omp_target_disassociate_ptr(void *host_ptr, int device_num);
//
///// adds a target shared library to the target execution image
// void __tgt_register_lib(__tgt_bin_desc *desc);
//
///// removes a target shared library from the target execution image
// void __tgt_unregister_lib(__tgt_bin_desc *desc);
//
//// creates the host to target data mapping, stores it in the
//// libomptarget.so internal structure (an entry in a stack of data maps) and
//// passes the data to the device;
// void __tgt_target_data_begin(int64_t device_id, int32_t arg_num,
//                             void **args_base, void **args, int64_t
//                             *arg_sizes, int64_t *arg_types);
// void __tgt_target_data_begin_nowait(int64_t device_id, int32_t arg_num,
//                                    void **args_base, void **args,
//                                    int64_t *arg_sizes, int64_t *arg_types,
//                                    int32_t depNum, void *depList,
//                                    int32_t noAliasDepNum,
//                                    void *noAliasDepList);
//
//// passes data from the target, release target memory and destroys the
//// host-target mapping (top entry from the stack of data maps) created by
//// the last __tgt_target_data_begin
// void __tgt_target_data_end(int64_t device_id, int32_t arg_num, void
// **args_base,
//                           void **args, int64_t *arg_sizes, int64_t
//                           *arg_types);
// void __tgt_target_data_end_nowait(int64_t device_id, int32_t arg_num,
//                                  void **args_base, void **args,
//                                  int64_t *arg_sizes, int64_t *arg_types,
//                                  int32_t depNum, void *depList,
//                                  int32_t noAliasDepNum, void
//                                  *noAliasDepList);
//
///// passes data to/from the target
// void __tgt_target_data_update(int64_t device_id, int32_t arg_num,
//                              void **args_base, void **args, int64_t
//                              *arg_sizes, int64_t *arg_types);
// void __tgt_target_data_update_nowait(int64_t device_id, int32_t arg_num,
//                                     void **args_base, void **args,
//                                     int64_t *arg_sizes, int64_t *arg_types,
//                                     int32_t depNum, void *depList,
//                                     int32_t noAliasDepNum,
//                                     void *noAliasDepList);
//
//// Performs the same actions as data_begin in case arg_num is non-zero
//// and initiates run of offloaded region on target platform; if arg_num
//// is non-zero after the region execution is done it also performs the
//// same action as data_end above. The following types are used; this
//// function returns 0 if it was able to transfer the execution to a
//// target and an int different from zero otherwise.
// int __tgt_target(int64_t device_id, void *host_ptr, int32_t arg_num,
//                 void **args_base, void **args, int64_t *arg_sizes,
//                 int64_t *arg_types);
// int __tgt_target_nowait(int64_t device_id, void *host_ptr, int32_t arg_num,
//                        void **args_base, void **args, int64_t *arg_sizes,
//                        int64_t *arg_types, int32_t depNum, void *depList,
//                        int32_t noAliasDepNum, void *noAliasDepList);
//
// int __tgt_target_teams(int64_t device_id, void *host_ptr, int32_t arg_num,
//                       void **args_base, void **args, int64_t *arg_sizes,
//                       int64_t *arg_types, int32_t num_teams,
//                       int32_t thread_limit);
// int __tgt_target_teams_nowait(int64_t device_id, void *host_ptr,
//                              int32_t arg_num, void **args_base, void **args,
//                              int64_t *arg_sizes, int64_t *arg_types,
//                              int32_t num_teams, int32_t thread_limit,
//                              int32_t depNum, void *depList,
//                              int32_t noAliasDepNum, void *noAliasDepList);
