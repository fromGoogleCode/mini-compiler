//===-- SelectionDAGISel.cpp - Implement the SelectionDAGISel class -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This implements the SelectionDAGISel class.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "isel"
#include "ScheduleDAGSDNodes.h"
#include "SelectionDAGBuilder.h"
#include "FunctionLoweringInfo.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/DebugInfo.h"
#include "llvm/Constants.h"
#include "llvm/CallingConv.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/GlobalVariable.h"
#include "llvm/InlineAsm.h"
#include "llvm/Instructions.h"
#include "llvm/Intrinsics.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/LLVMContext.h"
#include "llvm/CodeGen/FastISel.h"
#include "llvm/CodeGen/GCStrategy.h"
#include "llvm/CodeGen/GCMetadata.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionAnalysis.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/ScheduleHazardRecognizer.h"
#include "llvm/CodeGen/SchedulerRegistry.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetFrameInfo.h"
#include "llvm/Target/TargetIntrinsicInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/Statistic.h"
#include <algorithm>
using namespace llvm;

STATISTIC(NumFastIselFailures, "Number of instructions fast isel failed on");
STATISTIC(NumDAGIselRetries,"Number of times dag isel has to try another path");

static cl::opt<bool>
EnableFastISelVerbose("fast-isel-verbose", cl::Hidden,
          cl::desc("Enable verbose messages in the \"fast\" "
                   "instruction selector"));
static cl::opt<bool>
EnableFastISelAbort("fast-isel-abort", cl::Hidden,
          cl::desc("Enable abort calls when \"fast\" instruction fails"));
static cl::opt<bool>
SchedLiveInCopies("schedule-livein-copies", cl::Hidden,
                  cl::desc("Schedule copies of livein registers"),
                  cl::init(false));

#ifndef NDEBUG
static cl::opt<bool>
ViewDAGCombine1("view-dag-combine1-dags", cl::Hidden,
          cl::desc("Pop up a window to show dags before the first "
                   "dag combine pass"));
static cl::opt<bool>
ViewLegalizeTypesDAGs("view-legalize-types-dags", cl::Hidden,
          cl::desc("Pop up a window to show dags before legalize types"));
static cl::opt<bool>
ViewLegalizeDAGs("view-legalize-dags", cl::Hidden,
          cl::desc("Pop up a window to show dags before legalize"));
static cl::opt<bool>
ViewDAGCombine2("view-dag-combine2-dags", cl::Hidden,
          cl::desc("Pop up a window to show dags before the second "
                   "dag combine pass"));
static cl::opt<bool>
ViewDAGCombineLT("view-dag-combine-lt-dags", cl::Hidden,
          cl::desc("Pop up a window to show dags before the post legalize types"
                   " dag combine pass"));
static cl::opt<bool>
ViewISelDAGs("view-isel-dags", cl::Hidden,
          cl::desc("Pop up a window to show isel dags as they are selected"));
static cl::opt<bool>
ViewSchedDAGs("view-sched-dags", cl::Hidden,
          cl::desc("Pop up a window to show sched dags as they are processed"));
static cl::opt<bool>
ViewSUnitDAGs("view-sunit-dags", cl::Hidden,
      cl::desc("Pop up a window to show SUnit dags after they are processed"));
#else
static const bool ViewDAGCombine1 = false,
                  ViewLegalizeTypesDAGs = false, ViewLegalizeDAGs = false,
                  ViewDAGCombine2 = false,
                  ViewDAGCombineLT = false,
                  ViewISelDAGs = false, ViewSchedDAGs = false,
                  ViewSUnitDAGs = false;
#endif

//===---------------------------------------------------------------------===//
///
/// RegisterScheduler class - Track the registration of instruction schedulers.
///
//===---------------------------------------------------------------------===//
MachinePassRegistry RegisterScheduler::Registry;

//===---------------------------------------------------------------------===//
///
/// ISHeuristic command line option for instruction schedulers.
///
//===---------------------------------------------------------------------===//
static cl::opt<RegisterScheduler::FunctionPassCtor, false,
               RegisterPassParser<RegisterScheduler> >
ISHeuristic("pre-RA-sched",
            cl::init(&createDefaultScheduler),
            cl::desc("Instruction schedulers available (before register"
                     " allocation):"));

static RegisterScheduler
defaultListDAGScheduler("default", "Best scheduler for the target",
                        createDefaultScheduler);

namespace llvm {
  //===--------------------------------------------------------------------===//
  /// createDefaultScheduler - This creates an instruction scheduler appropriate
  /// for the target.
  ScheduleDAGSDNodes* createDefaultScheduler(SelectionDAGISel *IS,
                                             CodeGenOpt::Level OptLevel) {
    const TargetLowering &TLI = IS->getTargetLowering();

    if (OptLevel == CodeGenOpt::None)
      return createFastDAGScheduler(IS, OptLevel);
    if (TLI.getSchedulingPreference() == TargetLowering::SchedulingForLatency)
      return createTDListDAGScheduler(IS, OptLevel);
    assert(TLI.getSchedulingPreference() ==
           TargetLowering::SchedulingForRegPressure && "Unknown sched type!");
    return createBURRListDAGScheduler(IS, OptLevel);
  }
}

// EmitInstrWithCustomInserter - This method should be implemented by targets
// that mark instructions with the 'usesCustomInserter' flag.  These
// instructions are special in various ways, which require special support to
// insert.  The specified MachineInstr is created but not inserted into any
// basic blocks, and this method is called to expand it into a sequence of
// instructions, potentially also creating new basic blocks and control flow.
// When new basic blocks are inserted and the edges from MBB to its successors
// are modified, the method should insert pairs of <OldSucc, NewSucc> into the
// DenseMap.
MachineBasicBlock *TargetLowering::EmitInstrWithCustomInserter(MachineInstr *MI,
                                                         MachineBasicBlock *MBB,
                   DenseMap<MachineBasicBlock*, MachineBasicBlock*> *EM) const {
#ifndef NDEBUG
  dbgs() << "If a target marks an instruction with "
          "'usesCustomInserter', it must implement "
          "TargetLowering::EmitInstrWithCustomInserter!";
#endif
  llvm_unreachable(0);
  return 0;
}

/// EmitLiveInCopy - Emit a copy for a live in physical register. If the
/// physical register has only a single copy use, then coalesced the copy
/// if possible.
static void EmitLiveInCopy(MachineBasicBlock *MBB,
                           MachineBasicBlock::iterator &InsertPos,
                           unsigned VirtReg, unsigned PhysReg,
                           const TargetRegisterClass *RC,
                           DenseMap<MachineInstr*, unsigned> &CopyRegMap,
                           const MachineRegisterInfo &MRI,
                           const TargetRegisterInfo &TRI,
                           const TargetInstrInfo &TII) {
  unsigned NumUses = 0;
  MachineInstr *UseMI = NULL;
  for (MachineRegisterInfo::use_iterator UI = MRI.use_begin(VirtReg),
         UE = MRI.use_end(); UI != UE; ++UI) {
    UseMI = &*UI;
    if (++NumUses > 1)
      break;
  }

  // If the number of uses is not one, or the use is not a move instruction,
  // don't coalesce. Also, only coalesce away a virtual register to virtual
  // register copy.
  bool Coalesced = false;
  unsigned SrcReg, DstReg, SrcSubReg, DstSubReg;
  if (NumUses == 1 &&
      TII.isMoveInstr(*UseMI, SrcReg, DstReg, SrcSubReg, DstSubReg) &&
      TargetRegisterInfo::isVirtualRegister(DstReg)) {
    VirtReg = DstReg;
    Coalesced = true;
  }

  // Now find an ideal location to insert the copy.
  MachineBasicBlock::iterator Pos = InsertPos;
  while (Pos != MBB->begin()) {
    MachineInstr *PrevMI = prior(Pos);
    DenseMap<MachineInstr*, unsigned>::iterator RI = CopyRegMap.find(PrevMI);
    // copyRegToReg might emit multiple instructions to do a copy.
    unsigned CopyDstReg = (RI == CopyRegMap.end()) ? 0 : RI->second;
    if (CopyDstReg && !TRI.regsOverlap(CopyDstReg, PhysReg))
      // This is what the BB looks like right now:
      // r1024 = mov r0
      // ...
      // r1    = mov r1024
      //
      // We want to insert "r1025 = mov r1". Inserting this copy below the
      // move to r1024 makes it impossible for that move to be coalesced.
      //
      // r1025 = mov r1
      // r1024 = mov r0
      // ...
      // r1    = mov 1024
      // r2    = mov 1025
      break; // Woot! Found a good location.
    --Pos;
  }

  bool Emitted = TII.copyRegToReg(*MBB, Pos, VirtReg, PhysReg, RC, RC);
  assert(Emitted && "Unable to issue a live-in copy instruction!\n");
  (void) Emitted;

  CopyRegMap.insert(std::make_pair(prior(Pos), VirtReg));
  if (Coalesced) {
    if (&*InsertPos == UseMI) ++InsertPos;
    MBB->erase(UseMI);
  }
}

/// EmitLiveInCopies - If this is the first basic block in the function,
/// and if it has live ins that need to be copied into vregs, emit the
/// copies into the block.
static void EmitLiveInCopies(MachineBasicBlock *EntryMBB,
                             const MachineRegisterInfo &MRI,
                             const TargetRegisterInfo &TRI,
                             const TargetInstrInfo &TII) {
  if (SchedLiveInCopies) {
    // Emit the copies at a heuristically-determined location in the block.
    DenseMap<MachineInstr*, unsigned> CopyRegMap;
    MachineBasicBlock::iterator InsertPos = EntryMBB->begin();
    for (MachineRegisterInfo::livein_iterator LI = MRI.livein_begin(),
           E = MRI.livein_end(); LI != E; ++LI)
      if (LI->second) {
        const TargetRegisterClass *RC = MRI.getRegClass(LI->second);
        EmitLiveInCopy(EntryMBB, InsertPos, LI->second, LI->first,
                       RC, CopyRegMap, MRI, TRI, TII);
      }
  } else {
    // Emit the copies into the top of the block.
    for (MachineRegisterInfo::livein_iterator LI = MRI.livein_begin(),
           E = MRI.livein_end(); LI != E; ++LI)
      if (LI->second) {
        const TargetRegisterClass *RC = MRI.getRegClass(LI->second);
        bool Emitted = TII.copyRegToReg(*EntryMBB, EntryMBB->begin(),
                                        LI->second, LI->first, RC, RC);
        assert(Emitted && "Unable to issue a live-in copy instruction!\n");
        (void) Emitted;
      }
  }
}

//===----------------------------------------------------------------------===//
// SelectionDAGISel code
//===----------------------------------------------------------------------===//

SelectionDAGISel::SelectionDAGISel(TargetMachine &tm, CodeGenOpt::Level OL) :
  MachineFunctionPass(&ID), TM(tm), TLI(*tm.getTargetLowering()),
  FuncInfo(new FunctionLoweringInfo(TLI)),
  CurDAG(new SelectionDAG(TLI, *FuncInfo)),
  SDB(new SelectionDAGBuilder(*CurDAG, TLI, *FuncInfo, OL)),
  GFI(),
  OptLevel(OL),
  DAGSize(0)
{}

SelectionDAGISel::~SelectionDAGISel() {
  delete SDB;
  delete CurDAG;
  delete FuncInfo;
}

unsigned SelectionDAGISel::MakeReg(EVT VT) {
  return RegInfo->createVirtualRegister(TLI.getRegClassFor(VT));
}

void SelectionDAGISel::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<AliasAnalysis>();
  AU.addPreserved<AliasAnalysis>();
  AU.addRequired<GCModuleInfo>();
  AU.addPreserved<GCModuleInfo>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool SelectionDAGISel::runOnMachineFunction(MachineFunction &mf) {
  Function &Fn = *mf.getFunction();

  // Do some sanity-checking on the command-line options.
  assert((!EnableFastISelVerbose || EnableFastISel) &&
         "-fast-isel-verbose requires -fast-isel");
  assert((!EnableFastISelAbort || EnableFastISel) &&
         "-fast-isel-abort requires -fast-isel");

  // Get alias analysis for load/store combining.
  AA = &getAnalysis<AliasAnalysis>();

  MF = &mf;
  const TargetInstrInfo &TII = *TM.getInstrInfo();
  const TargetRegisterInfo &TRI = *TM.getRegisterInfo();

  if (Fn.hasGC())
    GFI = &getAnalysis<GCModuleInfo>().getFunctionInfo(Fn);
  else
    GFI = 0;
  RegInfo = &MF->getRegInfo();
  DEBUG(dbgs() << "\n\n\n=== " << Fn.getName() << "\n");

  CurDAG->init(*MF);
  FuncInfo->set(Fn, *MF, EnableFastISel);
  SDB->init(GFI, *AA);

  for (Function::iterator I = Fn.begin(), E = Fn.end(); I != E; ++I)
    if (InvokeInst *Invoke = dyn_cast<InvokeInst>(I->getTerminator()))
      // Mark landing pad.
      FuncInfo->MBBMap[Invoke->getSuccessor(1)]->setIsLandingPad();

  SelectAllBasicBlocks(Fn, *MF, TII);

  // If the first basic block in the function has live ins that need to be
  // copied into vregs, emit the copies into the top of the block before
  // emitting the code for the block.
  EmitLiveInCopies(MF->begin(), *RegInfo, TRI, TII);

  // Add function live-ins to entry block live-in set.
  for (MachineRegisterInfo::livein_iterator I = RegInfo->livein_begin(),
         E = RegInfo->livein_end(); I != E; ++I)
    MF->begin()->addLiveIn(I->first);

#ifndef NDEBUG
  assert(FuncInfo->CatchInfoFound.size() == FuncInfo->CatchInfoLost.size() &&
         "Not all catch info was assigned to a landing pad!");
#endif

  FuncInfo->clear();

  return true;
}

/// SetDebugLoc - Update MF's and SDB's DebugLocs if debug information is
/// attached with this instruction.
static void SetDebugLoc(Instruction *I, SelectionDAGBuilder *SDB,
                        FastISel *FastIS, MachineFunction *MF) {
  DebugLoc DL = I->getDebugLoc();
  if (DL.isUnknown()) return;
  
  SDB->setCurDebugLoc(DL);

  if (FastIS)
    FastIS->setCurDebugLoc(DL);

  // If the function doesn't have a default debug location yet, set
  // it. This is kind of a hack.
  if (MF->getDefaultDebugLoc().isUnknown())
    MF->setDefaultDebugLoc(DL);
}

/// ResetDebugLoc - Set MF's and SDB's DebugLocs to Unknown.
static void ResetDebugLoc(SelectionDAGBuilder *SDB, FastISel *FastIS) {
  SDB->setCurDebugLoc(DebugLoc());
  if (FastIS)
    FastIS->setCurDebugLoc(DebugLoc());
}

void SelectionDAGISel::SelectBasicBlock(BasicBlock *LLVMBB,
                                        BasicBlock::iterator Begin,
                                        BasicBlock::iterator End,
                                        bool &HadTailCall) {
  SDB->setCurrentBasicBlock(BB);

  // Lower all of the non-terminator instructions. If a call is emitted
  // as a tail call, cease emitting nodes for this block.
  for (BasicBlock::iterator I = Begin; I != End && !SDB->HasTailCall; ++I) {
    SetDebugLoc(I, SDB, 0, MF);

    if (!isa<TerminatorInst>(I)) {
      SDB->visit(*I);

      // Set the current debug location back to "unknown" so that it doesn't
      // spuriously apply to subsequent instructions.
      ResetDebugLoc(SDB, 0);
    }
  }

  if (!SDB->HasTailCall) {
    // Ensure that all instructions which are used outside of their defining
    // blocks are available as virtual registers.  Invoke is handled elsewhere.
    for (BasicBlock::iterator I = Begin; I != End; ++I)
      if (!isa<PHINode>(I) && !isa<InvokeInst>(I))
        SDB->CopyToExportRegsIfNeeded(I);

    // Handle PHI nodes in successor blocks.
    if (End == LLVMBB->end()) {
      HandlePHINodesInSuccessorBlocks(LLVMBB);

      // Lower the terminator after the copies are emitted.
      SetDebugLoc(LLVMBB->getTerminator(), SDB, 0, MF);
      SDB->visit(*LLVMBB->getTerminator());
      ResetDebugLoc(SDB, 0);
    }
  }

  // Make sure the root of the DAG is up-to-date.
  CurDAG->setRoot(SDB->getControlRoot());

  // Final step, emit the lowered DAG as machine code.
  CodeGenAndEmitDAG();
  HadTailCall = SDB->HasTailCall;
  SDB->clear();
}

namespace {
/// WorkListRemover - This class is a DAGUpdateListener that removes any deleted
/// nodes from the worklist.
class SDOPsWorkListRemover : public SelectionDAG::DAGUpdateListener {
  SmallVector<SDNode*, 128> &Worklist;
  SmallPtrSet<SDNode*, 128> &InWorklist;
public:
  SDOPsWorkListRemover(SmallVector<SDNode*, 128> &wl,
                       SmallPtrSet<SDNode*, 128> &inwl)
    : Worklist(wl), InWorklist(inwl) {}

  void RemoveFromWorklist(SDNode *N) {
    if (!InWorklist.erase(N)) return;
    
    SmallVector<SDNode*, 128>::iterator I =
    std::find(Worklist.begin(), Worklist.end(), N);
    assert(I != Worklist.end() && "Not in worklist");
    
    *I = Worklist.back();
    Worklist.pop_back();
  }
  
  virtual void NodeDeleted(SDNode *N, SDNode *E) {
    RemoveFromWorklist(N);
  }

  virtual void NodeUpdated(SDNode *N) {
    // Ignore updates.
  }
};
}

/// TrivialTruncElim - Eliminate some trivial nops that can result from
/// ShrinkDemandedOps: (trunc (ext n)) -> n.
static bool TrivialTruncElim(SDValue Op,
                             TargetLowering::TargetLoweringOpt &TLO) {
  SDValue N0 = Op.getOperand(0);
  EVT VT = Op.getValueType();
  if ((N0.getOpcode() == ISD::ZERO_EXTEND ||
       N0.getOpcode() == ISD::SIGN_EXTEND ||
       N0.getOpcode() == ISD::ANY_EXTEND) &&
      N0.getOperand(0).getValueType() == VT) {
    return TLO.CombineTo(Op, N0.getOperand(0));
  }
  return false;
}

/// ShrinkDemandedOps - A late transformation pass that shrink expressions
/// using TargetLowering::TargetLoweringOpt::ShrinkDemandedOp. It converts
/// x+y to (VT)((SmallVT)x+(SmallVT)y) if the casts are free.
void SelectionDAGISel::ShrinkDemandedOps() {
  SmallVector<SDNode*, 128> Worklist;
  SmallPtrSet<SDNode*, 128> InWorklist;

  // Add all the dag nodes to the worklist.
  Worklist.reserve(CurDAG->allnodes_size());
  for (SelectionDAG::allnodes_iterator I = CurDAG->allnodes_begin(),
       E = CurDAG->allnodes_end(); I != E; ++I) {
    Worklist.push_back(I);
    InWorklist.insert(I);
  }

  TargetLowering::TargetLoweringOpt TLO(*CurDAG, true);
  while (!Worklist.empty()) {
    SDNode *N = Worklist.pop_back_val();
    InWorklist.erase(N);

    if (N->use_empty() && N != CurDAG->getRoot().getNode()) {
      // Deleting this node may make its operands dead, add them to the worklist
      // if they aren't already there.
      for (unsigned i = 0, e = N->getNumOperands(); i != e; ++i)
        if (InWorklist.insert(N->getOperand(i).getNode()))
          Worklist.push_back(N->getOperand(i).getNode());
      
      CurDAG->DeleteNode(N);
      continue;
    }

    // Run ShrinkDemandedOp on scalar binary operations.
    if (N->getNumValues() != 1 ||
        !N->getValueType(0).isSimple() || !N->getValueType(0).isInteger())
      continue;
    
    unsigned BitWidth = N->getValueType(0).getScalarType().getSizeInBits();
    APInt Demanded = APInt::getAllOnesValue(BitWidth);
    APInt KnownZero, KnownOne;
    if (!TLI.SimplifyDemandedBits(SDValue(N, 0), Demanded,
                                  KnownZero, KnownOne, TLO) &&
        (N->getOpcode() != ISD::TRUNCATE ||
         !TrivialTruncElim(SDValue(N, 0), TLO)))
      continue;
    
    // Revisit the node.
    assert(!InWorklist.count(N) && "Already in worklist");
    Worklist.push_back(N);
    InWorklist.insert(N);

    // Replace the old value with the new one.
    DEBUG(errs() << "\nShrinkDemandedOps replacing "; 
          TLO.Old.getNode()->dump(CurDAG);
          errs() << "\nWith: ";
          TLO.New.getNode()->dump(CurDAG);
          errs() << '\n');

    if (InWorklist.insert(TLO.New.getNode()))
      Worklist.push_back(TLO.New.getNode());

    SDOPsWorkListRemover DeadNodes(Worklist, InWorklist);
    CurDAG->ReplaceAllUsesOfValueWith(TLO.Old, TLO.New, &DeadNodes);

    if (!TLO.Old.getNode()->use_empty()) continue;
        
    for (unsigned i = 0, e = TLO.Old.getNode()->getNumOperands();
         i != e; ++i) {
      SDNode *OpNode = TLO.Old.getNode()->getOperand(i).getNode(); 
      if (OpNode->hasOneUse()) {
        // Add OpNode to the end of the list to revisit.
        DeadNodes.RemoveFromWorklist(OpNode);
        Worklist.push_back(OpNode);
        InWorklist.insert(OpNode);
      }
    }

    DeadNodes.RemoveFromWorklist(TLO.Old.getNode());
    CurDAG->DeleteNode(TLO.Old.getNode());
  }
}

void SelectionDAGISel::ComputeLiveOutVRegInfo() {
  SmallPtrSet<SDNode*, 128> VisitedNodes;
  SmallVector<SDNode*, 128> Worklist;

  Worklist.push_back(CurDAG->getRoot().getNode());

  APInt Mask;
  APInt KnownZero;
  APInt KnownOne;

  do {
    SDNode *N = Worklist.pop_back_val();

    // If we've already seen this node, ignore it.
    if (!VisitedNodes.insert(N))
      continue;

    // Otherwise, add all chain operands to the worklist.
    for (unsigned i = 0, e = N->getNumOperands(); i != e; ++i)
      if (N->getOperand(i).getValueType() == MVT::Other)
        Worklist.push_back(N->getOperand(i).getNode());

    // If this is a CopyToReg with a vreg dest, process it.
    if (N->getOpcode() != ISD::CopyToReg)
      continue;

    unsigned DestReg = cast<RegisterSDNode>(N->getOperand(1))->getReg();
    if (!TargetRegisterInfo::isVirtualRegister(DestReg))
      continue;

    // Ignore non-scalar or non-integer values.
    SDValue Src = N->getOperand(2);
    EVT SrcVT = Src.getValueType();
    if (!SrcVT.isInteger() || SrcVT.isVector())
      continue;

    unsigned NumSignBits = CurDAG->ComputeNumSignBits(Src);
    Mask = APInt::getAllOnesValue(SrcVT.getSizeInBits());
    CurDAG->ComputeMaskedBits(Src, Mask, KnownZero, KnownOne);

    // Only install this information if it tells us something.
    if (NumSignBits != 1 || KnownZero != 0 || KnownOne != 0) {
      DestReg -= TargetRegisterInfo::FirstVirtualRegister;
      if (DestReg >= FuncInfo->LiveOutRegInfo.size())
        FuncInfo->LiveOutRegInfo.resize(DestReg+1);
      FunctionLoweringInfo::LiveOutInfo &LOI =
        FuncInfo->LiveOutRegInfo[DestReg];
      LOI.NumSignBits = NumSignBits;
      LOI.KnownOne = KnownOne;
      LOI.KnownZero = KnownZero;
    }
  } while (!Worklist.empty());
}

void SelectionDAGISel::CodeGenAndEmitDAG() {
  std::string GroupName;
  if (TimePassesIsEnabled)
    GroupName = "Instruction Selection and Scheduling";
  std::string BlockName;
  if (ViewDAGCombine1 || ViewLegalizeTypesDAGs || ViewLegalizeDAGs ||
      ViewDAGCombine2 || ViewDAGCombineLT || ViewISelDAGs || ViewSchedDAGs ||
      ViewSUnitDAGs)
    BlockName = MF->getFunction()->getNameStr() + ":" +
                BB->getBasicBlock()->getNameStr();

  DEBUG(dbgs() << "Initial selection DAG:\n");
  DEBUG(CurDAG->dump());

  if (ViewDAGCombine1) CurDAG->viewGraph("dag-combine1 input for " + BlockName);

  // Run the DAG combiner in pre-legalize mode.
  if (TimePassesIsEnabled) {
    NamedRegionTimer T("DAG Combining 1", GroupName);
    CurDAG->Combine(Unrestricted, *AA, OptLevel);
  } else {
    CurDAG->Combine(Unrestricted, *AA, OptLevel);
  }

  DEBUG(dbgs() << "Optimized lowered selection DAG:\n");
  DEBUG(CurDAG->dump());

  // Second step, hack on the DAG until it only uses operations and types that
  // the target supports.
  if (ViewLegalizeTypesDAGs) CurDAG->viewGraph("legalize-types input for " +
                                               BlockName);

  bool Changed;
  if (TimePassesIsEnabled) {
    NamedRegionTimer T("Type Legalization", GroupName);
    Changed = CurDAG->LegalizeTypes();
  } else {
    Changed = CurDAG->LegalizeTypes();
  }

  DEBUG(dbgs() << "Type-legalized selection DAG:\n");
  DEBUG(CurDAG->dump());

  if (Changed) {
    if (ViewDAGCombineLT)
      CurDAG->viewGraph("dag-combine-lt input for " + BlockName);

    // Run the DAG combiner in post-type-legalize mode.
    if (TimePassesIsEnabled) {
      NamedRegionTimer T("DAG Combining after legalize types", GroupName);
      CurDAG->Combine(NoIllegalTypes, *AA, OptLevel);
    } else {
      CurDAG->Combine(NoIllegalTypes, *AA, OptLevel);
    }

    DEBUG(dbgs() << "Optimized type-legalized selection DAG:\n");
    DEBUG(CurDAG->dump());
  }

  if (TimePassesIsEnabled) {
    NamedRegionTimer T("Vector Legalization", GroupName);
    Changed = CurDAG->LegalizeVectors();
  } else {
    Changed = CurDAG->LegalizeVectors();
  }

  if (Changed) {
    if (TimePassesIsEnabled) {
      NamedRegionTimer T("Type Legalization 2", GroupName);
      CurDAG->LegalizeTypes();
    } else {
      CurDAG->LegalizeTypes();
    }

    if (ViewDAGCombineLT)
      CurDAG->viewGraph("dag-combine-lv input for " + BlockName);

    // Run the DAG combiner in post-type-legalize mode.
    if (TimePassesIsEnabled) {
      NamedRegionTimer T("DAG Combining after legalize vectors", GroupName);
      CurDAG->Combine(NoIllegalOperations, *AA, OptLevel);
    } else {
      CurDAG->Combine(NoIllegalOperations, *AA, OptLevel);
    }

    DEBUG(dbgs() << "Optimized vector-legalized selection DAG:\n");
    DEBUG(CurDAG->dump());
  }

  if (ViewLegalizeDAGs) CurDAG->viewGraph("legalize input for " + BlockName);

  if (TimePassesIsEnabled) {
    NamedRegionTimer T("DAG Legalization", GroupName);
    CurDAG->Legalize(OptLevel);
  } else {
    CurDAG->Legalize(OptLevel);
  }

  DEBUG(dbgs() << "Legalized selection DAG:\n");
  DEBUG(CurDAG->dump());

  if (ViewDAGCombine2) CurDAG->viewGraph("dag-combine2 input for " + BlockName);

  // Run the DAG combiner in post-legalize mode.
  if (TimePassesIsEnabled) {
    NamedRegionTimer T("DAG Combining 2", GroupName);
    CurDAG->Combine(NoIllegalOperations, *AA, OptLevel);
  } else {
    CurDAG->Combine(NoIllegalOperations, *AA, OptLevel);
  }

  DEBUG(dbgs() << "Optimized legalized selection DAG:\n");
  DEBUG(CurDAG->dump());

  if (OptLevel != CodeGenOpt::None) {
    ShrinkDemandedOps();
    ComputeLiveOutVRegInfo();
  }

  if (ViewISelDAGs) CurDAG->viewGraph("isel input for " + BlockName);

  // Third, instruction select all of the operations to machine code, adding the
  // code to the MachineBasicBlock.
  if (TimePassesIsEnabled) {
    NamedRegionTimer T("Instruction Selection", GroupName);
    DoInstructionSelection();
  } else {
    DoInstructionSelection();
  }

  DEBUG(dbgs() << "Selected selection DAG:\n");
  DEBUG(CurDAG->dump());

  if (ViewSchedDAGs) CurDAG->viewGraph("scheduler input for " + BlockName);

  // Schedule machine code.
  ScheduleDAGSDNodes *Scheduler = CreateScheduler();
  if (TimePassesIsEnabled) {
    NamedRegionTimer T("Instruction Scheduling", GroupName);
    Scheduler->Run(CurDAG, BB, BB->end());
  } else {
    Scheduler->Run(CurDAG, BB, BB->end());
  }

  if (ViewSUnitDAGs) Scheduler->viewGraph();

  // Emit machine code to BB.  This can change 'BB' to the last block being
  // inserted into.
  if (TimePassesIsEnabled) {
    NamedRegionTimer T("Instruction Creation", GroupName);
    BB = Scheduler->EmitSchedule(&SDB->EdgeMapping);
  } else {
    BB = Scheduler->EmitSchedule(&SDB->EdgeMapping);
  }

  // Free the scheduler state.
  if (TimePassesIsEnabled) {
    NamedRegionTimer T("Instruction Scheduling Cleanup", GroupName);
    delete Scheduler;
  } else {
    delete Scheduler;
  }

  DEBUG(dbgs() << "Selected machine code:\n");
  DEBUG(BB->dump());
}

void SelectionDAGISel::DoInstructionSelection() {
  DEBUG(errs() << "===== Instruction selection begins:\n");

  PreprocessISelDAG();
  
  // Select target instructions for the DAG.
  {
    // Number all nodes with a topological order and set DAGSize.
    DAGSize = CurDAG->AssignTopologicalOrder();
    
    // Create a dummy node (which is not added to allnodes), that adds
    // a reference to the root node, preventing it from being deleted,
    // and tracking any changes of the root.
    HandleSDNode Dummy(CurDAG->getRoot());
    ISelPosition = SelectionDAG::allnodes_iterator(CurDAG->getRoot().getNode());
    ++ISelPosition;
    
    // The AllNodes list is now topological-sorted. Visit the
    // nodes by starting at the end of the list (the root of the
    // graph) and preceding back toward the beginning (the entry
    // node).
    while (ISelPosition != CurDAG->allnodes_begin()) {
      SDNode *Node = --ISelPosition;
      // Skip dead nodes. DAGCombiner is expected to eliminate all dead nodes,
      // but there are currently some corner cases that it misses. Also, this
      // makes it theoretically possible to disable the DAGCombiner.
      if (Node->use_empty())
        continue;
      
      SDNode *ResNode = Select(Node);
      
      // FIXME: This is pretty gross.  'Select' should be changed to not return
      // anything at all and this code should be nuked with a tactical strike.
      
      // If node should not be replaced, continue with the next one.
      if (ResNode == Node || Node->getOpcode() == ISD::DELETED_NODE)
        continue;
      // Replace node.
      if (ResNode)
        ReplaceUses(Node, ResNode);
      
      // If after the replacement this node is not used any more,
      // remove this dead node.
      if (Node->use_empty()) { // Don't delete EntryToken, etc.
        ISelUpdater ISU(ISelPosition);
        CurDAG->RemoveDeadNode(Node, &ISU);
      }
    }
    
    CurDAG->setRoot(Dummy.getValue());
  }    
  DEBUG(errs() << "===== Instruction selection ends:\n");

  PostprocessISelDAG();
}


void SelectionDAGISel::SelectAllBasicBlocks(Function &Fn,
                                            MachineFunction &MF,
                                            const TargetInstrInfo &TII) {
  // Initialize the Fast-ISel state, if needed.
  FastISel *FastIS = 0;
  if (EnableFastISel)
    FastIS = TLI.createFastISel(MF, FuncInfo->ValueMap, FuncInfo->MBBMap,
                                FuncInfo->StaticAllocaMap
#ifndef NDEBUG
                                , FuncInfo->CatchInfoLost
#endif
                                );

  // Iterate over all basic blocks in the function.
  for (Function::iterator I = Fn.begin(), E = Fn.end(); I != E; ++I) {
    BasicBlock *LLVMBB = &*I;
    BB = FuncInfo->MBBMap[LLVMBB];

    BasicBlock::iterator const Begin = LLVMBB->begin();
    BasicBlock::iterator const End = LLVMBB->end();
    BasicBlock::iterator BI = Begin;

    // Lower any arguments needed in this block if this is the entry block.
    bool SuppressFastISel = false;
    if (LLVMBB == &Fn.getEntryBlock()) {
      LowerArguments(LLVMBB);

      // If any of the arguments has the byval attribute, forgo
      // fast-isel in the entry block.
      if (FastIS) {
        unsigned j = 1;
        for (Function::arg_iterator I = Fn.arg_begin(), E = Fn.arg_end();
             I != E; ++I, ++j)
          if (Fn.paramHasAttr(j, Attribute::ByVal)) {
            if (EnableFastISelVerbose || EnableFastISelAbort)
              dbgs() << "FastISel skips entry block due to byval argument\n";
            SuppressFastISel = true;
            break;
          }
      }
    }

    if (BB->isLandingPad()) {
      // Add a label to mark the beginning of the landing pad.  Deletion of the
      // landing pad can thus be detected via the MachineModuleInfo.
      MCSymbol *Label = MF.getMMI().addLandingPad(BB);

      const TargetInstrDesc &II = TII.get(TargetOpcode::EH_LABEL);
      BuildMI(BB, SDB->getCurDebugLoc(), II).addSym(Label);

      // Mark exception register as live in.
      unsigned Reg = TLI.getExceptionAddressRegister();
      if (Reg) BB->addLiveIn(Reg);

      // Mark exception selector register as live in.
      Reg = TLI.getExceptionSelectorRegister();
      if (Reg) BB->addLiveIn(Reg);

      // FIXME: Hack around an exception handling flaw (PR1508): the personality
      // function and list of typeids logically belong to the invoke (or, if you
      // like, the basic block containing the invoke), and need to be associated
      // with it in the dwarf exception handling tables.  Currently however the
      // information is provided by an intrinsic (eh.selector) that can be moved
      // to unexpected places by the optimizers: if the unwind edge is critical,
      // then breaking it can result in the intrinsics being in the successor of
      // the landing pad, not the landing pad itself.  This results
      // in exceptions not being caught because no typeids are associated with
      // the invoke.  This may not be the only way things can go wrong, but it
      // is the only way we try to work around for the moment.
      BranchInst *Br = dyn_cast<BranchInst>(LLVMBB->getTerminator());

      if (Br && Br->isUnconditional()) { // Critical edge?
        BasicBlock::iterator I, E;
        for (I = LLVMBB->begin(), E = --LLVMBB->end(); I != E; ++I)
          if (isa<EHSelectorInst>(I))
            break;

        if (I == E)
          // No catch info found - try to extract some from the successor.
          CopyCatchInfo(Br->getSuccessor(0), LLVMBB, &MF.getMMI(), *FuncInfo);
      }
    }

    // Before doing SelectionDAG ISel, see if FastISel has been requested.
    if (FastIS && !SuppressFastISel) {
      // Emit code for any incoming arguments. This must happen before
      // beginning FastISel on the entry block.
      if (LLVMBB == &Fn.getEntryBlock()) {
        CurDAG->setRoot(SDB->getControlRoot());
        CodeGenAndEmitDAG();
        SDB->clear();
      }
      FastIS->startNewBlock(BB);
      // Do FastISel on as many instructions as possible.
      for (; BI != End; ++BI) {
        // Just before the terminator instruction, insert instructions to
        // feed PHI nodes in successor blocks.
        if (isa<TerminatorInst>(BI))
          if (!HandlePHINodesInSuccessorBlocksFast(LLVMBB, FastIS)) {
            ++NumFastIselFailures;
            ResetDebugLoc(SDB, FastIS);
            if (EnableFastISelVerbose || EnableFastISelAbort) {
              dbgs() << "FastISel miss: ";
              BI->dump();
            }
            assert(!EnableFastISelAbort &&
                   "FastISel didn't handle a PHI in a successor");
            break;
          }

        SetDebugLoc(BI, SDB, FastIS, &MF);

        // Try to select the instruction with FastISel.
        if (FastIS->SelectInstruction(BI)) {
          ResetDebugLoc(SDB, FastIS);
          continue;
        }

        // Clear out the debug location so that it doesn't carry over to
        // unrelated instructions.
        ResetDebugLoc(SDB, FastIS);

        // Then handle certain instructions as single-LLVM-Instruction blocks.
        if (isa<CallInst>(BI)) {
          ++NumFastIselFailures;
          if (EnableFastISelVerbose || EnableFastISelAbort) {
            dbgs() << "FastISel missed call: ";
            BI->dump();
          }

          if (!BI->getType()->isVoidTy()) {
            unsigned &R = FuncInfo->ValueMap[BI];
            if (!R)
              R = FuncInfo->CreateRegForValue(BI);
          }

          bool HadTailCall = false;
          SelectBasicBlock(LLVMBB, BI, llvm::next(BI), HadTailCall);

          // If the call was emitted as a tail call, we're done with the block.
          if (HadTailCall) {
            BI = End;
            break;
          }

          // If the instruction was codegen'd with multiple blocks,
          // inform the FastISel object where to resume inserting.
          FastIS->setCurrentBlock(BB);
          continue;
        }

        // Otherwise, give up on FastISel for the rest of the block.
        // For now, be a little lenient about non-branch terminators.
        if (!isa<TerminatorInst>(BI) || isa<BranchInst>(BI)) {
          ++NumFastIselFailures;
          if (EnableFastISelVerbose || EnableFastISelAbort) {
            dbgs() << "FastISel miss: ";
            BI->dump();
          }
          if (EnableFastISelAbort)
            // The "fast" selector couldn't handle something and bailed.
            // For the purpose of debugging, just abort.
            llvm_unreachable("FastISel didn't select the entire block");
        }
        break;
      }
    }

    // Run SelectionDAG instruction selection on the remainder of the block
    // not handled by FastISel. If FastISel is not run, this is the entire
    // block.
    if (BI != End) {
      bool HadTailCall;
      SelectBasicBlock(LLVMBB, BI, End, HadTailCall);
    }

    FinishBasicBlock();
  }

  delete FastIS;
}

void
SelectionDAGISel::FinishBasicBlock() {

  DEBUG(dbgs() << "Target-post-processed machine code:\n");
  DEBUG(BB->dump());

  DEBUG(dbgs() << "Total amount of phi nodes to update: "
               << SDB->PHINodesToUpdate.size() << "\n");
  DEBUG(for (unsigned i = 0, e = SDB->PHINodesToUpdate.size(); i != e; ++i)
          dbgs() << "Node " << i << " : ("
                 << SDB->PHINodesToUpdate[i].first
                 << ", " << SDB->PHINodesToUpdate[i].second << ")\n");

  // Next, now that we know what the last MBB the LLVM BB expanded is, update
  // PHI nodes in successors.
  if (SDB->SwitchCases.empty() &&
      SDB->JTCases.empty() &&
      SDB->BitTestCases.empty()) {
    for (unsigned i = 0, e = SDB->PHINodesToUpdate.size(); i != e; ++i) {
      MachineInstr *PHI = SDB->PHINodesToUpdate[i].first;
      assert(PHI->isPHI() &&
             "This is not a machine PHI node that we are updating!");
      if (!BB->isSuccessor(PHI->getParent()))
        continue;
      PHI->addOperand(MachineOperand::CreateReg(SDB->PHINodesToUpdate[i].second,
                                                false));
      PHI->addOperand(MachineOperand::CreateMBB(BB));
    }
    SDB->PHINodesToUpdate.clear();
    return;
  }

  for (unsigned i = 0, e = SDB->BitTestCases.size(); i != e; ++i) {
    // Lower header first, if it wasn't already lowered
    if (!SDB->BitTestCases[i].Emitted) {
      // Set the current basic block to the mbb we wish to insert the code into
      BB = SDB->BitTestCases[i].Parent;
      SDB->setCurrentBasicBlock(BB);
      // Emit the code
      SDB->visitBitTestHeader(SDB->BitTestCases[i]);
      CurDAG->setRoot(SDB->getRoot());
      CodeGenAndEmitDAG();
      SDB->clear();
    }

    for (unsigned j = 0, ej = SDB->BitTestCases[i].Cases.size(); j != ej; ++j) {
      // Set the current basic block to the mbb we wish to insert the code into
      BB = SDB->BitTestCases[i].Cases[j].ThisBB;
      SDB->setCurrentBasicBlock(BB);
      // Emit the code
      if (j+1 != ej)
        SDB->visitBitTestCase(SDB->BitTestCases[i].Cases[j+1].ThisBB,
                              SDB->BitTestCases[i].Reg,
                              SDB->BitTestCases[i].Cases[j]);
      else
        SDB->visitBitTestCase(SDB->BitTestCases[i].Default,
                              SDB->BitTestCases[i].Reg,
                              SDB->BitTestCases[i].Cases[j]);


      CurDAG->setRoot(SDB->getRoot());
      CodeGenAndEmitDAG();
      SDB->clear();
    }

    // Update PHI Nodes
    for (unsigned pi = 0, pe = SDB->PHINodesToUpdate.size(); pi != pe; ++pi) {
      MachineInstr *PHI = SDB->PHINodesToUpdate[pi].first;
      MachineBasicBlock *PHIBB = PHI->getParent();
      assert(PHI->isPHI() &&
             "This is not a machine PHI node that we are updating!");
      // This is "default" BB. We have two jumps to it. From "header" BB and
      // from last "case" BB.
      if (PHIBB == SDB->BitTestCases[i].Default) {
        PHI->addOperand(MachineOperand::
                        CreateReg(SDB->PHINodesToUpdate[pi].second, false));
        PHI->addOperand(MachineOperand::CreateMBB(SDB->BitTestCases[i].Parent));
        PHI->addOperand(MachineOperand::
                        CreateReg(SDB->PHINodesToUpdate[pi].second, false));
        PHI->addOperand(MachineOperand::CreateMBB(SDB->BitTestCases[i].Cases.
                                                  back().ThisBB));
      }
      // One of "cases" BB.
      for (unsigned j = 0, ej = SDB->BitTestCases[i].Cases.size();
           j != ej; ++j) {
        MachineBasicBlock* cBB = SDB->BitTestCases[i].Cases[j].ThisBB;
        if (cBB->isSuccessor(PHIBB)) {
          PHI->addOperand(MachineOperand::
                          CreateReg(SDB->PHINodesToUpdate[pi].second, false));
          PHI->addOperand(MachineOperand::CreateMBB(cBB));
        }
      }
    }
  }
  SDB->BitTestCases.clear();

  // If the JumpTable record is filled in, then we need to emit a jump table.
  // Updating the PHI nodes is tricky in this case, since we need to determine
  // whether the PHI is a successor of the range check MBB or the jump table MBB
  for (unsigned i = 0, e = SDB->JTCases.size(); i != e; ++i) {
    // Lower header first, if it wasn't already lowered
    if (!SDB->JTCases[i].first.Emitted) {
      // Set the current basic block to the mbb we wish to insert the code into
      BB = SDB->JTCases[i].first.HeaderBB;
      SDB->setCurrentBasicBlock(BB);
      // Emit the code
      SDB->visitJumpTableHeader(SDB->JTCases[i].second, SDB->JTCases[i].first);
      CurDAG->setRoot(SDB->getRoot());
      CodeGenAndEmitDAG();
      SDB->clear();
    }

    // Set the current basic block to the mbb we wish to insert the code into
    BB = SDB->JTCases[i].second.MBB;
    SDB->setCurrentBasicBlock(BB);
    // Emit the code
    SDB->visitJumpTable(SDB->JTCases[i].second);
    CurDAG->setRoot(SDB->getRoot());
    CodeGenAndEmitDAG();
    SDB->clear();

    // Update PHI Nodes
    for (unsigned pi = 0, pe = SDB->PHINodesToUpdate.size(); pi != pe; ++pi) {
      MachineInstr *PHI = SDB->PHINodesToUpdate[pi].first;
      MachineBasicBlock *PHIBB = PHI->getParent();
      assert(PHI->isPHI() &&
             "This is not a machine PHI node that we are updating!");
      // "default" BB. We can go there only from header BB.
      if (PHIBB == SDB->JTCases[i].second.Default) {
        PHI->addOperand
          (MachineOperand::CreateReg(SDB->PHINodesToUpdate[pi].second, false));
        PHI->addOperand
          (MachineOperand::CreateMBB(SDB->JTCases[i].first.HeaderBB));
      }
      // JT BB. Just iterate over successors here
      if (BB->isSuccessor(PHIBB)) {
        PHI->addOperand
          (MachineOperand::CreateReg(SDB->PHINodesToUpdate[pi].second, false));
        PHI->addOperand(MachineOperand::CreateMBB(BB));
      }
    }
  }
  SDB->JTCases.clear();

  // If the switch block involved a branch to one of the actual successors, we
  // need to update PHI nodes in that block.
  for (unsigned i = 0, e = SDB->PHINodesToUpdate.size(); i != e; ++i) {
    MachineInstr *PHI = SDB->PHINodesToUpdate[i].first;
    assert(PHI->isPHI() &&
           "This is not a machine PHI node that we are updating!");
    if (BB->isSuccessor(PHI->getParent())) {
      PHI->addOperand(MachineOperand::CreateReg(SDB->PHINodesToUpdate[i].second,
                                                false));
      PHI->addOperand(MachineOperand::CreateMBB(BB));
    }
  }

  // If we generated any switch lowering information, build and codegen any
  // additional DAGs necessary.
  for (unsigned i = 0, e = SDB->SwitchCases.size(); i != e; ++i) {
    // Set the current basic block to the mbb we wish to insert the code into
    MachineBasicBlock *ThisBB = BB = SDB->SwitchCases[i].ThisBB;
    SDB->setCurrentBasicBlock(BB);

    // Emit the code
    SDB->visitSwitchCase(SDB->SwitchCases[i]);
    CurDAG->setRoot(SDB->getRoot());
    CodeGenAndEmitDAG();

    // Handle any PHI nodes in successors of this chunk, as if we were coming
    // from the original BB before switch expansion.  Note that PHI nodes can
    // occur multiple times in PHINodesToUpdate.  We have to be very careful to
    // handle them the right number of times.
    while ((BB = SDB->SwitchCases[i].TrueBB)) {  // Handle LHS and RHS.
      // If new BB's are created during scheduling, the edges may have been
      // updated. That is, the edge from ThisBB to BB may have been split and
      // BB's predecessor is now another block.
      DenseMap<MachineBasicBlock*, MachineBasicBlock*>::iterator EI =
        SDB->EdgeMapping.find(BB);
      if (EI != SDB->EdgeMapping.end())
        ThisBB = EI->second;

      // BB may have been removed from the CFG if a branch was constant folded.
      if (ThisBB->isSuccessor(BB)) {
        for (MachineBasicBlock::iterator Phi = BB->begin();
             Phi != BB->end() && Phi->isPHI();
             ++Phi) {
          // This value for this PHI node is recorded in PHINodesToUpdate.
          for (unsigned pn = 0; ; ++pn) {
            assert(pn != SDB->PHINodesToUpdate.size() &&
                   "Didn't find PHI entry!");
            if (SDB->PHINodesToUpdate[pn].first == Phi) {
              Phi->addOperand(MachineOperand::
                              CreateReg(SDB->PHINodesToUpdate[pn].second,
                                        false));
              Phi->addOperand(MachineOperand::CreateMBB(ThisBB));
              break;
            }
          }
        }
      }

      // Don't process RHS if same block as LHS.
      if (BB == SDB->SwitchCases[i].FalseBB)
        SDB->SwitchCases[i].FalseBB = 0;

      // If we haven't handled the RHS, do so now.  Otherwise, we're done.
      SDB->SwitchCases[i].TrueBB = SDB->SwitchCases[i].FalseBB;
      SDB->SwitchCases[i].FalseBB = 0;
    }
    assert(SDB->SwitchCases[i].TrueBB == 0 && SDB->SwitchCases[i].FalseBB == 0);
    SDB->clear();
  }
  SDB->SwitchCases.clear();

  SDB->PHINodesToUpdate.clear();
}


/// Create the scheduler. If a specific scheduler was specified
/// via the SchedulerRegistry, use it, otherwise select the
/// one preferred by the target.
///
ScheduleDAGSDNodes *SelectionDAGISel::CreateScheduler() {
  RegisterScheduler::FunctionPassCtor Ctor = RegisterScheduler::getDefault();

  if (!Ctor) {
    Ctor = ISHeuristic;
    RegisterScheduler::setDefault(Ctor);
  }

  return Ctor(this, OptLevel);
}

ScheduleHazardRecognizer *SelectionDAGISel::CreateTargetHazardRecognizer() {
  return new ScheduleHazardRecognizer();
}

//===----------------------------------------------------------------------===//
// Helper functions used by the generated instruction selector.
//===----------------------------------------------------------------------===//
// Calls to these methods are generated by tblgen.

/// CheckAndMask - The isel is trying to match something like (and X, 255).  If
/// the dag combiner simplified the 255, we still want to match.  RHS is the
/// actual value in the DAG on the RHS of an AND, and DesiredMaskS is the value
/// specified in the .td file (e.g. 255).
bool SelectionDAGISel::CheckAndMask(SDValue LHS, ConstantSDNode *RHS,
                                    int64_t DesiredMaskS) const {
  const APInt &ActualMask = RHS->getAPIntValue();
  const APInt &DesiredMask = APInt(LHS.getValueSizeInBits(), DesiredMaskS);

  // If the actual mask exactly matches, success!
  if (ActualMask == DesiredMask)
    return true;

  // If the actual AND mask is allowing unallowed bits, this doesn't match.
  if (ActualMask.intersects(~DesiredMask))
    return false;

  // Otherwise, the DAG Combiner may have proven that the value coming in is
  // either already zero or is not demanded.  Check for known zero input bits.
  APInt NeededMask = DesiredMask & ~ActualMask;
  if (CurDAG->MaskedValueIsZero(LHS, NeededMask))
    return true;

  // TODO: check to see if missing bits are just not demanded.

  // Otherwise, this pattern doesn't match.
  return false;
}

/// CheckOrMask - The isel is trying to match something like (or X, 255).  If
/// the dag combiner simplified the 255, we still want to match.  RHS is the
/// actual value in the DAG on the RHS of an OR, and DesiredMaskS is the value
/// specified in the .td file (e.g. 255).
bool SelectionDAGISel::CheckOrMask(SDValue LHS, ConstantSDNode *RHS,
                                   int64_t DesiredMaskS) const {
  const APInt &ActualMask = RHS->getAPIntValue();
  const APInt &DesiredMask = APInt(LHS.getValueSizeInBits(), DesiredMaskS);

  // If the actual mask exactly matches, success!
  if (ActualMask == DesiredMask)
    return true;

  // If the actual AND mask is allowing unallowed bits, this doesn't match.
  if (ActualMask.intersects(~DesiredMask))
    return false;

  // Otherwise, the DAG Combiner may have proven that the value coming in is
  // either already zero or is not demanded.  Check for known zero input bits.
  APInt NeededMask = DesiredMask & ~ActualMask;

  APInt KnownZero, KnownOne;
  CurDAG->ComputeMaskedBits(LHS, NeededMask, KnownZero, KnownOne);

  // If all the missing bits in the or are already known to be set, match!
  if ((NeededMask & KnownOne) == NeededMask)
    return true;

  // TODO: check to see if missing bits are just not demanded.

  // Otherwise, this pattern doesn't match.
  return false;
}


/// SelectInlineAsmMemoryOperands - Calls to this are automatically generated
/// by tblgen.  Others should not call it.
void SelectionDAGISel::
SelectInlineAsmMemoryOperands(std::vector<SDValue> &Ops) {
  std::vector<SDValue> InOps;
  std::swap(InOps, Ops);

  Ops.push_back(InOps[InlineAsm::Op_InputChain]); // 0
  Ops.push_back(InOps[InlineAsm::Op_AsmString]);  // 1
  Ops.push_back(InOps[InlineAsm::Op_MDNode]);     // 2, !srcloc

  unsigned i = InlineAsm::Op_FirstOperand, e = InOps.size();
  if (InOps[e-1].getValueType() == MVT::Flag)
    --e;  // Don't process a flag operand if it is here.

  while (i != e) {
    unsigned Flags = cast<ConstantSDNode>(InOps[i])->getZExtValue();
    if (!InlineAsm::isMemKind(Flags)) {
      // Just skip over this operand, copying the operands verbatim.
      Ops.insert(Ops.end(), InOps.begin()+i,
                 InOps.begin()+i+InlineAsm::getNumOperandRegisters(Flags) + 1);
      i += InlineAsm::getNumOperandRegisters(Flags) + 1;
    } else {
      assert(InlineAsm::getNumOperandRegisters(Flags) == 1 &&
             "Memory operand with multiple values?");
      // Otherwise, this is a memory operand.  Ask the target to select it.
      std::vector<SDValue> SelOps;
      if (SelectInlineAsmMemoryOperand(InOps[i+1], 'm', SelOps))
        report_fatal_error("Could not match memory address.  Inline asm"
                           " failure!");

      // Add this to the output node.
      unsigned NewFlags =
        InlineAsm::getFlagWord(InlineAsm::Kind_Mem, SelOps.size());
      Ops.push_back(CurDAG->getTargetConstant(NewFlags, MVT::i32));
      Ops.insert(Ops.end(), SelOps.begin(), SelOps.end());
      i += 2;
    }
  }

  // Add the flag input back if present.
  if (e != InOps.size())
    Ops.push_back(InOps.back());
}

/// findFlagUse - Return use of EVT::Flag value produced by the specified
/// SDNode.
///
static SDNode *findFlagUse(SDNode *N) {
  unsigned FlagResNo = N->getNumValues()-1;
  for (SDNode::use_iterator I = N->use_begin(), E = N->use_end(); I != E; ++I) {
    SDUse &Use = I.getUse();
    if (Use.getResNo() == FlagResNo)
      return Use.getUser();
  }
  return NULL;
}

/// findNonImmUse - Return true if "Use" is a non-immediate use of "Def".
/// This function recursively traverses up the operand chain, ignoring
/// certain nodes.
static bool findNonImmUse(SDNode *Use, SDNode* Def, SDNode *ImmedUse,
                          SDNode *Root, SmallPtrSet<SDNode*, 16> &Visited,
                          bool IgnoreChains) {
  // The NodeID's are given uniques ID's where a node ID is guaranteed to be
  // greater than all of its (recursive) operands.  If we scan to a point where
  // 'use' is smaller than the node we're scanning for, then we know we will
  // never find it.
  //
  // The Use may be -1 (unassigned) if it is a newly allocated node.  This can
  // happen because we scan down to newly selected nodes in the case of flag
  // uses.
  if ((Use->getNodeId() < Def->getNodeId() && Use->getNodeId() != -1))
    return false;
  
  // Don't revisit nodes if we already scanned it and didn't fail, we know we
  // won't fail if we scan it again.
  if (!Visited.insert(Use))
    return false;

  for (unsigned i = 0, e = Use->getNumOperands(); i != e; ++i) {
    // Ignore chain uses, they are validated by HandleMergeInputChains.
    if (Use->getOperand(i).getValueType() == MVT::Other && IgnoreChains)
      continue;
    
    SDNode *N = Use->getOperand(i).getNode();
    if (N == Def) {
      if (Use == ImmedUse || Use == Root)
        continue;  // We are not looking for immediate use.
      assert(N != Root);
      return true;
    }

    // Traverse up the operand chain.
    if (findNonImmUse(N, Def, ImmedUse, Root, Visited, IgnoreChains))
      return true;
  }
  return false;
}

/// IsProfitableToFold - Returns true if it's profitable to fold the specific
/// operand node N of U during instruction selection that starts at Root.
bool SelectionDAGISel::IsProfitableToFold(SDValue N, SDNode *U,
                                          SDNode *Root) const {
  if (OptLevel == CodeGenOpt::None) return false;
  return N.hasOneUse();
}

/// IsLegalToFold - Returns true if the specific operand node N of
/// U can be folded during instruction selection that starts at Root.
bool SelectionDAGISel::IsLegalToFold(SDValue N, SDNode *U, SDNode *Root,
                                     bool IgnoreChains) const {
  if (OptLevel == CodeGenOpt::None) return false;

  // If Root use can somehow reach N through a path that that doesn't contain
  // U then folding N would create a cycle. e.g. In the following
  // diagram, Root can reach N through X. If N is folded into into Root, then
  // X is both a predecessor and a successor of U.
  //
  //          [N*]           //
  //         ^   ^           //
  //        /     \          //
  //      [U*]    [X]?       //
  //        ^     ^          //
  //         \   /           //
  //          \ /            //
  //         [Root*]         //
  //
  // * indicates nodes to be folded together.
  //
  // If Root produces a flag, then it gets (even more) interesting. Since it
  // will be "glued" together with its flag use in the scheduler, we need to
  // check if it might reach N.
  //
  //          [N*]           //
  //         ^   ^           //
  //        /     \          //
  //      [U*]    [X]?       //
  //        ^       ^        //
  //         \       \       //
  //          \      |       //
  //         [Root*] |       //
  //          ^      |       //
  //          f      |       //
  //          |      /       //
  //         [Y]    /        //
  //           ^   /         //
  //           f  /          //
  //           | /           //
  //          [FU]           //
  //
  // If FU (flag use) indirectly reaches N (the load), and Root folds N
  // (call it Fold), then X is a predecessor of FU and a successor of
  // Fold. But since Fold and FU are flagged together, this will create
  // a cycle in the scheduling graph.

  // If the node has flags, walk down the graph to the "lowest" node in the
  // flagged set.
  EVT VT = Root->getValueType(Root->getNumValues()-1);
  while (VT == MVT::Flag) {
    SDNode *FU = findFlagUse(Root);
    if (FU == NULL)
      break;
    Root = FU;
    VT = Root->getValueType(Root->getNumValues()-1);
    
    // If our query node has a flag result with a use, we've walked up it.  If
    // the user (which has already been selected) has a chain or indirectly uses
    // the chain, our WalkChainUsers predicate will not consider it.  Because of
    // this, we cannot ignore chains in this predicate.
    IgnoreChains = false;
  }
  

  SmallPtrSet<SDNode*, 16> Visited;
  return !findNonImmUse(Root, N.getNode(), U, Root, Visited, IgnoreChains);
}

SDNode *SelectionDAGISel::Select_INLINEASM(SDNode *N) {
  std::vector<SDValue> Ops(N->op_begin(), N->op_end());
  SelectInlineAsmMemoryOperands(Ops);
    
  std::vector<EVT> VTs;
  VTs.push_back(MVT::Other);
  VTs.push_back(MVT::Flag);
  SDValue New = CurDAG->getNode(ISD::INLINEASM, N->getDebugLoc(),
                                VTs, &Ops[0], Ops.size());
  New->setNodeId(-1);
  return New.getNode();
}

SDNode *SelectionDAGISel::Select_UNDEF(SDNode *N) {
  return CurDAG->SelectNodeTo(N, TargetOpcode::IMPLICIT_DEF,N->getValueType(0));
}

/// GetVBR - decode a vbr encoding whose top bit is set.
ALWAYS_INLINE static uint64_t
GetVBR(uint64_t Val, const unsigned char *MatcherTable, unsigned &Idx) {
  assert(Val >= 128 && "Not a VBR");
  Val &= 127;  // Remove first vbr bit.
  
  unsigned Shift = 7;
  uint64_t NextBits;
  do {
    NextBits = MatcherTable[Idx++];
    Val |= (NextBits&127) << Shift;
    Shift += 7;
  } while (NextBits & 128);
  
  return Val;
}


/// UpdateChainsAndFlags - When a match is complete, this method updates uses of
/// interior flag and chain results to use the new flag and chain results.
void SelectionDAGISel::
UpdateChainsAndFlags(SDNode *NodeToMatch, SDValue InputChain,
                     const SmallVectorImpl<SDNode*> &ChainNodesMatched,
                     SDValue InputFlag,
                     const SmallVectorImpl<SDNode*> &FlagResultNodesMatched,
                     bool isMorphNodeTo) {
  SmallVector<SDNode*, 4> NowDeadNodes;
  
  ISelUpdater ISU(ISelPosition);

  // Now that all the normal results are replaced, we replace the chain and
  // flag results if present.
  if (!ChainNodesMatched.empty()) {
    assert(InputChain.getNode() != 0 &&
           "Matched input chains but didn't produce a chain");
    // Loop over all of the nodes we matched that produced a chain result.
    // Replace all the chain results with the final chain we ended up with.
    for (unsigned i = 0, e = ChainNodesMatched.size(); i != e; ++i) {
      SDNode *ChainNode = ChainNodesMatched[i];
      
      // If this node was already deleted, don't look at it.
      if (ChainNode->getOpcode() == ISD::DELETED_NODE)
        continue;
      
      // Don't replace the results of the root node if we're doing a
      // MorphNodeTo.
      if (ChainNode == NodeToMatch && isMorphNodeTo)
        continue;
      
      SDValue ChainVal = SDValue(ChainNode, ChainNode->getNumValues()-1);
      if (ChainVal.getValueType() == MVT::Flag)
        ChainVal = ChainVal.getValue(ChainVal->getNumValues()-2);
      assert(ChainVal.getValueType() == MVT::Other && "Not a chain?");
      CurDAG->ReplaceAllUsesOfValueWith(ChainVal, InputChain, &ISU);
      
      // If the node became dead and we haven't already seen it, delete it.
      if (ChainNode->use_empty() &&
          !std::count(NowDeadNodes.begin(), NowDeadNodes.end(), ChainNode))
        NowDeadNodes.push_back(ChainNode);
    }
  }
  
  // If the result produces a flag, update any flag results in the matched
  // pattern with the flag result.
  if (InputFlag.getNode() != 0) {
    // Handle any interior nodes explicitly marked.
    for (unsigned i = 0, e = FlagResultNodesMatched.size(); i != e; ++i) {
      SDNode *FRN = FlagResultNodesMatched[i];
      
      // If this node was already deleted, don't look at it.
      if (FRN->getOpcode() == ISD::DELETED_NODE)
        continue;
      
      assert(FRN->getValueType(FRN->getNumValues()-1) == MVT::Flag &&
             "Doesn't have a flag result");
      CurDAG->ReplaceAllUsesOfValueWith(SDValue(FRN, FRN->getNumValues()-1),
                                        InputFlag, &ISU);
      
      // If the node became dead and we haven't already seen it, delete it.
      if (FRN->use_empty() &&
          !std::count(NowDeadNodes.begin(), NowDeadNodes.end(), FRN))
        NowDeadNodes.push_back(FRN);
    }
  }
  
  if (!NowDeadNodes.empty())
    CurDAG->RemoveDeadNodes(NowDeadNodes, &ISU);
  
  DEBUG(errs() << "ISEL: Match complete!\n");
}

enum ChainResult {
  CR_Simple,
  CR_InducesCycle,
  CR_LeadsToInteriorNode
};

/// WalkChainUsers - Walk down the users of the specified chained node that is
/// part of the pattern we're matching, looking at all of the users we find.
/// This determines whether something is an interior node, whether we have a
/// non-pattern node in between two pattern nodes (which prevent folding because
/// it would induce a cycle) and whether we have a TokenFactor node sandwiched
/// between pattern nodes (in which case the TF becomes part of the pattern).
///
/// The walk we do here is guaranteed to be small because we quickly get down to
/// already selected nodes "below" us.
static ChainResult 
WalkChainUsers(SDNode *ChainedNode,
               SmallVectorImpl<SDNode*> &ChainedNodesInPattern,
               SmallVectorImpl<SDNode*> &InteriorChainedNodes) {
  ChainResult Result = CR_Simple;
  
  for (SDNode::use_iterator UI = ChainedNode->use_begin(),
         E = ChainedNode->use_end(); UI != E; ++UI) {
    // Make sure the use is of the chain, not some other value we produce.
    if (UI.getUse().getValueType() != MVT::Other) continue;
    
    SDNode *User = *UI;

    // If we see an already-selected machine node, then we've gone beyond the
    // pattern that we're selecting down into the already selected chunk of the
    // DAG.
    if (User->isMachineOpcode() ||
        User->getOpcode() == ISD::HANDLENODE)  // Root of the graph.
      continue;
    
    if (User->getOpcode() == ISD::CopyToReg ||
        User->getOpcode() == ISD::CopyFromReg ||
        User->getOpcode() == ISD::INLINEASM ||
        User->getOpcode() == ISD::EH_LABEL) {
      // If their node ID got reset to -1 then they've already been selected.
      // Treat them like a MachineOpcode.
      if (User->getNodeId() == -1)
        continue;
    }

    // If we have a TokenFactor, we handle it specially.
    if (User->getOpcode() != ISD::TokenFactor) {
      // If the node isn't a token factor and isn't part of our pattern, then it
      // must be a random chained node in between two nodes we're selecting.
      // This happens when we have something like:
      //   x = load ptr
      //   call
      //   y = x+4
      //   store y -> ptr
      // Because we structurally match the load/store as a read/modify/write,
      // but the call is chained between them.  We cannot fold in this case
      // because it would induce a cycle in the graph.
      if (!std::count(ChainedNodesInPattern.begin(),
                      ChainedNodesInPattern.end(), User))
        return CR_InducesCycle;
      
      // Otherwise we found a node that is part of our pattern.  For example in:
      //   x = load ptr
      //   y = x+4
      //   store y -> ptr
      // This would happen when we're scanning down from the load and see the
      // store as a user.  Record that there is a use of ChainedNode that is
      // part of the pattern and keep scanning uses.
      Result = CR_LeadsToInteriorNode;
      InteriorChainedNodes.push_back(User);
      continue;
    }
    
    // If we found a TokenFactor, there are two cases to consider: first if the
    // TokenFactor is just hanging "below" the pattern we're matching (i.e. no
    // uses of the TF are in our pattern) we just want to ignore it.  Second,
    // the TokenFactor can be sandwiched in between two chained nodes, like so:
    //     [Load chain]
    //         ^
    //         |
    //       [Load]
    //       ^    ^
    //       |    \                    DAG's like cheese
    //      /       \                       do you?
    //     /         |
    // [TokenFactor] [Op]
    //     ^          ^
    //     |          |
    //      \        /
    //       \      /
    //       [Store]
    //
    // In this case, the TokenFactor becomes part of our match and we rewrite it
    // as a new TokenFactor.
    //
    // To distinguish these two cases, do a recursive walk down the uses.
    switch (WalkChainUsers(User, ChainedNodesInPattern, InteriorChainedNodes)) {
    case CR_Simple:
      // If the uses of the TokenFactor are just already-selected nodes, ignore
      // it, it is "below" our pattern.
      continue;
    case CR_InducesCycle:
      // If the uses of the TokenFactor lead to nodes that are not part of our
      // pattern that are not selected, folding would turn this into a cycle,
      // bail out now.
      return CR_InducesCycle;
    case CR_LeadsToInteriorNode:
      break;  // Otherwise, keep processing.
    }
    
    // Okay, we know we're in the interesting interior case.  The TokenFactor
    // is now going to be considered part of the pattern so that we rewrite its
    // uses (it may have uses that are not part of the pattern) with the
    // ultimate chain result of the generated code.  We will also add its chain
    // inputs as inputs to the ultimate TokenFactor we create.
    Result = CR_LeadsToInteriorNode;
    ChainedNodesInPattern.push_back(User);
    InteriorChainedNodes.push_back(User);
    continue;
  }
  
  return Result;
}

/// HandleMergeInputChains - This implements the OPC_EmitMergeInputChains
/// operation for when the pattern matched at least one node with a chains.  The
/// input vector contains a list of all of the chained nodes that we match.  We
/// must determine if this is a valid thing to cover (i.e. matching it won't
/// induce cycles in the DAG) and if so, creating a TokenFactor node. that will
/// be used as the input node chain for the generated nodes.
static SDValue
HandleMergeInputChains(SmallVectorImpl<SDNode*> &ChainNodesMatched,
                       SelectionDAG *CurDAG) {
  // Walk all of the chained nodes we've matched, recursively scanning down the
  // users of the chain result. This adds any TokenFactor nodes that are caught
  // in between chained nodes to the chained and interior nodes list.
  SmallVector<SDNode*, 3> InteriorChainedNodes;
  for (unsigned i = 0, e = ChainNodesMatched.size(); i != e; ++i) {
    if (WalkChainUsers(ChainNodesMatched[i], ChainNodesMatched,
                       InteriorChainedNodes) == CR_InducesCycle)
      return SDValue(); // Would induce a cycle.
  }
  
  // Okay, we have walked all the matched nodes and collected TokenFactor nodes
  // that we are interested in.  Form our input TokenFactor node.
  SmallVector<SDValue, 3> InputChains;
  for (unsigned i = 0, e = ChainNodesMatched.size(); i != e; ++i) {
    // Add the input chain of this node to the InputChains list (which will be
    // the operands of the generated TokenFactor) if it's not an interior node.
    SDNode *N = ChainNodesMatched[i];
    if (N->getOpcode() != ISD::TokenFactor) {
      if (std::count(InteriorChainedNodes.begin(),InteriorChainedNodes.end(),N))
        continue;
      
      // Otherwise, add the input chain.
      SDValue InChain = ChainNodesMatched[i]->getOperand(0);
      assert(InChain.getValueType() == MVT::Other && "Not a chain");
      InputChains.push_back(InChain);
      continue;
    }
    
    // If we have a token factor, we want to add all inputs of the token factor
    // that are not part of the pattern we're matching.
    for (unsigned op = 0, e = N->getNumOperands(); op != e; ++op) {
      if (!std::count(ChainNodesMatched.begin(), ChainNodesMatched.end(),
                      N->getOperand(op).getNode()))
        InputChains.push_back(N->getOperand(op));
    }
  }
  
  SDValue Res;
  if (InputChains.size() == 1)
    return InputChains[0];
  return CurDAG->getNode(ISD::TokenFactor, ChainNodesMatched[0]->getDebugLoc(),
                         MVT::Other, &InputChains[0], InputChains.size());
}  

/// MorphNode - Handle morphing a node in place for the selector.
SDNode *SelectionDAGISel::
MorphNode(SDNode *Node, unsigned TargetOpc, SDVTList VTList,
          const SDValue *Ops, unsigned NumOps, unsigned EmitNodeInfo) {
  // It is possible we're using MorphNodeTo to replace a node with no
  // normal results with one that has a normal result (or we could be
  // adding a chain) and the input could have flags and chains as well.
  // In this case we need to shift the operands down.
  // FIXME: This is a horrible hack and broken in obscure cases, no worse
  // than the old isel though.
  int OldFlagResultNo = -1, OldChainResultNo = -1;

  unsigned NTMNumResults = Node->getNumValues();
  if (Node->getValueType(NTMNumResults-1) == MVT::Flag) {
    OldFlagResultNo = NTMNumResults-1;
    if (NTMNumResults != 1 &&
        Node->getValueType(NTMNumResults-2) == MVT::Other)
      OldChainResultNo = NTMNumResults-2;
  } else if (Node->getValueType(NTMNumResults-1) == MVT::Other)
    OldChainResultNo = NTMNumResults-1;

  // Call the underlying SelectionDAG routine to do the transmogrification. Note
  // that this deletes operands of the old node that become dead.
  SDNode *Res = CurDAG->MorphNodeTo(Node, ~TargetOpc, VTList, Ops, NumOps);

  // MorphNodeTo can operate in two ways: if an existing node with the
  // specified operands exists, it can just return it.  Otherwise, it
  // updates the node in place to have the requested operands.
  if (Res == Node) {
    // If we updated the node in place, reset the node ID.  To the isel,
    // this should be just like a newly allocated machine node.
    Res->setNodeId(-1);
  }

  unsigned ResNumResults = Res->getNumValues();
  // Move the flag if needed.
  if ((EmitNodeInfo & OPFL_FlagOutput) && OldFlagResultNo != -1 &&
      (unsigned)OldFlagResultNo != ResNumResults-1)
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(Node, OldFlagResultNo), 
                                      SDValue(Res, ResNumResults-1));

  if ((EmitNodeInfo & OPFL_FlagOutput) != 0)
  --ResNumResults;

  // Move the chain reference if needed.
  if ((EmitNodeInfo & OPFL_Chain) && OldChainResultNo != -1 &&
      (unsigned)OldChainResultNo != ResNumResults-1)
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(Node, OldChainResultNo), 
                                      SDValue(Res, ResNumResults-1));

  // Otherwise, no replacement happened because the node already exists. Replace
  // Uses of the old node with the new one.
  if (Res != Node)
    CurDAG->ReplaceAllUsesWith(Node, Res);
  
  return Res;
}

/// CheckPatternPredicate - Implements OP_CheckPatternPredicate.
ALWAYS_INLINE static bool
CheckSame(const unsigned char *MatcherTable, unsigned &MatcherIndex,
          SDValue N, const SmallVectorImpl<SDValue> &RecordedNodes) {
  // Accept if it is exactly the same as a previously recorded node.
  unsigned RecNo = MatcherTable[MatcherIndex++];
  assert(RecNo < RecordedNodes.size() && "Invalid CheckSame");
  return N == RecordedNodes[RecNo];
}
  
/// CheckPatternPredicate - Implements OP_CheckPatternPredicate.
ALWAYS_INLINE static bool
CheckPatternPredicate(const unsigned char *MatcherTable, unsigned &MatcherIndex,
                      SelectionDAGISel &SDISel) {
  return SDISel.CheckPatternPredicate(MatcherTable[MatcherIndex++]);
}

/// CheckNodePredicate - Implements OP_CheckNodePredicate.
ALWAYS_INLINE static bool
CheckNodePredicate(const unsigned char *MatcherTable, unsigned &MatcherIndex,
                   SelectionDAGISel &SDISel, SDNode *N) {
  return SDISel.CheckNodePredicate(N, MatcherTable[MatcherIndex++]);
}

ALWAYS_INLINE static bool
CheckOpcode(const unsigned char *MatcherTable, unsigned &MatcherIndex,
            SDNode *N) {
  uint16_t Opc = MatcherTable[MatcherIndex++];
  Opc |= (unsigned short)MatcherTable[MatcherIndex++] << 8;
  return N->getOpcode() == Opc;
}

ALWAYS_INLINE static bool
CheckType(const unsigned char *MatcherTable, unsigned &MatcherIndex,
          SDValue N, const TargetLowering &TLI) {
  MVT::SimpleValueType VT = (MVT::SimpleValueType)MatcherTable[MatcherIndex++];
  if (N.getValueType() == VT) return true;
  
  // Handle the case when VT is iPTR.
  return VT == MVT::iPTR && N.getValueType() == TLI.getPointerTy();
}

ALWAYS_INLINE static bool
CheckChildType(const unsigned char *MatcherTable, unsigned &MatcherIndex,
               SDValue N, const TargetLowering &TLI,
               unsigned ChildNo) {
  if (ChildNo >= N.getNumOperands())
    return false;  // Match fails if out of range child #.
  return ::CheckType(MatcherTable, MatcherIndex, N.getOperand(ChildNo), TLI);
}


ALWAYS_INLINE static bool
CheckCondCode(const unsigned char *MatcherTable, unsigned &MatcherIndex,
              SDValue N) {
  return cast<CondCodeSDNode>(N)->get() ==
      (ISD::CondCode)MatcherTable[MatcherIndex++];
}

ALWAYS_INLINE static bool
CheckValueType(const unsigned char *MatcherTable, unsigned &MatcherIndex,
               SDValue N, const TargetLowering &TLI) {
  MVT::SimpleValueType VT = (MVT::SimpleValueType)MatcherTable[MatcherIndex++];
  if (cast<VTSDNode>(N)->getVT() == VT)
    return true;
  
  // Handle the case when VT is iPTR.
  return VT == MVT::iPTR && cast<VTSDNode>(N)->getVT() == TLI.getPointerTy();
}

ALWAYS_INLINE static bool
CheckInteger(const unsigned char *MatcherTable, unsigned &MatcherIndex,
             SDValue N) {
  int64_t Val = MatcherTable[MatcherIndex++];
  if (Val & 128)
    Val = GetVBR(Val, MatcherTable, MatcherIndex);
  
  ConstantSDNode *C = dyn_cast<ConstantSDNode>(N);
  return C != 0 && C->getSExtValue() == Val;
}

ALWAYS_INLINE static bool
CheckAndImm(const unsigned char *MatcherTable, unsigned &MatcherIndex,
            SDValue N, SelectionDAGISel &SDISel) {
  int64_t Val = MatcherTable[MatcherIndex++];
  if (Val & 128)
    Val = GetVBR(Val, MatcherTable, MatcherIndex);
  
  if (N->getOpcode() != ISD::AND) return false;
  
  ConstantSDNode *C = dyn_cast<ConstantSDNode>(N->getOperand(1));
  return C != 0 && SDISel.CheckAndMask(N.getOperand(0), C, Val);
}

ALWAYS_INLINE static bool
CheckOrImm(const unsigned char *MatcherTable, unsigned &MatcherIndex,
           SDValue N, SelectionDAGISel &SDISel) {
  int64_t Val = MatcherTable[MatcherIndex++];
  if (Val & 128)
    Val = GetVBR(Val, MatcherTable, MatcherIndex);
  
  if (N->getOpcode() != ISD::OR) return false;
  
  ConstantSDNode *C = dyn_cast<ConstantSDNode>(N->getOperand(1));
  return C != 0 && SDISel.CheckOrMask(N.getOperand(0), C, Val);
}

/// IsPredicateKnownToFail - If we know how and can do so without pushing a
/// scope, evaluate the current node.  If the current predicate is known to
/// fail, set Result=true and return anything.  If the current predicate is
/// known to pass, set Result=false and return the MatcherIndex to continue
/// with.  If the current predicate is unknown, set Result=false and return the
/// MatcherIndex to continue with. 
static unsigned IsPredicateKnownToFail(const unsigned char *Table,
                                       unsigned Index, SDValue N,
                                       bool &Result, SelectionDAGISel &SDISel,
                                       SmallVectorImpl<SDValue> &RecordedNodes){
  switch (Table[Index++]) {
  default:
    Result = false;
    return Index-1;  // Could not evaluate this predicate.
  case SelectionDAGISel::OPC_CheckSame:
    Result = !::CheckSame(Table, Index, N, RecordedNodes);
    return Index;
  case SelectionDAGISel::OPC_CheckPatternPredicate:
    Result = !::CheckPatternPredicate(Table, Index, SDISel);
    return Index;
  case SelectionDAGISel::OPC_CheckPredicate:
    Result = !::CheckNodePredicate(Table, Index, SDISel, N.getNode());
    return Index;
  case SelectionDAGISel::OPC_CheckOpcode:
    Result = !::CheckOpcode(Table, Index, N.getNode());
    return Index;
  case SelectionDAGISel::OPC_CheckType:
    Result = !::CheckType(Table, Index, N, SDISel.TLI);
    return Index;
  case SelectionDAGISel::OPC_CheckChild0Type:
  case SelectionDAGISel::OPC_CheckChild1Type:
  case SelectionDAGISel::OPC_CheckChild2Type:
  case SelectionDAGISel::OPC_CheckChild3Type:
  case SelectionDAGISel::OPC_CheckChild4Type:
  case SelectionDAGISel::OPC_CheckChild5Type:
  case SelectionDAGISel::OPC_CheckChild6Type:
  case SelectionDAGISel::OPC_CheckChild7Type:
    Result = !::CheckChildType(Table, Index, N, SDISel.TLI,
                        Table[Index-1] - SelectionDAGISel::OPC_CheckChild0Type);
    return Index;
  case SelectionDAGISel::OPC_CheckCondCode:
    Result = !::CheckCondCode(Table, Index, N);
    return Index;
  case SelectionDAGISel::OPC_CheckValueType:
    Result = !::CheckValueType(Table, Index, N, SDISel.TLI);
    return Index;
  case SelectionDAGISel::OPC_CheckInteger:
    Result = !::CheckInteger(Table, Index, N);
    return Index;
  case SelectionDAGISel::OPC_CheckAndImm:
    Result = !::CheckAndImm(Table, Index, N, SDISel);
    return Index;
  case SelectionDAGISel::OPC_CheckOrImm:
    Result = !::CheckOrImm(Table, Index, N, SDISel);
    return Index;
  }
}


struct MatchScope {
  /// FailIndex - If this match fails, this is the index to continue with.
  unsigned FailIndex;
  
  /// NodeStack - The node stack when the scope was formed.
  SmallVector<SDValue, 4> NodeStack;
  
  /// NumRecordedNodes - The number of recorded nodes when the scope was formed.
  unsigned NumRecordedNodes;
  
  /// NumMatchedMemRefs - The number of matched memref entries.
  unsigned NumMatchedMemRefs;
  
  /// InputChain/InputFlag - The current chain/flag 
  SDValue InputChain, InputFlag;

  /// HasChainNodesMatched - True if the ChainNodesMatched list is non-empty.
  bool HasChainNodesMatched, HasFlagResultNodesMatched;
};

SDNode *SelectionDAGISel::
SelectCodeCommon(SDNode *NodeToMatch, const unsigned char *MatcherTable,
                 unsigned TableSize) {
  // FIXME: Should these even be selected?  Handle these cases in the caller?
  switch (NodeToMatch->getOpcode()) {
  default:
    break;
  case ISD::EntryToken:       // These nodes remain the same.
  case ISD::BasicBlock:
  case ISD::Register:
  //case ISD::VALUETYPE:
  //case ISD::CONDCODE:
  case ISD::HANDLENODE:
  case ISD::MDNODE_SDNODE:
  case ISD::TargetConstant:
  case ISD::TargetConstantFP:
  case ISD::TargetConstantPool:
  case ISD::TargetFrameIndex:
  case ISD::TargetExternalSymbol:
  case ISD::TargetBlockAddress:
  case ISD::TargetJumpTable:
  case ISD::TargetGlobalTLSAddress:
  case ISD::TargetGlobalAddress:
  case ISD::TokenFactor:
  case ISD::CopyFromReg:
  case ISD::CopyToReg:
  case ISD::EH_LABEL:
    NodeToMatch->setNodeId(-1); // Mark selected.
    return 0;
  case ISD::AssertSext:
  case ISD::AssertZext:
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(NodeToMatch, 0),
                                      NodeToMatch->getOperand(0));
    return 0;
  case ISD::INLINEASM: return Select_INLINEASM(NodeToMatch);
  case ISD::UNDEF:     return Select_UNDEF(NodeToMatch);
  }
  
  assert(!NodeToMatch->isMachineOpcode() && "Node already selected!");

  // Set up the node stack with NodeToMatch as the only node on the stack.
  SmallVector<SDValue, 8> NodeStack;
  SDValue N = SDValue(NodeToMatch, 0);
  NodeStack.push_back(N);

  // MatchScopes - Scopes used when matching, if a match failure happens, this
  // indicates where to continue checking.
  SmallVector<MatchScope, 8> MatchScopes;
  
  // RecordedNodes - This is the set of nodes that have been recorded by the
  // state machine.
  SmallVector<SDValue, 8> RecordedNodes;
  
  // MatchedMemRefs - This is the set of MemRef's we've seen in the input
  // pattern.
  SmallVector<MachineMemOperand*, 2> MatchedMemRefs;
  
  // These are the current input chain and flag for use when generating nodes.
  // Various Emit operations change these.  For example, emitting a copytoreg
  // uses and updates these.
  SDValue InputChain, InputFlag;
  
  // ChainNodesMatched - If a pattern matches nodes that have input/output
  // chains, the OPC_EmitMergeInputChains operation is emitted which indicates
  // which ones they are.  The result is captured into this list so that we can
  // update the chain results when the pattern is complete.
  SmallVector<SDNode*, 3> ChainNodesMatched;
  SmallVector<SDNode*, 3> FlagResultNodesMatched;
  
  DEBUG(errs() << "ISEL: Starting pattern match on root node: ";
        NodeToMatch->dump(CurDAG);
        errs() << '\n');
  
  // Determine where to start the interpreter.  Normally we start at opcode #0,
  // but if the state machine starts with an OPC_SwitchOpcode, then we
  // accelerate the first lookup (which is guaranteed to be hot) with the
  // OpcodeOffset table.
  unsigned MatcherIndex = 0;
  
  if (!OpcodeOffset.empty()) {
    // Already computed the OpcodeOffset table, just index into it.
    if (N.getOpcode() < OpcodeOffset.size())
      MatcherIndex = OpcodeOffset[N.getOpcode()];
    DEBUG(errs() << "  Initial Opcode index to " << MatcherIndex << "\n");

  } else if (MatcherTable[0] == OPC_SwitchOpcode) {
    // Otherwise, the table isn't computed, but the state machine does start
    // with an OPC_SwitchOpcode instruction.  Populate the table now, since this
    // is the first time we're selecting an instruction.
    unsigned Idx = 1;
    while (1) {
      // Get the size of this case.
      unsigned CaseSize = MatcherTable[Idx++];
      if (CaseSize & 128)
        CaseSize = GetVBR(CaseSize, MatcherTable, Idx);
      if (CaseSize == 0) break;

      // Get the opcode, add the index to the table.
      uint16_t Opc = MatcherTable[Idx++];
      Opc |= (unsigned short)MatcherTable[Idx++] << 8;
      if (Opc >= OpcodeOffset.size())
        OpcodeOffset.resize((Opc+1)*2);
      OpcodeOffset[Opc] = Idx;
      Idx += CaseSize;
    }

    // Okay, do the lookup for the first opcode.
    if (N.getOpcode() < OpcodeOffset.size())
      MatcherIndex = OpcodeOffset[N.getOpcode()];
  }
  
  while (1) {
    assert(MatcherIndex < TableSize && "Invalid index");
#ifndef NDEBUG
    unsigned CurrentOpcodeIndex = MatcherIndex;
#endif
    BuiltinOpcodes Opcode = (BuiltinOpcodes)MatcherTable[MatcherIndex++];
    switch (Opcode) {
    case OPC_Scope: {
      // Okay, the semantics of this operation are that we should push a scope
      // then evaluate the first child.  However, pushing a scope only to have
      // the first check fail (which then pops it) is inefficient.  If we can
      // determine immediately that the first check (or first several) will
      // immediately fail, don't even bother pushing a scope for them.
      unsigned FailIndex;
      
      while (1) {
        unsigned NumToSkip = MatcherTable[MatcherIndex++];
        if (NumToSkip & 128)
          NumToSkip = GetVBR(NumToSkip, MatcherTable, MatcherIndex);
        // Found the end of the scope with no match.
        if (NumToSkip == 0) {
          FailIndex = 0;
          break;
        }
        
        FailIndex = MatcherIndex+NumToSkip;
        
        unsigned MatcherIndexOfPredicate = MatcherIndex;
        (void)MatcherIndexOfPredicate; // silence warning.
        
        // If we can't evaluate this predicate without pushing a scope (e.g. if
        // it is a 'MoveParent') or if the predicate succeeds on this node, we
        // push the scope and evaluate the full predicate chain.
        bool Result;
        MatcherIndex = IsPredicateKnownToFail(MatcherTable, MatcherIndex, N,
                                              Result, *this, RecordedNodes);
        if (!Result)
          break;
        
        DEBUG(errs() << "  Skipped scope entry (due to false predicate) at "
                     << "index " << MatcherIndexOfPredicate
                     << ", continuing at " << FailIndex << "\n");
        ++NumDAGIselRetries;
        
        // Otherwise, we know that this case of the Scope is guaranteed to fail,
        // move to the next case.
        MatcherIndex = FailIndex;
      }
      
      // If the whole scope failed to match, bail.
      if (FailIndex == 0) break;
      
      // Push a MatchScope which indicates where to go if the first child fails
      // to match.
      MatchScope NewEntry;
      NewEntry.FailIndex = FailIndex;
      NewEntry.NodeStack.append(NodeStack.begin(), NodeStack.end());
      NewEntry.NumRecordedNodes = RecordedNodes.size();
      NewEntry.NumMatchedMemRefs = MatchedMemRefs.size();
      NewEntry.InputChain = InputChain;
      NewEntry.InputFlag = InputFlag;
      NewEntry.HasChainNodesMatched = !ChainNodesMatched.empty();
      NewEntry.HasFlagResultNodesMatched = !FlagResultNodesMatched.empty();
      MatchScopes.push_back(NewEntry);
      continue;
    }
    case OPC_RecordNode:
      // Remember this node, it may end up being an operand in the pattern.
      RecordedNodes.push_back(N);
      continue;
        
    case OPC_RecordChild0: case OPC_RecordChild1:
    case OPC_RecordChild2: case OPC_RecordChild3:
    case OPC_RecordChild4: case OPC_RecordChild5:
    case OPC_RecordChild6: case OPC_RecordChild7: {
      unsigned ChildNo = Opcode-OPC_RecordChild0;
      if (ChildNo >= N.getNumOperands())
        break;  // Match fails if out of range child #.

      RecordedNodes.push_back(N->getOperand(ChildNo));
      continue;
    }
    case OPC_RecordMemRef:
      MatchedMemRefs.push_back(cast<MemSDNode>(N)->getMemOperand());
      continue;
        
    case OPC_CaptureFlagInput:
      // If the current node has an input flag, capture it in InputFlag.
      if (N->getNumOperands() != 0 &&
          N->getOperand(N->getNumOperands()-1).getValueType() == MVT::Flag)
        InputFlag = N->getOperand(N->getNumOperands()-1);
      continue;
        
    case OPC_MoveChild: {
      unsigned ChildNo = MatcherTable[MatcherIndex++];
      if (ChildNo >= N.getNumOperands())
        break;  // Match fails if out of range child #.
      N = N.getOperand(ChildNo);
      NodeStack.push_back(N);
      continue;
    }
        
    case OPC_MoveParent:
      // Pop the current node off the NodeStack.
      NodeStack.pop_back();
      assert(!NodeStack.empty() && "Node stack imbalance!");
      N = NodeStack.back();  
      continue;
     
    case OPC_CheckSame:
      if (!::CheckSame(MatcherTable, MatcherIndex, N, RecordedNodes)) break;
      continue;
    case OPC_CheckPatternPredicate:
      if (!::CheckPatternPredicate(MatcherTable, MatcherIndex, *this)) break;
      continue;
    case OPC_CheckPredicate:
      if (!::CheckNodePredicate(MatcherTable, MatcherIndex, *this,
                                N.getNode()))
        break;
      continue;
    case OPC_CheckComplexPat: {
      unsigned CPNum = MatcherTable[MatcherIndex++];
      unsigned RecNo = MatcherTable[MatcherIndex++];
      assert(RecNo < RecordedNodes.size() && "Invalid CheckComplexPat");
      if (!CheckComplexPattern(NodeToMatch, RecordedNodes[RecNo], CPNum,
                               RecordedNodes))
        break;
      continue;
    }
    case OPC_CheckOpcode:
      if (!::CheckOpcode(MatcherTable, MatcherIndex, N.getNode())) break;
      continue;
        
    case OPC_CheckType:
      if (!::CheckType(MatcherTable, MatcherIndex, N, TLI)) break;
      continue;
        
    case OPC_SwitchOpcode: {
      unsigned CurNodeOpcode = N.getOpcode();
      unsigned SwitchStart = MatcherIndex-1; (void)SwitchStart;
      unsigned CaseSize;
      while (1) {
        // Get the size of this case.
        CaseSize = MatcherTable[MatcherIndex++];
        if (CaseSize & 128)
          CaseSize = GetVBR(CaseSize, MatcherTable, MatcherIndex);
        if (CaseSize == 0) break;

        uint16_t Opc = MatcherTable[MatcherIndex++];
        Opc |= (unsigned short)MatcherTable[MatcherIndex++] << 8;

        // If the opcode matches, then we will execute this case.
        if (CurNodeOpcode == Opc)
          break;
      
        // Otherwise, skip over this case.
        MatcherIndex += CaseSize;
      }
      
      // If no cases matched, bail out.
      if (CaseSize == 0) break;
      
      // Otherwise, execute the case we found.
      DEBUG(errs() << "  OpcodeSwitch from " << SwitchStart
                   << " to " << MatcherIndex << "\n");
      continue;
    }
        
    case OPC_SwitchType: {
      MVT::SimpleValueType CurNodeVT = N.getValueType().getSimpleVT().SimpleTy;
      unsigned SwitchStart = MatcherIndex-1; (void)SwitchStart;
      unsigned CaseSize;
      while (1) {
        // Get the size of this case.
        CaseSize = MatcherTable[MatcherIndex++];
        if (CaseSize & 128)
          CaseSize = GetVBR(CaseSize, MatcherTable, MatcherIndex);
        if (CaseSize == 0) break;
        
        MVT::SimpleValueType CaseVT =
          (MVT::SimpleValueType)MatcherTable[MatcherIndex++];
        if (CaseVT == MVT::iPTR)
          CaseVT = TLI.getPointerTy().SimpleTy;
        
        // If the VT matches, then we will execute this case.
        if (CurNodeVT == CaseVT)
          break;
        
        // Otherwise, skip over this case.
        MatcherIndex += CaseSize;
      }
      
      // If no cases matched, bail out.
      if (CaseSize == 0) break;
      
      // Otherwise, execute the case we found.
      DEBUG(errs() << "  TypeSwitch[" << EVT(CurNodeVT).getEVTString()
                   << "] from " << SwitchStart << " to " << MatcherIndex<<'\n');
      continue;
    }
    case OPC_CheckChild0Type: case OPC_CheckChild1Type:
    case OPC_CheckChild2Type: case OPC_CheckChild3Type:
    case OPC_CheckChild4Type: case OPC_CheckChild5Type:
    case OPC_CheckChild6Type: case OPC_CheckChild7Type:
      if (!::CheckChildType(MatcherTable, MatcherIndex, N, TLI,
                            Opcode-OPC_CheckChild0Type))
        break;
      continue;
    case OPC_CheckCondCode:
      if (!::CheckCondCode(MatcherTable, MatcherIndex, N)) break;
      continue;
    case OPC_CheckValueType:
      if (!::CheckValueType(MatcherTable, MatcherIndex, N, TLI)) break;
      continue;
    case OPC_CheckInteger:
      if (!::CheckInteger(MatcherTable, MatcherIndex, N)) break;
      continue;
    case OPC_CheckAndImm:
      if (!::CheckAndImm(MatcherTable, MatcherIndex, N, *this)) break;
      continue;
    case OPC_CheckOrImm:
      if (!::CheckOrImm(MatcherTable, MatcherIndex, N, *this)) break;
      continue;
        
    case OPC_CheckFoldableChainNode: {
      assert(NodeStack.size() != 1 && "No parent node");
      // Verify that all intermediate nodes between the root and this one have
      // a single use.
      bool HasMultipleUses = false;
      for (unsigned i = 1, e = NodeStack.size()-1; i != e; ++i)
        if (!NodeStack[i].hasOneUse()) {
          HasMultipleUses = true;
          break;
        }
      if (HasMultipleUses) break;

      // Check to see that the target thinks this is profitable to fold and that
      // we can fold it without inducing cycles in the graph.
      if (!IsProfitableToFold(N, NodeStack[NodeStack.size()-2].getNode(),
                              NodeToMatch) ||
          !IsLegalToFold(N, NodeStack[NodeStack.size()-2].getNode(),
                         NodeToMatch, true/*We validate our own chains*/))
        break;
      
      continue;
    }
    case OPC_EmitInteger: {
      MVT::SimpleValueType VT =
        (MVT::SimpleValueType)MatcherTable[MatcherIndex++];
      int64_t Val = MatcherTable[MatcherIndex++];
      if (Val & 128)
        Val = GetVBR(Val, MatcherTable, MatcherIndex);
      RecordedNodes.push_back(CurDAG->getTargetConstant(Val, VT));
      continue;
    }
    case OPC_EmitRegister: {
      MVT::SimpleValueType VT =
        (MVT::SimpleValueType)MatcherTable[MatcherIndex++];
      unsigned RegNo = MatcherTable[MatcherIndex++];
      RecordedNodes.push_back(CurDAG->getRegister(RegNo, VT));
      continue;
    }
        
    case OPC_EmitConvertToTarget:  {
      // Convert from IMM/FPIMM to target version.
      unsigned RecNo = MatcherTable[MatcherIndex++];
      assert(RecNo < RecordedNodes.size() && "Invalid CheckSame");
      SDValue Imm = RecordedNodes[RecNo];

      if (Imm->getOpcode() == ISD::Constant) {
        int64_t Val = cast<ConstantSDNode>(Imm)->getZExtValue();
        Imm = CurDAG->getTargetConstant(Val, Imm.getValueType());
      } else if (Imm->getOpcode() == ISD::ConstantFP) {
        const ConstantFP *Val=cast<ConstantFPSDNode>(Imm)->getConstantFPValue();
        Imm = CurDAG->getTargetConstantFP(*Val, Imm.getValueType());
      }
      
      RecordedNodes.push_back(Imm);
      continue;
    }
        
    case OPC_EmitMergeInputChains1_0:    // OPC_EmitMergeInputChains, 1, 0
    case OPC_EmitMergeInputChains1_1: {  // OPC_EmitMergeInputChains, 1, 1
      // These are space-optimized forms of OPC_EmitMergeInputChains.
      assert(InputChain.getNode() == 0 &&
             "EmitMergeInputChains should be the first chain producing node");
      assert(ChainNodesMatched.empty() &&
             "Should only have one EmitMergeInputChains per match");
      
      // Read all of the chained nodes.
      unsigned RecNo = Opcode == OPC_EmitMergeInputChains1_1;
      assert(RecNo < RecordedNodes.size() && "Invalid CheckSame");
      ChainNodesMatched.push_back(RecordedNodes[RecNo].getNode());
        
      // FIXME: What if other value results of the node have uses not matched
      // by this pattern?
      if (ChainNodesMatched.back() != NodeToMatch &&
          !RecordedNodes[RecNo].hasOneUse()) {
        ChainNodesMatched.clear();
        break;
      }
      
      // Merge the input chains if they are not intra-pattern references.
      InputChain = HandleMergeInputChains(ChainNodesMatched, CurDAG);
      
      if (InputChain.getNode() == 0)
        break;  // Failed to merge.
      continue;
    }
        
    case OPC_EmitMergeInputChains: {
      assert(InputChain.getNode() == 0 &&
             "EmitMergeInputChains should be the first chain producing node");
      // This node gets a list of nodes we matched in the input that have
      // chains.  We want to token factor all of the input chains to these nodes
      // together.  However, if any of the input chains is actually one of the
      // nodes matched in this pattern, then we have an intra-match reference.
      // Ignore these because the newly token factored chain should not refer to
      // the old nodes.
      unsigned NumChains = MatcherTable[MatcherIndex++];
      assert(NumChains != 0 && "Can't TF zero chains");

      assert(ChainNodesMatched.empty() &&
             "Should only have one EmitMergeInputChains per match");

      // Read all of the chained nodes.
      for (unsigned i = 0; i != NumChains; ++i) {
        unsigned RecNo = MatcherTable[MatcherIndex++];
        assert(RecNo < RecordedNodes.size() && "Invalid CheckSame");
        ChainNodesMatched.push_back(RecordedNodes[RecNo].getNode());
        
        // FIXME: What if other value results of the node have uses not matched
        // by this pattern?
        if (ChainNodesMatched.back() != NodeToMatch &&
            !RecordedNodes[RecNo].hasOneUse()) {
          ChainNodesMatched.clear();
          break;
        }
      }
      
      // If the inner loop broke out, the match fails.
      if (ChainNodesMatched.empty())
        break;

      // Merge the input chains if they are not intra-pattern references.
      InputChain = HandleMergeInputChains(ChainNodesMatched, CurDAG);
      
      if (InputChain.getNode() == 0)
        break;  // Failed to merge.

      continue;
    }
        
    case OPC_EmitCopyToReg: {
      unsigned RecNo = MatcherTable[MatcherIndex++];
      assert(RecNo < RecordedNodes.size() && "Invalid CheckSame");
      unsigned DestPhysReg = MatcherTable[MatcherIndex++];
      
      if (InputChain.getNode() == 0)
        InputChain = CurDAG->getEntryNode();
      
      InputChain = CurDAG->getCopyToReg(InputChain, NodeToMatch->getDebugLoc(),
                                        DestPhysReg, RecordedNodes[RecNo],
                                        InputFlag);
      
      InputFlag = InputChain.getValue(1);
      continue;
    }
        
    case OPC_EmitNodeXForm: {
      unsigned XFormNo = MatcherTable[MatcherIndex++];
      unsigned RecNo = MatcherTable[MatcherIndex++];
      assert(RecNo < RecordedNodes.size() && "Invalid CheckSame");
      RecordedNodes.push_back(RunSDNodeXForm(RecordedNodes[RecNo], XFormNo));
      continue;
    }
        
    case OPC_EmitNode:
    case OPC_MorphNodeTo: {
      uint16_t TargetOpc = MatcherTable[MatcherIndex++];
      TargetOpc |= (unsigned short)MatcherTable[MatcherIndex++] << 8;
      unsigned EmitNodeInfo = MatcherTable[MatcherIndex++];
      // Get the result VT list.
      unsigned NumVTs = MatcherTable[MatcherIndex++];
      SmallVector<EVT, 4> VTs;
      for (unsigned i = 0; i != NumVTs; ++i) {
        MVT::SimpleValueType VT =
          (MVT::SimpleValueType)MatcherTable[MatcherIndex++];
        if (VT == MVT::iPTR) VT = TLI.getPointerTy().SimpleTy;
        VTs.push_back(VT);
      }
      
      if (EmitNodeInfo & OPFL_Chain)
        VTs.push_back(MVT::Other);
      if (EmitNodeInfo & OPFL_FlagOutput)
        VTs.push_back(MVT::Flag);
      
      // This is hot code, so optimize the two most common cases of 1 and 2
      // results.
      SDVTList VTList;
      if (VTs.size() == 1)
        VTList = CurDAG->getVTList(VTs[0]);
      else if (VTs.size() == 2)
        VTList = CurDAG->getVTList(VTs[0], VTs[1]);
      else
        VTList = CurDAG->getVTList(VTs.data(), VTs.size());

      // Get the operand list.
      unsigned NumOps = MatcherTable[MatcherIndex++];
      SmallVector<SDValue, 8> Ops;
      for (unsigned i = 0; i != NumOps; ++i) {
        unsigned RecNo = MatcherTable[MatcherIndex++];
        if (RecNo & 128)
          RecNo = GetVBR(RecNo, MatcherTable, MatcherIndex);
        
        assert(RecNo < RecordedNodes.size() && "Invalid EmitNode");
        Ops.push_back(RecordedNodes[RecNo]);
      }
      
      // If there are variadic operands to add, handle them now.
      if (EmitNodeInfo & OPFL_VariadicInfo) {
        // Determine the start index to copy from.
        unsigned FirstOpToCopy = getNumFixedFromVariadicInfo(EmitNodeInfo);
        FirstOpToCopy += (EmitNodeInfo & OPFL_Chain) ? 1 : 0;
        assert(NodeToMatch->getNumOperands() >= FirstOpToCopy &&
               "Invalid variadic node");
        // Copy all of the variadic operands, not including a potential flag
        // input.
        for (unsigned i = FirstOpToCopy, e = NodeToMatch->getNumOperands();
             i != e; ++i) {
          SDValue V = NodeToMatch->getOperand(i);
          if (V.getValueType() == MVT::Flag) break;
          Ops.push_back(V);
        }
      }
      
      // If this has chain/flag inputs, add them.
      if (EmitNodeInfo & OPFL_Chain)
        Ops.push_back(InputChain);
      if ((EmitNodeInfo & OPFL_FlagInput) && InputFlag.getNode() != 0)
        Ops.push_back(InputFlag);
      
      // Create the node.
      SDNode *Res = 0;
      if (Opcode != OPC_MorphNodeTo) {
        // If this is a normal EmitNode command, just create the new node and
        // add the results to the RecordedNodes list.
        Res = CurDAG->getMachineNode(TargetOpc, NodeToMatch->getDebugLoc(),
                                     VTList, Ops.data(), Ops.size());
        
        // Add all the non-flag/non-chain results to the RecordedNodes list.
        for (unsigned i = 0, e = VTs.size(); i != e; ++i) {
          if (VTs[i] == MVT::Other || VTs[i] == MVT::Flag) break;
          RecordedNodes.push_back(SDValue(Res, i));
        }
        
      } else {
        Res = MorphNode(NodeToMatch, TargetOpc, VTList, Ops.data(), Ops.size(),
                        EmitNodeInfo);
      }
      
      // If the node had chain/flag results, update our notion of the current
      // chain and flag.
      if (EmitNodeInfo & OPFL_FlagOutput) {
        InputFlag = SDValue(Res, VTs.size()-1);
        if (EmitNodeInfo & OPFL_Chain)
          InputChain = SDValue(Res, VTs.size()-2);
      } else if (EmitNodeInfo & OPFL_Chain)
        InputChain = SDValue(Res, VTs.size()-1);

      // If the OPFL_MemRefs flag is set on this node, slap all of the
      // accumulated memrefs onto it.
      //
      // FIXME: This is vastly incorrect for patterns with multiple outputs
      // instructions that access memory and for ComplexPatterns that match
      // loads.
      if (EmitNodeInfo & OPFL_MemRefs) {
        MachineSDNode::mmo_iterator MemRefs =
          MF->allocateMemRefsArray(MatchedMemRefs.size());
        std::copy(MatchedMemRefs.begin(), MatchedMemRefs.end(), MemRefs);
        cast<MachineSDNode>(Res)
          ->setMemRefs(MemRefs, MemRefs + MatchedMemRefs.size());
      }
      
      DEBUG(errs() << "  "
                   << (Opcode == OPC_MorphNodeTo ? "Morphed" : "Created")
                   << " node: "; Res->dump(CurDAG); errs() << "\n");
      
      // If this was a MorphNodeTo then we're completely done!
      if (Opcode == OPC_MorphNodeTo) {
        // Update chain and flag uses.
        UpdateChainsAndFlags(NodeToMatch, InputChain, ChainNodesMatched,
                             InputFlag, FlagResultNodesMatched, true);
        return Res;
      }
      
      continue;
    }
        
    case OPC_MarkFlagResults: {
      unsigned NumNodes = MatcherTable[MatcherIndex++];
      
      // Read and remember all the flag-result nodes.
      for (unsigned i = 0; i != NumNodes; ++i) {
        unsigned RecNo = MatcherTable[MatcherIndex++];
        if (RecNo & 128)
          RecNo = GetVBR(RecNo, MatcherTable, MatcherIndex);

        assert(RecNo < RecordedNodes.size() && "Invalid CheckSame");
        FlagResultNodesMatched.push_back(RecordedNodes[RecNo].getNode());
      }
      continue;
    }
      
    case OPC_CompleteMatch: {
      // The match has been completed, and any new nodes (if any) have been
      // created.  Patch up references to the matched dag to use the newly
      // created nodes.
      unsigned NumResults = MatcherTable[MatcherIndex++];

      for (unsigned i = 0; i != NumResults; ++i) {
        unsigned ResSlot = MatcherTable[MatcherIndex++];
        if (ResSlot & 128)
          ResSlot = GetVBR(ResSlot, MatcherTable, MatcherIndex);
        
        assert(ResSlot < RecordedNodes.size() && "Invalid CheckSame");
        SDValue Res = RecordedNodes[ResSlot];
        
        assert(i < NodeToMatch->getNumValues() &&
               NodeToMatch->getValueType(i) != MVT::Other &&
               NodeToMatch->getValueType(i) != MVT::Flag &&
               "Invalid number of results to complete!");
        assert((NodeToMatch->getValueType(i) == Res.getValueType() ||
                NodeToMatch->getValueType(i) == MVT::iPTR ||
                Res.getValueType() == MVT::iPTR ||
                NodeToMatch->getValueType(i).getSizeInBits() ==
                    Res.getValueType().getSizeInBits()) &&
               "invalid replacement");
        CurDAG->ReplaceAllUsesOfValueWith(SDValue(NodeToMatch, i), Res);
      }

      // If the root node defines a flag, add it to the flag nodes to update
      // list.
      if (NodeToMatch->getValueType(NodeToMatch->getNumValues()-1) == MVT::Flag)
        FlagResultNodesMatched.push_back(NodeToMatch);
      
      // Update chain and flag uses.
      UpdateChainsAndFlags(NodeToMatch, InputChain, ChainNodesMatched,
                           InputFlag, FlagResultNodesMatched, false);
      
      assert(NodeToMatch->use_empty() &&
             "Didn't replace all uses of the node?");
      
      // FIXME: We just return here, which interacts correctly with SelectRoot
      // above.  We should fix this to not return an SDNode* anymore.
      return 0;
    }
    }
    
    // If the code reached this point, then the match failed.  See if there is
    // another child to try in the current 'Scope', otherwise pop it until we
    // find a case to check.
    DEBUG(errs() << "  Match failed at index " << CurrentOpcodeIndex << "\n");
    ++NumDAGIselRetries;
    while (1) {
      if (MatchScopes.empty()) {
        CannotYetSelect(NodeToMatch);
        return 0;
      }

      // Restore the interpreter state back to the point where the scope was
      // formed.
      MatchScope &LastScope = MatchScopes.back();
      RecordedNodes.resize(LastScope.NumRecordedNodes);
      NodeStack.clear();
      NodeStack.append(LastScope.NodeStack.begin(), LastScope.NodeStack.end());
      N = NodeStack.back();

      if (LastScope.NumMatchedMemRefs != MatchedMemRefs.size())
        MatchedMemRefs.resize(LastScope.NumMatchedMemRefs);
      MatcherIndex = LastScope.FailIndex;
      
      DEBUG(errs() << "  Continuing at " << MatcherIndex << "\n");
    
      InputChain = LastScope.InputChain;
      InputFlag = LastScope.InputFlag;
      if (!LastScope.HasChainNodesMatched)
        ChainNodesMatched.clear();
      if (!LastScope.HasFlagResultNodesMatched)
        FlagResultNodesMatched.clear();

      // Check to see what the offset is at the new MatcherIndex.  If it is zero
      // we have reached the end of this scope, otherwise we have another child
      // in the current scope to try.
      unsigned NumToSkip = MatcherTable[MatcherIndex++];
      if (NumToSkip & 128)
        NumToSkip = GetVBR(NumToSkip, MatcherTable, MatcherIndex);

      // If we have another child in this scope to match, update FailIndex and
      // try it.
      if (NumToSkip != 0) {
        LastScope.FailIndex = MatcherIndex+NumToSkip;
        break;
      }
      
      // End of this scope, pop it and try the next child in the containing
      // scope.
      MatchScopes.pop_back();
    }
  }
}
    


void SelectionDAGISel::CannotYetSelect(SDNode *N) {
  std::string msg;
  raw_string_ostream Msg(msg);
  Msg << "Cannot yet select: ";
  
  if (N->getOpcode() != ISD::INTRINSIC_W_CHAIN &&
      N->getOpcode() != ISD::INTRINSIC_WO_CHAIN &&
      N->getOpcode() != ISD::INTRINSIC_VOID) {
    N->printrFull(Msg, CurDAG);
  } else {
    bool HasInputChain = N->getOperand(0).getValueType() == MVT::Other;
    unsigned iid =
      cast<ConstantSDNode>(N->getOperand(HasInputChain))->getZExtValue();
    if (iid < Intrinsic::num_intrinsics)
      Msg << "intrinsic %" << Intrinsic::getName((Intrinsic::ID)iid);
    else if (const TargetIntrinsicInfo *TII = TM.getIntrinsicInfo())
      Msg << "target intrinsic %" << TII->getName(iid);
    else
      Msg << "unknown intrinsic #" << iid;
  }
  report_fatal_error(Msg.str());
}

char SelectionDAGISel::ID = 0;
