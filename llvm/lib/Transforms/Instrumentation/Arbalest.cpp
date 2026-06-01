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
#include "llvm/Transforms/Instrumentation/OMPSan/OmpSanitizer.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
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
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"

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

// Per-address access dedup: under OpenMP's serial-elision execution model, the
// runtime VSM only needs to observe the first read of an address and the first
// write that follows it (or the first write, if it precedes any read). Further
// accesses to the same address inside the same call-bounded region cannot
// change the detector's verdict, so we elide them. Calls and invokes reset the
// per-address state because the callee may launch a kernel (or otherwise
// mutate memory) that re-arms detection for tracked addresses.
//
// The mode controls how two accesses are recognized as targeting the "same"
// address:
//   off    - no dedup; instrument every eligible access
//   value  - dedup on raw pointer SSA value (TSan-style)
//   strip  - dedup on stripPointerCasts()'d pointer (default)
//   aa     - dedup via AliasAnalysis MustAlias queries (most precise; costs
//            O(N^2 * AA) per function in the worst case)
enum DedupeMode {
  DM_Off,
  DM_Value,
  DM_Strip,
  DM_AA,
};

static cl::opt<DedupeMode> ClDedupeMode(
    "arbalest-dedupe-mode", cl::init(DM_Strip),
    cl::desc("How aggressively Arbalest dedupes redundant memory accesses"),
    cl::values(
        clEnumValN(DM_Off, "off",
                   "No dedup; instrument every eligible access"),
        clEnumValN(DM_Value, "value",
                   "Dedup on raw pointer SSA value (TSan-style)"),
        clEnumValN(DM_Strip, "strip",
                   "Dedup on stripPointerCasts'd pointer (default)"),
        clEnumValN(DM_AA, "aa",
                   "Dedup via MustAlias queries (precise, more expensive)")),
    cl::Hidden);

static cl::opt<bool> ClArbalestHoist(
    "arbalest-hoist", cl::init(true),
    cl::desc("Hoist loop-invariant Arbalest range checks out of loops using "
             "ScalarEvolution"),
    cl::Hidden);

// When non-empty, only functions whose names appear in this list are
// instrumented. This is an explicit include/allowlist: every function not
// named here is skipped entirely. Complements the per-attribute
// DisableSanitizerInstrumentation exclude mechanism.
static cl::list<std::string> ClArbalestIncludeFunctions(
    "arbalest-only-functions",
    cl::desc("Comma-separated list of function names Arbalest should "
             "instrument (empty = instrument all eligible functions)"),
    cl::Hidden, cl::CommaSeparated);

// When true, run the OMPSan data-mapping analysis before instrumentation and
// use its reported suspect functions as the include list.  OMPSan will flag
// both true and false positives; Arbalest then instruments only those
// functions so that the runtime can produce a precise verdict.
static cl::opt<bool> ClArbalestOMPSan(
    "arbalest-ompsan", cl::init(false),
    cl::desc("Run OMPSan analysis and restrict Arbalest instrumentation to "
             "the functions OMPSan identifies as data-mapping suspects"),
    cl::Hidden);

STATISTIC(NumInstrumentedArbalestAccesses,
          "Number of load/store accesses Arbalest instrumented");
STATISTIC(NumElidedArbalestAccesses,
          "Number of load/store accesses Arbalest elided by dedup");
STATISTIC(NumHoistedArbalestLoops,
          "Number of loops from which Arbalest hoisted range checks");

namespace {

constexpr char kArbalestModuleCtorName[] = "arbalest.module_ctor";
constexpr char kArbalestInitName[] = "__arbalest_init";
constexpr char kOmpOutlinedFuncPrefixFlag[] = "OmpOutlinedFuncPrefix";

constexpr size_t kNumberOfAccessSizes = 5;

class Arbalest {
public:
  explicit Arbalest(AAResults *AA = nullptr, LoopInfo *LI = nullptr,
                    ScalarEvolution *SE = nullptr)
      : AA(AA), LI(LI), SE(SE) {}
  bool sanitizeFunction(Function &F);

private:
  void initialize(Module &M);
  bool hoistLoopChecks(Function &F, DenseSet<Instruction *> &Hoisted);
  bool instrumentLoadOrStore(Instruction *I, const DataLayout &DL);
  bool instrumentGEP(GetElementPtrInst *GEP, const DataLayout &DL);

  AAResults *AA;
  LoopInfo *LI;
  ScalarEvolution *SE;
  FunctionCallee ArbalestRead[kNumberOfAccessSizes];
  FunctionCallee ArbalestWrite[kNumberOfAccessSizes];
  FunctionCallee ArbalestUnalignedRead[kNumberOfAccessSizes];
  FunctionCallee ArbalestUnalignedWrite[kNumberOfAccessSizes];
  FunctionCallee ArbalestCheckBound;
  // Loop-hoisted range/stride callbacks.
  FunctionCallee ArbalestReadRange;
  FunctionCallee ArbalestWriteRange;
  FunctionCallee ArbalestReadStride;
  FunctionCallee ArbalestWriteStride;
  FunctionCallee ArbalestReadCStride;
  FunctionCallee ArbalestWriteCStride;
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

// Populated by ModuleArbalestPass when ClArbalestOMPSan is active.
// ArbalestPass (a function pass) reads this to decide which functions to
// instrument.  Strings are owned by this set.
static StringSet<> ArbalestOMPSanIncludeFuncs;

} // namespace

PreservedAnalyses ModuleArbalestPass::run(Module &M, ModuleAnalysisManager &AM) {
  if (!ClEnableArbalest)
    return PreservedAnalyses::all();
  insertArbalestCtor(M);
  setOmpOutlinedFuncPrefix(M);

  // When OMPSan mode is active, run the OmpSanitizerGlobalAnalysis and
  // collect the set of functions it flags as data-mapping suspects.  These
  // become Arbalest's include list for this module; ArbalestPass (the per-
  // function pass that follows) will skip any function not in the set.
  if (ClArbalestOMPSan) {
    ArbalestOMPSanIncludeFuncs.clear();
    auto &SanInfo = AM.getResult<OmpSanitizerGlobalAnalysis>(M);
    for (auto &Entry : SanInfo.getIncludeList())
      ArbalestOMPSanIncludeFuncs.insert(Entry.first);
  }

  return PreservedAnalyses::none();
}

PreservedAnalyses ArbalestPass::run(Function &F, FunctionAnalysisManager &FAM) {
  if (!ClEnableArbalest)
    return PreservedAnalyses::all();
  // AA is only needed for the `aa` dedup mode. Request it lazily so the other
  // modes don't pay the AA pipeline cost.
  AAResults *AA =
      (ClDedupeMode == DM_AA) ? &FAM.getResult<AAManager>(F) : nullptr;
  // LoopInfo and ScalarEvolution are needed for loop-hoisting. Always request
  // them when hoisting is enabled; both are cheap if the function has no loops.
  LoopInfo *LI = nullptr;
  ScalarEvolution *SE = nullptr;
  if (ClArbalestHoist) {
    LI = &FAM.getResult<LoopAnalysis>(F);
    SE = &FAM.getResult<ScalarEvolutionAnalysis>(F);
  }
  Arbalest A(AA, LI, SE);
  if (A.sanitizeFunction(F))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// Per-address state for the dedup pass. See ClDedupeMode for the model.
namespace {
enum AccessState : uint8_t {
  AS_NoAccess = 0,
  AS_SeenRead,     // we have already emitted a read for this address
  AS_SeenWrite,    // we have already emitted a write; no further emits needed
};

// Apply the (NoAccess|SeenRead|SeenWrite) state machine to a single access.
// Returns true if the access should be instrumented; updates `S` in place.
bool stepAccessState(AccessState &S, bool IsWrite) {
  switch (S) {
  case AS_NoAccess:
    S = IsWrite ? AS_SeenWrite : AS_SeenRead;
    return true;
  case AS_SeenRead:
    if (IsWrite) {
      S = AS_SeenWrite;
      return true;
    }
    return false;
  case AS_SeenWrite:
    return false;
  }
  llvm_unreachable("invalid AccessState");
}
} // namespace

// Linear-scan dedup keyed on AA::alias(... MustAlias). Used only when the
// dedup mode is `aa`. Returns a pointer into State or nullptr if no existing
// entry MustAlias's `Loc`.
static AccessState *findMustAliasState(
    SmallVectorImpl<std::pair<MemoryLocation, AccessState>> &State,
    const MemoryLocation &Loc, AAResults &AA) {
  for (auto &Entry : State) {
    if (AA.alias(Loc, Entry.first) == AliasResult::MustAlias)
      return &Entry.second;
  }
  return nullptr;
}

static void collectAccessesWithDedup(Function &F, AAResults *AA,
                                     const DenseSet<Instruction *> &Hoisted,
                                     SmallVectorImpl<Instruction *> &Out) {
  // For value/strip modes we can use a hash map keyed on a Value*.
  DenseMap<Value *, AccessState> AddrState;
  // For aa mode we store MemoryLocation/state pairs and do a linear MustAlias
  // scan, since MemoryLocations aren't trivially hashable for "same address"
  // semantics.
  SmallVector<std::pair<MemoryLocation, AccessState>, 8> AAState;

  const Module *M = F.getParent();
  for (auto &BB : F) {
    for (auto &Inst : BB) {
      // A call/invoke may launch a kernel or otherwise mutate memory; treat it
      // as a region boundary by resetting per-address state.
      if ((isa<CallInst>(Inst) && !isa<DbgInfoIntrinsic>(Inst)) ||
          isa<InvokeInst>(Inst)) {
        AddrState.clear();
        AAState.clear();
        continue;
      }
      if (!isa<LoadInst>(Inst) && !isa<StoreInst>(Inst))
        continue;
      if (Hoisted.count(&Inst))
        continue;

      const bool IsWrite = isa<StoreInst>(Inst);
      Value *Addr = IsWrite ? cast<StoreInst>(&Inst)->getPointerOperand()
                            : cast<LoadInst>(&Inst)->getPointerOperand();

      if (!shouldInstrumentReadWriteFromAddress(M, Addr))
        continue;
      if (!IsWrite && addrPointsToConstantData(Addr))
        continue;

      bool Emit;
      switch (ClDedupeMode) {
      case DM_Off:
        Emit = true;
        break;
      case DM_Value:
      case DM_Strip: {
        Value *Key = (ClDedupeMode == DM_Strip) ? Addr->stripPointerCasts()
                                                : Addr;
        Emit = stepAccessState(AddrState[Key], IsWrite);
        break;
      }
      case DM_AA: {
        assert(AA && "aa dedup mode requires AAResults");
        MemoryLocation Loc = MemoryLocation::get(&Inst);
        if (AccessState *S = findMustAliasState(AAState, Loc, *AA)) {
          Emit = stepAccessState(*S, IsWrite);
        } else {
          AccessState NewS = AS_NoAccess;
          Emit = stepAccessState(NewS, IsWrite);
          AAState.emplace_back(Loc, NewS);
        }
        break;
      }
      }

      if (Emit)
        Out.push_back(&Inst);
      else
        ++NumElidedArbalestAccesses;
    }
  }
}

bool Arbalest::sanitizeFunction(Function &F) {
  if (F.getName() == kArbalestModuleCtorName)
    return false;
  if (F.hasFnAttribute(Attribute::Naked))
    return false;
  if (F.hasFnAttribute(Attribute::DisableSanitizerInstrumentation))
    return false;

  // Include-list filtering: if at least one include list is active, only
  // instrument functions that appear in at least one of them (union).
  // -arbalest-only-functions provides a user-specified explicit list;
  // -arbalest-ompsan provides a list derived from the OMPSan analysis.
  if (!ClArbalestIncludeFunctions.empty() || ClArbalestOMPSan) {
    bool Allowed = false;
    // Check OMPSan-derived list first (populated by ModuleArbalestPass).
    if (ClArbalestOMPSan && ArbalestOMPSanIncludeFuncs.count(F.getName()))
      Allowed = true;
    // Check user-supplied include list.
    if (!Allowed) {
      for (const std::string &Name : ClArbalestIncludeFunctions) {
        if (F.getName() == Name) {
          Allowed = true;
          break;
        }
      }
    }
    if (!Allowed)
      return false;
  }

  initialize(*F.getParent());

  const DataLayout &DL = F.getParent()->getDataLayout();
  SmallVector<Instruction *, 16> AllLoadsAndStores;

  // Phase 1: hoist range/stride checks out of loops where SCEV can prove the
  // access pattern. Instructions placed in Hoisted are skipped by dedup.
  DenseSet<Instruction *> Hoisted;
  bool Res = hoistLoopChecks(F, Hoisted);

  // Phase 2: per-element instrumentation for accesses not covered by hoisting.
  // All four dedup modes are handled inside collectAccessesWithDedup. DM_Off
  // simply emits every eligible access.
  collectAccessesWithDedup(F, AA, Hoisted, AllLoadsAndStores);

  for (auto *Inst : AllLoadsAndStores) {
    if (instrumentLoadOrStore(Inst, DL)) {
      Res = true;
      ++NumInstrumentedArbalestAccesses;
    }
  }

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

bool Arbalest::hoistLoopChecks(Function &F, DenseSet<Instruction *> &Hoisted) {
  if (!LI || !SE || !ClArbalestHoist)
    return false;

  const DataLayout &DL = F.getParent()->getDataLayout();
  const Module *M = F.getParent();
  LLVMContext &Ctx = F.getContext();
  Type *I8PtrTy = Type::getInt8PtrTy(Ctx);
  Type *I64Ty = Type::getInt64Ty(Ctx);

  bool Changed = false;

  // Process innermost loops first so their hoists take precedence.
  SmallVector<Loop *, 8> Loops = LI->getLoopsInPreorder();
  for (Loop *L : reverse(Loops)) {
    const SCEV *BTC = SE->getBackedgeTakenCount(L);
    if (isa<SCEVCouldNotCompute>(BTC))
      continue;

    BasicBlock *Preheader = L->getLoopPreheader();
    if (!Preheader)
      continue;

    // One SCEVExpander per loop; it caches and reuses expanded values.
    Instruction *InsertPt = Preheader->getTerminator();
    SCEVExpander Exp(*SE, DL, "arbalest.hoist");

    // Deduplicate: only emit one hoisted call per unique (start, step, rw).
    using HoistKey = std::pair<const SCEV *, const SCEV *>;
    DenseSet<HoistKey> SeenReads, SeenWrites;

    bool LoopChanged = false;
    for (BasicBlock *BB : L->blocks()) {
      for (Instruction &Inst : *BB) {
        if (!isa<LoadInst>(Inst) && !isa<StoreInst>(Inst))
          continue;
        if (Hoisted.count(&Inst))
          continue;

        const bool IsWrite = isa<StoreInst>(Inst);
        Value *Addr = IsWrite ? cast<StoreInst>(&Inst)->getPointerOperand()
                              : cast<LoadInst>(&Inst)->getPointerOperand();

        if (!shouldInstrumentReadWriteFromAddress(M, Addr))
          continue;
        if (!IsWrite && addrPointsToConstantData(Addr))
          continue;

        const SCEV *PtrSCEV = SE->getSCEV(Addr);
        const auto *AR = dyn_cast<SCEVAddRecExpr>(PtrSCEV);
        // Only hoist affine recurrences ({start,+,step}) in this exact loop.
        if (!AR || AR->getLoop() != L || !AR->isAffine())
          continue;

        const SCEV *StartSCEV = AR->getStart();
        const SCEV *StepSCEV = AR->getStepRecurrence(*SE);

        // Skip non-positive (reverse / zero) strides.
        if (const auto *CS = dyn_cast<SCEVConstant>(StepSCEV))
          if (CS->getValue()->getValue().isNonPositive())
            continue;

        HoistKey Key{StartSCEV, StepSCEV};
        auto &Seen = IsWrite ? SeenWrites : SeenReads;
        if (!Seen.insert(Key).second) {
          // Already emitted a hoisted call covering this range; just suppress
          // the per-element callback.
          Hoisted.insert(&Inst);
          continue;
        }

        // Element size from the original access instruction.
        Type *ElemTy = getLoadStoreType(&Inst);
        if (!ElemTy->isSized())
          continue;
        uint64_t ElemBytes = DL.getTypeStoreSize(ElemTy).getFixedSize();

        // --- Expand SCEV values into preheader IR ---
        // start pointer
        Value *StartV = Exp.expandCodeFor(StartSCEV, AR->getType(), InsertPt);

        // step as i64 bytes
        bool StepIsConst = isa<SCEVConstant>(StepSCEV);
        int64_t ConstStepBytes = StepIsConst
            ? cast<SCEVConstant>(StepSCEV)->getValue()->getSExtValue()
            : 0;
        Value *StepV = StepIsConst
            ? ConstantInt::get(I64Ty, ConstStepBytes)
            : Exp.expandCodeFor(StepSCEV, I64Ty, InsertPt);

        // trip count = BTC + 1
        const SCEV *TripCountSCEV =
            SE->getAddExpr(BTC, SE->getConstant(BTC->getType(), 1));
        Value *TripCountV = Exp.expandCodeFor(TripCountSCEV, I64Ty, InsertPt);

        // Derive end pointer and emit the call using IRBuilder.
        IRBuilder<> IRB(InsertPt);
        Value *StartI8 = IRB.CreatePointerCast(StartV, I8PtrTy, "arbalest.start");
        Value *ByteRange = IRB.CreateMul(StepV, TripCountV, "arbalest.range");
        Value *EndI8 = IRB.CreateGEP(IRB.getInt8Ty(), StartI8, ByteRange,
                                     "arbalest.end");
        Value *ElemBytesV = ConstantInt::get(I64Ty, ElemBytes);

        if (StepIsConst && ConstStepBytes == (int64_t)ElemBytes) {
          // Unit stride: all accessed bytes are contiguous.
          FunctionCallee Fn = IsWrite ? ArbalestWriteRange : ArbalestReadRange;
          IRB.CreateCall(Fn, {StartI8, EndI8});
        } else if (StepIsConst) {
          FunctionCallee Fn =
              IsWrite ? ArbalestWriteCStride : ArbalestReadCStride;
          IRB.CreateCall(Fn, {StartI8, EndI8, StepV, ElemBytesV});
        } else {
          FunctionCallee Fn =
              IsWrite ? ArbalestWriteStride : ArbalestReadStride;
          IRB.CreateCall(Fn, {StartI8, EndI8, StepV, ElemBytesV});
        }

        Hoisted.insert(&Inst);
        LoopChanged = true;
      }
    }

    if (LoopChanged) {
      ++NumHoistedArbalestLoops;
      Changed = true;
    }
  }
  return Changed;
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

  // Loop-hoisted range/stride callbacks.
  Type *I8PtrTy = IRB.getInt8PtrTy();
  Type *I64Ty = IRB.getInt64Ty();
  ArbalestReadRange = M.getOrInsertFunction("__arbalest_read_range", Attr,
      IRB.getVoidTy(), I8PtrTy, I8PtrTy);
  ArbalestWriteRange = M.getOrInsertFunction("__arbalest_write_range", Attr,
      IRB.getVoidTy(), I8PtrTy, I8PtrTy);
  ArbalestReadStride = M.getOrInsertFunction("__arbalest_read_stride", Attr,
      IRB.getVoidTy(), I8PtrTy, I8PtrTy, I64Ty, I64Ty);
  ArbalestWriteStride = M.getOrInsertFunction("__arbalest_write_stride", Attr,
      IRB.getVoidTy(), I8PtrTy, I8PtrTy, I64Ty, I64Ty);
  ArbalestReadCStride = M.getOrInsertFunction("__arbalest_read_cstride", Attr,
      IRB.getVoidTy(), I8PtrTy, I8PtrTy, I64Ty, I64Ty);
  ArbalestWriteCStride = M.getOrInsertFunction("__arbalest_write_cstride", Attr,
      IRB.getVoidTy(), I8PtrTy, I8PtrTy, I64Ty, I64Ty);
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
