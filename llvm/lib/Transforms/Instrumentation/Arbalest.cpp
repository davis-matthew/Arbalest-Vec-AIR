//===-- Arbalest.cpp - Arbalest data inconsistency detector ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Arbalest is a data inconsistency detector for OpenMP offloading applications.
// It emits __arbalest_* runtime callbacks on memory accesses and GEPs and
// shares ThreadSanitizer's runtime library (compiler-rt/lib/tsan). This pass
// previously lived inside ThreadSanitizer.cpp; it has been split out so it can
// be scheduled and reasoned about independently.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/Arbalest.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

#define DEBUG_TYPE "arbalest"

static cl::opt<bool>
    ClEnableArbalest("arbalest", cl::init(false),
                     cl::desc("Run Arbalest data inconsistency detector"),
                     cl::Hidden);

static cl::opt<bool> ClOMPDebugMode(
    "arbalest-debug-info", cl::init(false),
    cl::desc("Instrument OpenMP outlined functions with debug info"),
    cl::Hidden);

namespace {

constexpr char kArbalestModuleCtorName[] = "arbalest.module_ctor";
constexpr char kArbalestInitName[] = "__arbalest_init";
constexpr char kOmpOutlinedFuncPrefixFlag[] = "OmpOutlinedFuncPrefix";

constexpr size_t kNumberOfAccessSizes = 5;

class Arbalest {
public:
  Arbalest() = default;
  bool sanitizeFunction(Function &F);

private:
  void initialize(Module &M);
  void chooseInstructionsToInstrument(SmallVectorImpl<Instruction *> &Local,
                                      SmallVectorImpl<Instruction *> &All);
  bool instrumentLoadOrStore(Instruction *I, const DataLayout &DL);
  bool instrumentGEP(GetElementPtrInst *GEP, const DataLayout &DL);

  FunctionCallee ArbalestRead[kNumberOfAccessSizes];
  FunctionCallee ArbalestWrite[kNumberOfAccessSizes];
  FunctionCallee ArbalestUnalignedRead[kNumberOfAccessSizes];
  FunctionCallee ArbalestUnalignedWrite[kNumberOfAccessSizes];
  FunctionCallee ArbalestCheckBound;
  StringRef OutlinedFuncPrefix;
};

bool isVtableAccess(Instruction *I) {
  if (MDNode *Tag = I->getMetadata(LLVMContext::MD_tbaa))
    return Tag->isTBAAVtableAccess();
  return false;
}

bool shouldInstrumentReadWriteFromAddress(const Module *M, Value *Addr) {
  Addr = Addr->stripInBoundsOffsets();

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr)) {
    if (GV->hasSection()) {
      StringRef SectionName = GV->getSection();
      auto OF = Triple(M->getTargetTriple()).getObjectFormat();
      if (SectionName.endswith(
              getInstrProfSectionName(IPSK_cnts, OF, /*AddSegmentInfo=*/false)))
        return false;
    }
    if (GV->getName().startswith("__llvm_gcov") ||
        GV->getName().startswith("__llvm_gcda"))
      return false;
  }

  if (Addr) {
    Type *PtrTy = cast<PointerType>(Addr->getType()->getScalarType());
    if (PtrTy->getPointerAddressSpace() != 0)
      return false;
  }
  return true;
}

bool addrPointsToConstantData(Value *Addr) {
  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Addr))
    Addr = GEP->getPointerOperand();
  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr)) {
    if (GV->isConstant())
      return true;
  } else if (LoadInst *L = dyn_cast<LoadInst>(Addr)) {
    if (isVtableAccess(L))
      return true;
  }
  return false;
}

int getMemoryAccessFuncIndex(Type *OrigTy, Value *Addr, const DataLayout &DL) {
  assert(OrigTy->isSized());
  assert(
      cast<PointerType>(Addr->getType())->isOpaqueOrPointeeTypeMatches(OrigTy));
  uint32_t TypeSize = DL.getTypeStoreSizeInBits(OrigTy);
  if (TypeSize != 8 && TypeSize != 16 && TypeSize != 32 && TypeSize != 64 &&
      TypeSize != 128)
    return -1;
  size_t Idx = countTrailingZeros(TypeSize / 8);
  assert(Idx < kNumberOfAccessSizes);
  return Idx;
}

int getMemoryAccessSize(Type *OrigTy, const DataLayout &DL) {
  assert(OrigTy->isSized());
  uint32_t TypeSize = DL.getTypeStoreSizeInBits(OrigTy);
  if (TypeSize != 8 && TypeSize != 16 && TypeSize != 32 && TypeSize != 64 &&
      TypeSize != 128)
    return -1;
  return TypeSize / 8;
}

uint32_t insertGlobalVariableInfo(Module &M,
                                  SmallVector<Constant *> &GlobInfo) {
  SmallVector<GlobalVariable *, 8> UserDefinedGlobs;
  for (GlobalVariable &G : M.globals()) {
    if (!G.getName().empty() && !G.getName().startswith(".") &&
        !G.getName().startswith("llvm")) {
      UserDefinedGlobs.push_back(&G);
    }
  }
  if (UserDefinedGlobs.empty())
    return 0;

  SmallVector<uint64_t, 8> GVS;
  SmallVector<Constant *, 8> GVN;
  SmallVector<Constant *, 8> GV;
  for (GlobalVariable *GP : UserDefinedGlobs) {
    GVS.push_back(
        M.getDataLayout().getTypeStoreSize(GP->getValueType()).getFixedSize());
    Constant *VarNameInitializer =
        ConstantDataArray::getString(M.getContext(), GP->getName());
    GlobalVariable *VarName = new GlobalVariable(
        M, VarNameInitializer->getType(), false, GlobalValue::PrivateLinkage,
        VarNameInitializer);
    GVN.push_back(VarName);
    GV.push_back(GP);
  }
  ArrayType *SizeArrayTy =
      ArrayType::get(Type::getInt64Ty(M.getContext()), GVS.size());
  ArrayType *PtrArrayTy = ArrayType::get(
      Type::getVoidTy(M.getContext())->getPointerTo(), GV.size());
  ArrayType *NameArrayTy = ArrayType::get(
      Type::getInt8Ty(M.getContext())->getPointerTo(), GVN.size());
  GlobalVariable *GlobalsSize = new GlobalVariable(
      M, SizeArrayTy, false, GlobalValue::PrivateLinkage,
      ConstantDataArray::get(M.getContext(), ArrayRef<uint64_t>(GVS)),
      "arbalest_global_size");
  GlobalVariable *GlobalsName = new GlobalVariable(
      M, NameArrayTy, false, GlobalValue::PrivateLinkage,
      ConstantArray::get(NameArrayTy, ArrayRef<Constant *>(GVN)),
      "arbalest_global_name");
  GlobalVariable *Globals = new GlobalVariable(
      M, PtrArrayTy, false, GlobalValue::PrivateLinkage,
      ConstantArray::get(PtrArrayTy, ArrayRef<Constant *>(GV)),
      "arbalest_global_ptr");
  GlobInfo[0] = Globals;
  GlobInfo[1] = GlobalsSize;
  GlobInfo[2] = GlobalsName;
  return UserDefinedGlobs.size();
}

void insertArbalestCtor(Module &M) {
  bool IsHostModule = (M.getTargetTriple() == "x86_64-unknown-linux-gnu");
  if (!IsHostModule)
    return;

  errs() << "Turn on Arbalest-related instrumentation"
         << (ClOMPDebugMode ? " with" : " without") << " debug info\n";

  SmallVector<Constant *> GlobInfo{nullptr, nullptr, nullptr};
  uint32_t UserDefinedGlobNum = insertGlobalVariableInfo(M, GlobInfo);

  IntegerType *U32 = Type::getInt32Ty(M.getContext());
  PointerType *PtrPtr =
      Type::getVoidTy(M.getContext())->getPointerTo()->getPointerTo();
  PointerType *U64Ptr = Type::getInt64PtrTy(M.getContext());
  PointerType *StrPtr = Type::getInt8PtrTy(M.getContext())->getPointerTo();
  getOrCreateSanitizerCtorAndInitFunctions(
      M, kArbalestModuleCtorName, kArbalestInitName,
      /*InitArgTypes=*/{U32, PtrPtr, U64Ptr, StrPtr},
      /*InitArgs=*/
      {ConstantInt::get(U32, UserDefinedGlobNum),
       GlobInfo[0] ? GlobInfo[0] : ConstantPointerNull::get(PtrPtr),
       GlobInfo[1] ? GlobInfo[1] : ConstantPointerNull::get(U64Ptr),
       GlobInfo[2] ? GlobInfo[2] : ConstantPointerNull::get(StrPtr)},
      [&](Function *Ctor, FunctionCallee) {
        appendToGlobalCtors(M, Ctor, 0);
      });
}

void setOmpOutlinedFuncPrefix(Module &M) {
  StringRef OptPrefix = ".omp_outlined";
  StringRef DebugPrefix = ".omp_outlined._debug__";
  bool UseOptPrefix = true;
  if (ClOMPDebugMode) {
    for (auto &Func : M) {
      if (Func.getName().startswith(DebugPrefix)) {
        UseOptPrefix = false;
        break;
      }
    }
  }
  M.addModuleFlag(Module::Error, kOmpOutlinedFuncPrefixFlag,
                  MDString::get(M.getContext(),
                                UseOptPrefix ? OptPrefix : DebugPrefix));
}

} // namespace

PreservedAnalyses ModuleArbalestPass::run(Module &M, ModuleAnalysisManager &) {
  if (!ClEnableArbalest)
    return PreservedAnalyses::all();
  insertArbalestCtor(M);
  setOmpOutlinedFuncPrefix(M);
  return PreservedAnalyses::none();
}

PreservedAnalyses ArbalestPass::run(Function &F, FunctionAnalysisManager &) {
  if (!ClEnableArbalest)
    return PreservedAnalyses::all();
  Arbalest A;
  if (A.sanitizeFunction(F))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

bool Arbalest::sanitizeFunction(Function &F) {
  if (F.getName() == kArbalestModuleCtorName)
    return false;
  if (F.hasFnAttribute(Attribute::Naked))
    return false;
  if (F.hasFnAttribute(Attribute::DisableSanitizerInstrumentation))
    return false;

  initialize(*F.getParent());

  const DataLayout &DL = F.getParent()->getDataLayout();
  SmallVector<Instruction *, 8> AllLoadsAndStores;
  SmallVector<Instruction *, 8> LocalLoadsAndStores;

  for (auto &BB : F) {
    for (auto &Inst : BB) {
      if (isa<LoadInst>(Inst) || isa<StoreInst>(Inst)) {
        LocalLoadsAndStores.push_back(&Inst);
      } else if ((isa<CallInst>(Inst) && !isa<DbgInfoIntrinsic>(Inst)) ||
                 isa<InvokeInst>(Inst)) {
        chooseInstructionsToInstrument(LocalLoadsAndStores, AllLoadsAndStores);
      }
    }
    chooseInstructionsToInstrument(LocalLoadsAndStores, AllLoadsAndStores);
  }

  bool Res = false;
  for (auto *Inst : AllLoadsAndStores)
    Res |= instrumentLoadOrStore(Inst, DL);

  if (F.getName().startswith(OutlinedFuncPrefix)) {
    for (auto &BB : F) {
      for (auto &Inst : BB) {
        if (auto *GEP = dyn_cast<GetElementPtrInst>(&Inst))
          Res |= instrumentGEP(GEP, DL);
      }
    }
  }
  return Res;
}

void Arbalest::initialize(Module &M) {
  IRBuilder<> IRB(M.getContext());
  AttributeList Attr;
  Attr = Attr.addFnAttribute(M.getContext(), Attribute::NoUnwind);

  if (auto *Flag = M.getModuleFlag(kOmpOutlinedFuncPrefixFlag))
    OutlinedFuncPrefix = cast<MDString>(Flag)->getString();

  for (size_t i = 0; i < kNumberOfAccessSizes; ++i) {
    const unsigned ByteSize = 1U << i;
    std::string ByteSizeStr = utostr(ByteSize);

    SmallString<32> ReadName("__arbalest_read" + ByteSizeStr);
    ArbalestRead[i] = M.getOrInsertFunction(ReadName, Attr, IRB.getVoidTy(),
                                            IRB.getInt8PtrTy());

    SmallString<32> WriteName("__arbalest_write" + ByteSizeStr);
    ArbalestWrite[i] = M.getOrInsertFunction(WriteName, Attr, IRB.getVoidTy(),
                                             IRB.getInt8PtrTy());

    SmallString<64> UnalignedReadName("__arbalest_unaligned_read" +
                                      ByteSizeStr);
    ArbalestUnalignedRead[i] = M.getOrInsertFunction(
        UnalignedReadName, Attr, IRB.getVoidTy(), IRB.getInt8PtrTy());

    SmallString<64> UnalignedWriteName("__arbalest_unaligned_write" +
                                       ByteSizeStr);
    ArbalestUnalignedWrite[i] = M.getOrInsertFunction(
        UnalignedWriteName, Attr, IRB.getVoidTy(), IRB.getInt8PtrTy());
  }
  ArbalestCheckBound = M.getOrInsertFunction(
      "__arbalest_check_bound", Attr, IRB.getVoidTy(), IRB.getInt8PtrTy(),
      IRB.getInt8PtrTy(), IRB.getInt32Ty());
}

void Arbalest::chooseInstructionsToInstrument(
    SmallVectorImpl<Instruction *> &Local,
    SmallVectorImpl<Instruction *> &All) {
  for (Instruction *I : reverse(Local)) {
    const bool IsWrite = isa<StoreInst>(*I);
    Value *Addr = IsWrite ? cast<StoreInst>(I)->getPointerOperand()
                          : cast<LoadInst>(I)->getPointerOperand();

    if (!shouldInstrumentReadWriteFromAddress(I->getModule(), Addr))
      continue;
    if (!IsWrite && addrPointsToConstantData(Addr))
      continue;
    All.emplace_back(I);
  }
  Local.clear();
}

bool Arbalest::instrumentLoadOrStore(Instruction *I, const DataLayout &DL) {
  IRBuilder<> IRB(I);
  const bool IsWrite = isa<StoreInst>(*I);
  Value *Addr = IsWrite ? cast<StoreInst>(I)->getPointerOperand()
                        : cast<LoadInst>(I)->getPointerOperand();
  Type *OrigTy = getLoadStoreType(I);

  if (Addr->isSwiftError())
    return false;

  int Idx = getMemoryAccessFuncIndex(OrigTy, Addr, DL);
  if (Idx < 0)
    return false;
  if (isVtableAccess(I))
    return true;

  const Align Alignment = IsWrite ? cast<StoreInst>(I)->getAlign()
                                  : cast<LoadInst>(I)->getAlign();
  const uint32_t TypeSize = DL.getTypeStoreSizeInBits(OrigTy);
  FunctionCallee OnAccessFunc;
  if (Alignment >= Align(8) || (Alignment.value() % (TypeSize / 8)) == 0)
    OnAccessFunc = IsWrite ? ArbalestWrite[Idx] : ArbalestRead[Idx];
  else
    OnAccessFunc =
        IsWrite ? ArbalestUnalignedWrite[Idx] : ArbalestUnalignedRead[Idx];

  IRB.CreateCall(OnAccessFunc, IRB.CreatePointerCast(Addr, IRB.getInt8PtrTy()));
  return true;
}

bool Arbalest::instrumentGEP(GetElementPtrInst *GEP, const DataLayout &DL) {
  Value *BasePtr = GEP->getOperand(0);
  for (auto UIt = GEP->use_begin(), UEnd = GEP->use_end(); UIt != UEnd; ++UIt) {
    User *U = UIt->getUser();
    if (auto *LI = dyn_cast<LoadInst>(U)) {
      IRBuilder<> IRB(LI);
      Type *OrigTy = getLoadStoreType(LI);
      int Size = getMemoryAccessSize(OrigTy, DL);
      assert(Size > 0);
      IRB.CreateCall(
          ArbalestCheckBound,
          {IRB.CreatePointerCast(BasePtr, IRB.getInt8PtrTy()),
           IRB.CreatePointerCast(GEP, IRB.getInt8PtrTy()),
           IRB.getInt32(Size)});
    }
  }
  return true;
}
