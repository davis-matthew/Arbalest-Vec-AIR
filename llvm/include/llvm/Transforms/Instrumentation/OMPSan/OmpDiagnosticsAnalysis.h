//===- OmpDiagnosticsAnalysis.h - Analyze omp target clauses and infer data
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

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_OMPSAN_OMPDIAGNOSTICSANALYSIS_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_OMPSAN_OMPDIAGNOSTICSANALYSIS_H

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Operator.h"
#include "llvm/Pass.h"

#include <queue>
#include <set>
#include <map>

namespace llvm {

using namespace llvm;
// Copied the enum for map type from openmp/libomptarget/include/omptarget.h
// TODO: Can we include the above file ?
// enum tgt_map_type {
//   // No flags
//   OMP_TGT_MAPTYPE_NONE = 0x000,
//   // copy data from host to device
//   OMP_TGT_MAPTYPE_TO = 0x001,
//   // copy data from device to host
//   OMP_TGT_MAPTYPE_FROM = 0x002,
//   // copy regardless of the reference count
//   OMP_TGT_MAPTYPE_ALWAYS = 0x004,
//   // force unmapping of data
//   OMP_TGT_MAPTYPE_DELETE = 0x008,
//   // map the pointer as well as the pointee
//   OMP_TGT_MAPTYPE_PTR_AND_OBJ = 0x010,
//   // pass device base address to kernel
//   OMP_TGT_MAPTYPE_TARGET_PARAM = 0x020,
//   // return base device address of mapped data
//   OMP_TGT_MAPTYPE_RETURN_PARAM = 0x040,
//   // private variable - not mapped
//   OMP_TGT_MAPTYPE_PRIVATE = 0x080,
//   // copy by value - not mapped
//   OMP_TGT_MAPTYPE_LITERAL = 0x100,
//   // mapping is implicit
//   OMP_TGT_MAPTYPE_IMPLICIT = 0x200,
//   // member of struct, member given by [16 MSBs] - 1
//   OMP_TGT_MAPTYPE_MEMBER_OF = 0xffff000000000000
// };
/// Represents the string type of Omp RTL call names
using ConstCallNameType = const std::string;
/// Represents the information associated with each RTL call.
/// Each RTL call can have different arguments,
/// this struct records which argument is what, by recording the argument number
/// of parameters required for inferring the data mapping
struct RTLinfoType {
  using ArgPos = const unsigned;
  /// Some RTL calls are not relevant for data mapping,
  /// use this flag to ignore them.
  bool IsDataMappingRTL, IsEnterEnv, IsExitEnv;
  /// The four arguments that we are interested in,
  /// Number of variables mapped.
  /// BaseAddress of the variable mapped.
  /// Size of the array section mapped.
  /// Type of mapping specified.
  ArgPos NumVariablesPosition, VariableBaseAddressPosition,
      VariableSizePosition, MapTypePosition;
  RTLinfoType()
      : IsDataMappingRTL(false), IsEnterEnv(false), IsExitEnv(false),
        NumVariablesPosition(0), VariableBaseAddressPosition(0),
        VariableSizePosition(0), MapTypePosition(0) {}
  RTLinfoType(ArgPos A, ArgPos B, ArgPos C, ArgPos D, bool IsEnter, bool IsExit)
      : IsDataMappingRTL(true), IsEnterEnv(IsEnter), IsExitEnv(IsExit),
        NumVariablesPosition(A), VariableBaseAddressPosition(B),
        VariableSizePosition(C), MapTypePosition(D) {}
};
/// Map of RTL call to its arguments
using RTL2infoMapType = const std::map<ConstCallNameType, const RTLinfoType>;
/// This map provides information about the meaning of each argument
/// of the RTL calls,
/// Our Analysis will look at the arguments to analyze the semantics of the
/// RTL calls, and infer the data mapping according to OMP 5.0
RTL2infoMapType TargetRTLMap = {
    //"omp_get_num_devices",
    //"omp_get_initial_device",
    //"omp_target_alloc",
    //"omp_target_free",
    //"omp_target_is_present",
    //"omp_target_memcpy",
    //"omp_target_memcpy_rect",
    //"omp_target_associate_ptr",
    //"omp_target_disassociate_ptr",
    //{"__tgt_register_lib",RTLinfoType( ) },
    //{"__tgt_unregister_lib",RTLinfoType( ) },
    // Legacy API (LLVM ≤13): no ident_t* prefix
    {"__tgt_target_data_begin", RTLinfoType(1, 2, 4, 5, true, false)},
    {"__tgt_target_data_end", RTLinfoType(1, 2, 4, 5, false, true)},
    {"__tgt_target_data_update", RTLinfoType(1, 2, 4, 5, true, true)},
    {"__tgt_target", RTLinfoType(2, 3, 5, 6, true, true)},
    {"__tgt_target_teams", RTLinfoType(2, 3, 5, 6, true, true)},
    {"__tgt_target_data_begin_nowait", RTLinfoType(1, 2, 4, 5, true, false)},
    {"__tgt_target_data_end_nowait", RTLinfoType(1, 2, 4, 5, false, true)},
    {"__tgt_target_data_update_nowait", RTLinfoType(1, 2, 4, 5, true, true)},
    {"__tgt_target_nowait", RTLinfoType(2, 3, 5, 6, true, true)},
    // Mapper API (LLVM ≥14): prepends ident_t* loc at arg 0, appends
    // arg_names/arg_mappers at args 7-8.  arg_num→2, args_base→3,
    // arg_sizes→5, arg_types→6.
    {"__tgt_target_data_begin_mapper", RTLinfoType(2, 3, 5, 6, true, false)},
    {"__tgt_target_data_end_mapper", RTLinfoType(2, 3, 5, 6, false, true)},
    {"__tgt_target_data_update_mapper", RTLinfoType(2, 3, 5, 6, true, true)},
    {"__tgt_target_data_begin_mapper_nowait", RTLinfoType(2, 3, 5, 6, true, false)},
    {"__tgt_target_data_end_mapper_nowait", RTLinfoType(2, 3, 5, 6, false, true)},
    //"__tgt_target_teams_nowait",RTLinfoType(2,3,5,6 ) },
    //"__tgt_target_data_begin_depend",RTLinfoType(1,2,4,5 ) },
    //"__tgt_target_data_end_depend",RTLinfoType(1,2,4,5 ) },
    //"__tgt_target_data_update_depend",RTLinfoType(1,2,4,5 ) },
    //"__tgt_target_data_begin_nowait_depend",
    //"__tgt_target_data_end_nowait_depend",
    //"__tgt_target_data_update_nowait_depend",
};

/// The information required to be extracted from the Omp RTL calls.
/// The Value* that is mapped, the map type, and the array section size.
struct OmpDataMapping {
  const Value *MappedValue;
  unsigned MapTypeInt;
  const unsigned MappedSectionSize;
  OmpDataMapping(const Value *V, const unsigned T, const unsigned S)
      : MappedValue(V), MapTypeInt(T), MappedSectionSize(S) {}
  OmpDataMapping(const OmpDataMapping &O)
      : MappedValue(O.MappedValue), MapTypeInt(O.MapTypeInt),
        MappedSectionSize(O.MappedSectionSize) {}
  // cannot be copied/assigned, since the members are all constant
  // OmpDataMapping & operator=(OmpDataMapping &O)
};

#ifndef EXISTSinMap
#define EXISTSinMap(MAP, ELEM) (MAP.find(ELEM) != MAP.end())
#endif

  using IdType = const Value *;
  /// Map of an Item represented by the Id to its reference count
  using ItemsRefCountType = std::map<IdType, unsigned>;
  using DeviceEnvironmentsType = SmallVector<ItemsRefCountType, 1>;
  using DataMappingSetType = std::vector<OmpDataMapping>;
  using InstructionToMemCopyMapType =
      std::map<const Instruction *, DataMappingSetType>;
  using OmpDirectiveDataMapType =
      std::map<const CallInst *, DataMappingSetType>;
/// This is the type of object that our analysis returns.
/// It contains the mapping information for every Omp RTL call.
/// It contains the host-device and device-host memory copies introduced.
/// Note for future:This might be too much information to be returned.
class OmpDiagnosticsInfo {
  // friend class ValueFlowAtInstruction;
private:

  /// Contains the reference count of every mapped  variable.
  DeviceEnvironmentsType DeviceEnvironments;
  /// The map of Omp RTL calls to the Value that is allocated on device.
  InstructionToMemCopyMapType AllocatedItems;
  /// The map of Omp RTL calls to the Value that is copied from host to device.
  InstructionToMemCopyMapType HostDeviceCopy;
  /// The map of Omp RTL calls to the Value that is copied from device to host.
  InstructionToMemCopyMapType DeviceHostCopy;
  /// The map of Omp RTL calls to the Value that is persistent on device.
  InstructionToMemCopyMapType DevicePersistentIn;
  InstructionToMemCopyMapType DevicePersistentOut;

  /// Map of Omp RTL to the {Value*, MapType, and Array section size}.
  OmpDirectiveDataMapType DirectiveToDataMap;

  std::map<const Value *, std::string> Value2NameMap;

  bool alreadyInserted(DataMappingSetType &DM, OmpDataMapping &I) {
    for (auto D : DM) {
      if (D.MappedValue == I.MappedValue)
        return true;
    }
    return false;
  }

public:
  OmpDiagnosticsInfo();
  ~OmpDiagnosticsInfo();
  /// Symbolically interpret the effect of executing this OpenMP RTL. Depending
  /// on which function is called, it will inturn call the enter or exit data
  /// env function. The DirectiveToDataMap member map is accessed to use the
  /// stored information.
  void processOmpRTLCall(const CallInst &CI);
  /// Interpret the effect of enter data environment, according to OpenMP
  /// semantics. It will either create a host-device memory copy or ignore the
  /// call.
  void enterDataEnv(const Instruction &OmpCall, OmpDataMapping &MappedVar,
                    unsigned deviceid = 0);
  // IdType ItemId, unsigned MapTypeInt, unsigned ItemSize, unsigned
  // deviceid=0);
  /// Interpret the effect of exit data environment, according to OpenMP
  /// semantics. It will either create a device-host memory copy or ignore the
  /// call.
  void exitDataEnv(const Instruction &OmpCall, OmpDataMapping &MappedVar,
                   unsigned deviceid = 0);
  // void addDirectiveDataMapping(const CallInst &CI, const OmpDataMapping &DM
  // );
  void addDirectiveDataMapping(const CallInst &CI, const Value *V,
                               const unsigned T, const unsigned S);

  bool filterId(const IdType &ID) const;
  std::string getName(const IdType &ID) const;
  // TODO: Add useful for client methods.
  void print(raw_ostream &O) const;
  void print() const;
void printCopies(const InstructionToMemCopyMapType &ItoCopy, const std::string &CopyKind, raw_ostream &O) const;
  void insertNameForVal(const Value *V, const std::string &Name);
  void insertNameForVal(const Value *From, const Value *To);
  /// Get the map of Omp RTL calls to the Value that is allocated on device.
  const InstructionToMemCopyMapType &getAllocatedItems(){
    return AllocatedItems;
  }
  /// Get the map of Omp RTL calls to the Value that is copied from host to device.
  const InstructionToMemCopyMapType &getHostDeviceCopy(){
    return HostDeviceCopy;
  }
  /// Get the map of Omp RTL calls to the Value that is copied from device to host.
  const InstructionToMemCopyMapType &getDeviceHostCopy(){
    return DeviceHostCopy;
  }
  /// Get the map of Omp RTL calls to the Value that is persistent on device.
  const InstructionToMemCopyMapType &getDevicePersistentIn(){
    return DevicePersistentIn;
  }

  const InstructionToMemCopyMapType &getDevicePersistentOut(){
    return DevicePersistentOut;
  }

  
};

/// Used to analyze the last written contents of every Value* within the
/// basicblock This works here since the values passed to the Omp RTL are
/// assigned values within the same basicblock.
class ValueFlowAtInstruction {
public:
  using IdType = unsigned;
  using ValueType = const Value *;
  using VectorOfIdsType = std::map<unsigned, IdType>;
  using VectorOfValuesType = std::vector<ValueType>;
  // using AddressContentsdMapType = std::map<const Value *, VectorOfIdsType>;
  // using ValueFlowMapType = std::map<IdType, IdType>;
  // using Id2ValueMapType = std::map<IdType, const Value*>;//std::vector<const
  // Value *>;
  using Id2ValueMapType = std::vector<const Value *>;
  using Value2IdMapType =
      std::map<const Value *, VectorOfIdsType>; // std::vector<const Value *>;
  // using ValueFlowMapType = std::map<IdType, VectorOfIdsType>;
  using ValueFlowMapType = std::map<IdType, IdType>;

private:
  // This is a map from a value to an array of Ids,
  // Where Each Id from the array of Ids refers to the
  // corresponding offset from the Value,
  // So, A->[8,9,10], means,
  // A[0] has an id of 8, A[1] of 9 and so on.
  // AddressContentsdMapType Address2ContentMap;
  Id2ValueMapType Id2ValueMap;
  Value2IdMapType Value2IdMap;
  // ValueFlowMap is used to store Identifiers whose contents are same
  // entry maybe a pointer, and ValueFlowMap[Ptr1] = Ptr2 means, both Ptr1 and
  // Ptr2 point to same address ValueFlowMap[Addres] = V, Means that the
  // contents at Address is V ValueFlowMap[Address1] = &Address2, Means that
  // Address1 is a double pointer, and contains Address2
  ValueFlowMapType ValueFlowMap;
  // AddresContentType AddresContent;
  // const Instruction *ValuesAtInstruction;
  OmpDiagnosticsInfo &OmpEnvInfo;

  IdType getIdForValue(const Value *V, const unsigned Index = 0);
  // bool getIdForValue(IdType &Id, const Value *V, const unsigned Index);
  // void addAlias(const Value *, const Value *, unsigned Index = 0);
  bool isMemory(ValueType V);
  void getValueForId(IdType Id, ValueType &Val, unsigned &Index);

  bool getGep(const Value *GEP, ValueType &PtrOp, unsigned &Index);

public:
  ValueFlowAtInstruction(OmpDiagnosticsInfo &O) : OmpEnvInfo(O) {
    Id2ValueMap.resize(1);
  }
  // ValueFlowAtInstruction(const Instruction &I, OmpDiagnosticsInfo &O)  :
  // ValuesAtInstruction(&I), OmpEnvInfo(O) {}
  void updateInstr(const Instruction &ValuesAtInstruction);
  // void run(const Instruction &ValuesAtInstruction);
  void print();
  unsigned get_BaseElementTypeSize(const Type *eType) const;
  void parseArguments(const CallInst &, const unsigned NumVars,
                      const Value *BaseAddrArg, const Value *SizeArg,
                      const Value *MaptTypeArg);
};

/// Iterate over every Omp RTL call instruction, and parse its arguments, to
/// deduce the data mapping pragma used by the programmer. Then record all the
/// variables mapped and its mapping properties for every Omp RTL call.
class OmpDiagnosticsLocalAnalysis {
  const Function &Func2Analyze;
  OmpDiagnosticsInfo &OmpEnvInfo;

  // RTLinfoType records what each argument of the RTL calls means
  // We are only concerned about the following 4 arguments,
  // These are the argument numbers of the interesting properties
  bool getRTLArgsPos(const CallInst &CI, unsigned &NumVarsPos,
                     unsigned &BaseAddrPos, unsigned &SizePos,
                     unsigned &MapTypePos);
  void analyzeRTLArguments(const CallInst &CI, const unsigned NumVarsPos,
                           const unsigned BaseAddrPos, const unsigned SizePos,
                           const unsigned MapTypePos,
                           ValueFlowAtInstruction &VFA);

public:
  /// Updates the \p OInfo, with the mapping informa
  OmpDiagnosticsLocalAnalysis(const Function &F, OmpDiagnosticsInfo &OInfo)
      : Func2Analyze(F), OmpEnvInfo(OInfo) {}
  // TODO : make sure the order of traversal of Basicblocks is correct, preorder
  // traversal only
  OmpDiagnosticsInfo &run();
  void print();
};

/// This pass performs the global (interprocedural) Omp Data Mapping Analysis.
/// (New pass manager).
class OmpDiagnosticsGlobalAnalysis
    : public AnalysisInfoMixin<OmpDiagnosticsGlobalAnalysis> {
  friend AnalysisInfoMixin<OmpDiagnosticsGlobalAnalysis>;
  static AnalysisKey Key;
  Module *ThisModule;
  ModuleAnalysisManager *AnalysisManager;

  void analyzeModule(OmpDiagnosticsInfo &OmpEnvInfo);
  void analyzeFunction(const Function &F, OmpDiagnosticsInfo &OmpEnvInfo);
  void analyzeBasicBlock(const BasicBlock &BB, OmpDiagnosticsInfo &OmpEnvInfo);
  std::map<std::string, Function *> FuncNameMap;

public:
  using Result = OmpDiagnosticsInfo;
  Result run(Module &M, ModuleAnalysisManager &AM);
};

/// Printer pass for the \c OmpDiagnosticsGlobalAnalysis results.
class OmpDiagnosticsGlobalPrinterPass
    : public PassInfoMixin<OmpDiagnosticsGlobalPrinterPass> {
  raw_ostream &OS;

public:
  explicit OmpDiagnosticsGlobalPrinterPass(raw_ostream &OS) : OS(OS) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

/// This pass performs the global (interprocedural) Omp Data Mapping Analysis.
/// (legacy pass manager).
// class OmpDiagnosticsGlobalInfoWrapperPass : public ModulePass {
//  OmpDiagnosticsGlobalInfo SSI;
//
// public:
//  static char ID;
//
//  OmpDiagnosticsGlobalInfoWrapperPass();
//
//  const OmpDiagnosticsGlobalInfo &getResult() const { return SSI; }
//
//  void print(raw_ostream &O, const Module *M) const override;
//  void getAnalysisUsage(AnalysisUsage &AU) const override;
//
//  bool runOnModule(Module &M) override;
//};

} // end namespace llvm

#endif // LLVM_ANALYSIS_OmpDiagnosticsANALYSIS_H
