add_llvm_component_library(LLVMInstrumentation
  AddressSanitizer.cpp
  Arbalest.cpp
  BoundsChecking.cpp
  CGProfile.cpp
  ControlHeightReduction.cpp
  DataFlowSanitizer.cpp
  GCOVProfiling.cpp
  MemProfiler.cpp
  MemorySanitizer.cpp
  IndirectCallPromotion.cpp
  Instrumentation.cpp
  InstrOrderFile.cpp
  InstrProfiling.cpp
  PGOInstrumentation.cpp
  PGOMemOPSizeOpt.cpp
  PoisonChecking.cpp
  SanitizerCoverage.cpp
  ValueProfileCollector.cpp
  ThreadSanitizer.cpp
  HWAddressSanitizer.cpp

  # OMPSan & OMPMemOpt static analysis passes (used by Arbalest --arbalest-ompsan)
  OMPSan/MemUseDefAnalysis.cpp
  OMPSan/OmpDiagnosticsAnalysis.cpp
  OMPSan/OmpSanitizer.cpp
  OMPSan/OmpOptiMem.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  Demangle
  MC
  Support
  TransformUtils
  ProfileData
  )
