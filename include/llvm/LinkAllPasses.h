//===- llvm/LinkAllPasses.h ------------ Reference All Passes ---*- C++ -*-===//
//
//                      The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This header file pulls in all transformation and analysis passes for tools 
// like opt and bugpoint that need this functionality.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LINKALLPASSES_H
#define LLVM_LINKALLPASSES_H

#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/Analysis/DomPrinter.h"
#include "llvm/Analysis/FindUsedTypes.h"
#include "llvm/Analysis/IntervalPartition.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/PointerTracking.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/Lint.h"
#include "llvm/Assembly/PrintModulePass.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Function.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"
#include <cstdlib>

namespace {
  struct ForcePassLinking {
    ForcePassLinking() {
      // We must reference the passes in such a way that compilers will not
      // delete it all as dead code, even with whole program optimization,
      // yet is effectively a NO-OP. As the compiler isn't smart enough
      // to know that getenv() never returns -1, this will do the job.
      if (std::getenv("bar") != (char*) -1)
        return;

      (void) llvm::createAAEvalPass();
      (void) llvm::createAggressiveDCEPass();
      (void) llvm::createAliasAnalysisCounterPass();
      (void) llvm::createAliasDebugger();
      (void) llvm::createArgumentPromotionPass();
      (void) llvm::createStructRetPromotionPass();
      (void) llvm::createBasicAliasAnalysisPass();
      (void) llvm::createLibCallAliasAnalysisPass(0);
      (void) llvm::createScalarEvolutionAliasAnalysisPass();
      (void) llvm::createBlockPlacementPass();
      (void) llvm::createBreakCriticalEdgesPass();
      (void) llvm::createCFGSimplificationPass();
      (void) llvm::createConstantMergePass();
      (void) llvm::createConstantPropagationPass();
      (void) llvm::createDeadArgEliminationPass();
      (void) llvm::createDeadCodeEliminationPass();
      (void) llvm::createDeadInstEliminationPass();
      (void) llvm::createDeadStoreEliminationPass();
      (void) llvm::createDeadTypeEliminationPass();
      (void) llvm::createDomOnlyPrinterPass();
      (void) llvm::createDomPrinterPass();
      (void) llvm::createDomOnlyViewerPass();
      (void) llvm::createDomViewerPass();
      (void) llvm::createEdgeProfilerPass();
      (void) llvm::createOptimalEdgeProfilerPass();
      (void) llvm::createFunctionInliningPass();
      (void) llvm::createAlwaysInlinerPass();
      (void) llvm::createGlobalDCEPass();
      (void) llvm::createGlobalOptimizerPass();
      (void) llvm::createGlobalsModRefPass();
      (void) llvm::createIPConstantPropagationPass();
      (void) llvm::createIPSCCPPass();
      (void) llvm::createIndVarSimplifyPass();
      (void) llvm::createInstructionCombiningPass();
      (void) llvm::createInternalizePass(false);
      (void) llvm::createLCSSAPass();
      (void) llvm::createLICMPass();
      (void) llvm::createLazyValueInfoPass();
      (void) llvm::createLiveValuesPass();
      (void) llvm::createLoopDependenceAnalysisPass();
      (void) llvm::createLoopExtractorPass();
      (void) llvm::createLoopSimplifyPass();
      (void) llvm::createLoopStrengthReducePass();
      (void) llvm::createLoopUnrollPass();
      (void) llvm::createLoopUnswitchPass();
      (void) llvm::createLoopRotatePass();
      (void) llvm::createLoopIndexSplitPass();
      (void) llvm::createLowerInvokePass();
      (void) llvm::createLowerSetJmpPass();
      (void) llvm::createLowerSwitchPass();
      (void) llvm::createNoAAPass();
      (void) llvm::createNoProfileInfoPass();
      (void) llvm::createProfileEstimatorPass();
      (void) llvm::createProfileVerifierPass();
      (void) llvm::createProfileLoaderPass();
      (void) llvm::createPromoteMemoryToRegisterPass();
      (void) llvm::createDemoteRegisterToMemoryPass();
      (void) llvm::createPruneEHPass();
      (void) llvm::createPostDomOnlyPrinterPass();
      (void) llvm::createPostDomPrinterPass();
      (void) llvm::createPostDomOnlyViewerPass();
      (void) llvm::createPostDomViewerPass();
      (void) llvm::createReassociatePass();
      (void) llvm::createSCCPPass();
      (void) llvm::createScalarReplAggregatesPass();
      (void) llvm::createSimplifyLibCallsPass();
      (void) llvm::createSimplifyHalfPowrLibCallsPass();
      (void) llvm::createSingleLoopExtractorPass();
      (void) llvm::createStripSymbolsPass();
      (void) llvm::createStripNonDebugSymbolsPass();
      (void) llvm::createStripDeadPrototypesPass();
      (void) llvm::createTailCallEliminationPass();
      (void) llvm::createTailDuplicationPass();
      (void) llvm::createJumpThreadingPass();
      (void) llvm::createUnifyFunctionExitNodesPass();
      (void) llvm::createInstCountPass();
      (void) llvm::createCodeGenPreparePass();
      (void) llvm::createGVNPass();
      (void) llvm::createMemCpyOptPass();
      (void) llvm::createLoopDeletionPass();
      (void) llvm::createPostDomTree();
      (void) llvm::createPostDomFrontier();
      (void) llvm::createInstructionNamerPass();
      (void) llvm::createPartialSpecializationPass();
      (void) llvm::createFunctionAttrsPass();
      (void) llvm::createMergeFunctionsPass();
      (void) llvm::createPrintModulePass(0);
      (void) llvm::createPrintFunctionPass("", 0);
      (void) llvm::createDbgInfoPrinterPass();
      (void) llvm::createPartialInliningPass();
      (void) llvm::createSSIPass();
      (void) llvm::createSSIEverythingPass();
      (void) llvm::createGEPSplitterPass();
      (void) llvm::createSCCVNPass();
      (void) llvm::createABCDPass();
      (void) llvm::createLintPass();

      (void)new llvm::IntervalPartition();
      (void)new llvm::FindUsedTypes();
      (void)new llvm::ScalarEvolution();
      (void)new llvm::PointerTracking();
      ((llvm::Function*)0)->viewCFGOnly();
      llvm::AliasSetTracker X(*(llvm::AliasAnalysis*)0);
      X.add((llvm::Value*)0, 0);  // for -print-alias-sets
    }
  } ForcePassLinking; // Force link by creating a global definition.
}

#endif
