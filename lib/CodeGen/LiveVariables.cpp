//===-- LiveVariables.cpp - Live Variable Analysis for Machine Code -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the LiveVariable analysis pass.  For each machine
// instruction in the function, this pass calculates the set of registers that
// are immediately dead after the instruction (i.e., the instruction calculates
// the value, but it is never used) and the set of registers that are used by
// the instruction, but are never used after the instruction (i.e., they are
// killed).
//
// This class computes live variables using are sparse implementation based on
// the machine code SSA form.  This class computes live variable information for
// each virtual and _register allocatable_ physical register in a function.  It
// uses the dominance properties of SSA form to efficiently compute live
// variables for virtual registers, and assumes that physical registers are only
// live within a single basic block (allowing it to do a single local analysis
// to resolve physical register lifetimes in each basic block).  If a physical
// register is not register allocatable, it is not tracked.  This is useful for
// things like the stack pointer and condition codes.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/STLExtras.h"
#include <algorithm>
using namespace llvm;

char LiveVariables::ID = 0;
static RegisterPass<LiveVariables> X("livevars", "Live Variable Analysis");


void LiveVariables::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequiredID(UnreachableMachineBlockElimID);
  AU.setPreservesAll();
  MachineFunctionPass::getAnalysisUsage(AU);
}

MachineInstr *
LiveVariables::VarInfo::findKill(const MachineBasicBlock *MBB) const {
  for (unsigned i = 0, e = Kills.size(); i != e; ++i)
    if (Kills[i]->getParent() == MBB)
      return Kills[i];
  return NULL;
}

void LiveVariables::VarInfo::dump() const {
  dbgs() << "  Alive in blocks: ";
  for (SparseBitVector<>::iterator I = AliveBlocks.begin(),
           E = AliveBlocks.end(); I != E; ++I)
    dbgs() << *I << ", ";
  dbgs() << "\n  Killed by:";
  if (Kills.empty())
    dbgs() << " No instructions.\n";
  else {
    for (unsigned i = 0, e = Kills.size(); i != e; ++i)
      dbgs() << "\n    #" << i << ": " << *Kills[i];
    dbgs() << "\n";
  }
}

/// getVarInfo - Get (possibly creating) a VarInfo object for the given vreg.
LiveVariables::VarInfo &LiveVariables::getVarInfo(unsigned RegIdx) {
  assert(TargetRegisterInfo::isVirtualRegister(RegIdx) &&
         "getVarInfo: not a virtual register!");
  RegIdx -= TargetRegisterInfo::FirstVirtualRegister;
  if (RegIdx >= VirtRegInfo.size()) {
    if (RegIdx >= 2*VirtRegInfo.size())
      VirtRegInfo.resize(RegIdx*2);
    else
      VirtRegInfo.resize(2*VirtRegInfo.size());
  }
  return VirtRegInfo[RegIdx];
}

void LiveVariables::MarkVirtRegAliveInBlock(VarInfo& VRInfo,
                                            MachineBasicBlock *DefBlock,
                                            MachineBasicBlock *MBB,
                                    std::vector<MachineBasicBlock*> &WorkList) {
  unsigned BBNum = MBB->getNumber();
  
  // Check to see if this basic block is one of the killing blocks.  If so,
  // remove it.
  for (unsigned i = 0, e = VRInfo.Kills.size(); i != e; ++i)
    if (VRInfo.Kills[i]->getParent() == MBB) {
      VRInfo.Kills.erase(VRInfo.Kills.begin()+i);  // Erase entry
      break;
    }
  
  if (MBB == DefBlock) return;  // Terminate recursion

  if (VRInfo.AliveBlocks.test(BBNum))
    return;  // We already know the block is live

  // Mark the variable known alive in this bb
  VRInfo.AliveBlocks.set(BBNum);

  for (MachineBasicBlock::const_pred_reverse_iterator PI = MBB->pred_rbegin(),
         E = MBB->pred_rend(); PI != E; ++PI)
    WorkList.push_back(*PI);
}

void LiveVariables::MarkVirtRegAliveInBlock(VarInfo &VRInfo,
                                            MachineBasicBlock *DefBlock,
                                            MachineBasicBlock *MBB) {
  std::vector<MachineBasicBlock*> WorkList;
  MarkVirtRegAliveInBlock(VRInfo, DefBlock, MBB, WorkList);

  while (!WorkList.empty()) {
    MachineBasicBlock *Pred = WorkList.back();
    WorkList.pop_back();
    MarkVirtRegAliveInBlock(VRInfo, DefBlock, Pred, WorkList);
  }
}

void LiveVariables::HandleVirtRegUse(unsigned reg, MachineBasicBlock *MBB,
                                     MachineInstr *MI) {
  assert(MRI->getVRegDef(reg) && "Register use before def!");

  unsigned BBNum = MBB->getNumber();

  VarInfo& VRInfo = getVarInfo(reg);
  VRInfo.NumUses++;

  // Check to see if this basic block is already a kill block.
  if (!VRInfo.Kills.empty() && VRInfo.Kills.back()->getParent() == MBB) {
    // Yes, this register is killed in this basic block already. Increase the
    // live range by updating the kill instruction.
    VRInfo.Kills.back() = MI;
    return;
  }

#ifndef NDEBUG
  for (unsigned i = 0, e = VRInfo.Kills.size(); i != e; ++i)
    assert(VRInfo.Kills[i]->getParent() != MBB && "entry should be at end!");
#endif

  // This situation can occur:
  //
  //     ,------.
  //     |      |
  //     |      v
  //     |   t2 = phi ... t1 ...
  //     |      |
  //     |      v
  //     |   t1 = ...
  //     |  ... = ... t1 ...
  //     |      |
  //     `------'
  //
  // where there is a use in a PHI node that's a predecessor to the defining
  // block. We don't want to mark all predecessors as having the value "alive"
  // in this case.
  if (MBB == MRI->getVRegDef(reg)->getParent()) return;

  // Add a new kill entry for this basic block. If this virtual register is
  // already marked as alive in this basic block, that means it is alive in at
  // least one of the successor blocks, it's not a kill.
  if (!VRInfo.AliveBlocks.test(BBNum))
    VRInfo.Kills.push_back(MI);

  // Update all dominating blocks to mark them as "known live".
  for (MachineBasicBlock::const_pred_iterator PI = MBB->pred_begin(),
         E = MBB->pred_end(); PI != E; ++PI)
    MarkVirtRegAliveInBlock(VRInfo, MRI->getVRegDef(reg)->getParent(), *PI);
}

void LiveVariables::HandleVirtRegDef(unsigned Reg, MachineInstr *MI) {
  VarInfo &VRInfo = getVarInfo(Reg);

  if (VRInfo.AliveBlocks.empty())
    // If vr is not alive in any block, then defaults to dead.
    VRInfo.Kills.push_back(MI);
}

/// FindLastPartialDef - Return the last partial def of the specified register.
/// Also returns the sub-registers that're defined by the instruction.
MachineInstr *LiveVariables::FindLastPartialDef(unsigned Reg,
                                            SmallSet<unsigned,4> &PartDefRegs) {
  unsigned LastDefReg = 0;
  unsigned LastDefDist = 0;
  MachineInstr *LastDef = NULL;
  for (const unsigned *SubRegs = TRI->getSubRegisters(Reg);
       unsigned SubReg = *SubRegs; ++SubRegs) {
    MachineInstr *Def = PhysRegDef[SubReg];
    if (!Def)
      continue;
    unsigned Dist = DistanceMap[Def];
    if (Dist > LastDefDist) {
      LastDefReg  = SubReg;
      LastDef     = Def;
      LastDefDist = Dist;
    }
  }

  if (!LastDef)
    return 0;

  PartDefRegs.insert(LastDefReg);
  for (unsigned i = 0, e = LastDef->getNumOperands(); i != e; ++i) {
    MachineOperand &MO = LastDef->getOperand(i);
    if (!MO.isReg() || !MO.isDef() || MO.getReg() == 0)
      continue;
    unsigned DefReg = MO.getReg();
    if (TRI->isSubRegister(Reg, DefReg)) {
      PartDefRegs.insert(DefReg);
      for (const unsigned *SubRegs = TRI->getSubRegisters(DefReg);
           unsigned SubReg = *SubRegs; ++SubRegs)
        PartDefRegs.insert(SubReg);
    }
  }
  return LastDef;
}

/// HandlePhysRegUse - Turn previous partial def's into read/mod/writes. Add
/// implicit defs to a machine instruction if there was an earlier def of its
/// super-register.
void LiveVariables::HandlePhysRegUse(unsigned Reg, MachineInstr *MI) {
  MachineInstr *LastDef = PhysRegDef[Reg];
  // If there was a previous use or a "full" def all is well.
  if (!LastDef && !PhysRegUse[Reg]) {
    // Otherwise, the last sub-register def implicitly defines this register.
    // e.g.
    // AH =
    // AL = ... <imp-def EAX>, <imp-kill AH>
    //    = AH
    // ...
    //    = EAX
    // All of the sub-registers must have been defined before the use of Reg!
    SmallSet<unsigned, 4> PartDefRegs;
    MachineInstr *LastPartialDef = FindLastPartialDef(Reg, PartDefRegs);
    // If LastPartialDef is NULL, it must be using a livein register.
    if (LastPartialDef) {
      LastPartialDef->addOperand(MachineOperand::CreateReg(Reg, true/*IsDef*/,
                                                           true/*IsImp*/));
      PhysRegDef[Reg] = LastPartialDef;
      SmallSet<unsigned, 8> Processed;
      for (const unsigned *SubRegs = TRI->getSubRegisters(Reg);
           unsigned SubReg = *SubRegs; ++SubRegs) {
        if (Processed.count(SubReg))
          continue;
        if (PartDefRegs.count(SubReg))
          continue;
        // This part of Reg was defined before the last partial def. It's killed
        // here.
        LastPartialDef->addOperand(MachineOperand::CreateReg(SubReg,
                                                             false/*IsDef*/,
                                                             true/*IsImp*/));
        PhysRegDef[SubReg] = LastPartialDef;
        for (const unsigned *SS = TRI->getSubRegisters(SubReg); *SS; ++SS)
          Processed.insert(*SS);
      }
    }
  }
  else if (LastDef && !PhysRegUse[Reg] &&
           !LastDef->findRegisterDefOperand(Reg))
    // Last def defines the super register, add an implicit def of reg.
    LastDef->addOperand(MachineOperand::CreateReg(Reg,
                                                 true/*IsDef*/, true/*IsImp*/));

  // Remember this use.
  PhysRegUse[Reg]  = MI;
  for (const unsigned *SubRegs = TRI->getSubRegisters(Reg);
       unsigned SubReg = *SubRegs; ++SubRegs)
    PhysRegUse[SubReg] =  MI;
}

/// FindLastRefOrPartRef - Return the last reference or partial reference of
/// the specified register.
MachineInstr *LiveVariables::FindLastRefOrPartRef(unsigned Reg) {
  MachineInstr *LastDef = PhysRegDef[Reg];
  MachineInstr *LastUse = PhysRegUse[Reg];
  if (!LastDef && !LastUse)
    return false;

  MachineInstr *LastRefOrPartRef = LastUse ? LastUse : LastDef;
  unsigned LastRefOrPartRefDist = DistanceMap[LastRefOrPartRef];
  unsigned LastPartDefDist = 0;
  for (const unsigned *SubRegs = TRI->getSubRegisters(Reg);
       unsigned SubReg = *SubRegs; ++SubRegs) {
    MachineInstr *Def = PhysRegDef[SubReg];
    if (Def && Def != LastDef) {
      // There was a def of this sub-register in between. This is a partial
      // def, keep track of the last one.
      unsigned Dist = DistanceMap[Def];
      if (Dist > LastPartDefDist)
        LastPartDefDist = Dist;
    } else if (MachineInstr *Use = PhysRegUse[SubReg]) {
      unsigned Dist = DistanceMap[Use];
      if (Dist > LastRefOrPartRefDist) {
        LastRefOrPartRefDist = Dist;
        LastRefOrPartRef = Use;
      }
    }
  }

  return LastRefOrPartRef;
}

bool LiveVariables::HandlePhysRegKill(unsigned Reg, MachineInstr *MI) {
  MachineInstr *LastDef = PhysRegDef[Reg];
  MachineInstr *LastUse = PhysRegUse[Reg];
  if (!LastDef && !LastUse)
    return false;

  MachineInstr *LastRefOrPartRef = LastUse ? LastUse : LastDef;
  unsigned LastRefOrPartRefDist = DistanceMap[LastRefOrPartRef];
  // The whole register is used.
  // AL =
  // AH =
  //
  //    = AX
  //    = AL, AX<imp-use, kill>
  // AX =
  //
  // Or whole register is defined, but not used at all.
  // AX<dead> =
  // ...
  // AX =
  //
  // Or whole register is defined, but only partly used.
  // AX<dead> = AL<imp-def>
  //    = AL<kill>
  // AX = 
  MachineInstr *LastPartDef = 0;
  unsigned LastPartDefDist = 0;
  SmallSet<unsigned, 8> PartUses;
  for (const unsigned *SubRegs = TRI->getSubRegisters(Reg);
       unsigned SubReg = *SubRegs; ++SubRegs) {
    MachineInstr *Def = PhysRegDef[SubReg];
    if (Def && Def != LastDef) {
      // There was a def of this sub-register in between. This is a partial
      // def, keep track of the last one.
      unsigned Dist = DistanceMap[Def];
      if (Dist > LastPartDefDist) {
        LastPartDefDist = Dist;
        LastPartDef = Def;
      }
      continue;
    }
    if (MachineInstr *Use = PhysRegUse[SubReg]) {
      PartUses.insert(SubReg);
      for (const unsigned *SS = TRI->getSubRegisters(SubReg); *SS; ++SS)
        PartUses.insert(*SS);
      unsigned Dist = DistanceMap[Use];
      if (Dist > LastRefOrPartRefDist) {
        LastRefOrPartRefDist = Dist;
        LastRefOrPartRef = Use;
      }
    }
  }

  if (!PhysRegUse[Reg]) {
    // Partial uses. Mark register def dead and add implicit def of
    // sub-registers which are used.
    // EAX<dead>  = op  AL<imp-def>
    // That is, EAX def is dead but AL def extends pass it.
    PhysRegDef[Reg]->addRegisterDead(Reg, TRI, true);
    for (const unsigned *SubRegs = TRI->getSubRegisters(Reg);
         unsigned SubReg = *SubRegs; ++SubRegs) {
      if (!PartUses.count(SubReg))
        continue;
      bool NeedDef = true;
      if (PhysRegDef[Reg] == PhysRegDef[SubReg]) {
        MachineOperand *MO = PhysRegDef[Reg]->findRegisterDefOperand(SubReg);
        if (MO) {
          NeedDef = false;
          assert(!MO->isDead());
        }
      }
      if (NeedDef)
        PhysRegDef[Reg]->addOperand(MachineOperand::CreateReg(SubReg,
                                                 true/*IsDef*/, true/*IsImp*/));
      MachineInstr *LastSubRef = FindLastRefOrPartRef(SubReg);
      if (LastSubRef)
        LastSubRef->addRegisterKilled(SubReg, TRI, true);
      else {
        LastRefOrPartRef->addRegisterKilled(SubReg, TRI, true);
        PhysRegUse[SubReg] = LastRefOrPartRef;
        for (const unsigned *SSRegs = TRI->getSubRegisters(SubReg);
             unsigned SSReg = *SSRegs; ++SSRegs)
          PhysRegUse[SSReg] = LastRefOrPartRef;
      }
      for (const unsigned *SS = TRI->getSubRegisters(SubReg); *SS; ++SS)
        PartUses.erase(*SS);
    }
  } else if (LastRefOrPartRef == PhysRegDef[Reg] && LastRefOrPartRef != MI) {
    if (LastPartDef)
      // The last partial def kills the register.
      LastPartDef->addOperand(MachineOperand::CreateReg(Reg, false/*IsDef*/,
                                                true/*IsImp*/, true/*IsKill*/));
    else {
      MachineOperand *MO =
        LastRefOrPartRef->findRegisterDefOperand(Reg, false, TRI);
      bool NeedEC = MO->isEarlyClobber() && MO->getReg() != Reg;
      // If the last reference is the last def, then it's not used at all.
      // That is, unless we are currently processing the last reference itself.
      LastRefOrPartRef->addRegisterDead(Reg, TRI, true);
      if (NeedEC) {
        // If we are adding a subreg def and the superreg def is marked early
        // clobber, add an early clobber marker to the subreg def.
        MO = LastRefOrPartRef->findRegisterDefOperand(Reg);
        if (MO)
          MO->setIsEarlyClobber();
      }
    }
  } else
    LastRefOrPartRef->addRegisterKilled(Reg, TRI, true);
  return true;
}

void LiveVariables::HandlePhysRegDef(unsigned Reg, MachineInstr *MI,
                                     SmallVector<unsigned, 4> &Defs) {
  // What parts of the register are previously defined?
  SmallSet<unsigned, 32> Live;
  if (PhysRegDef[Reg] || PhysRegUse[Reg]) {
    Live.insert(Reg);
    for (const unsigned *SS = TRI->getSubRegisters(Reg); *SS; ++SS)
      Live.insert(*SS);
  } else {
    for (const unsigned *SubRegs = TRI->getSubRegisters(Reg);
         unsigned SubReg = *SubRegs; ++SubRegs) {
      // If a register isn't itself defined, but all parts that make up of it
      // are defined, then consider it also defined.
      // e.g.
      // AL =
      // AH =
      //    = AX
      if (Live.count(SubReg))
        continue;
      if (PhysRegDef[SubReg] || PhysRegUse[SubReg]) {
        Live.insert(SubReg);
        for (const unsigned *SS = TRI->getSubRegisters(SubReg); *SS; ++SS)
          Live.insert(*SS);
      }
    }
  }

  // Start from the largest piece, find the last time any part of the register
  // is referenced.
  HandlePhysRegKill(Reg, MI);
  // Only some of the sub-registers are used.
  for (const unsigned *SubRegs = TRI->getSubRegisters(Reg);
       unsigned SubReg = *SubRegs; ++SubRegs) {
    if (!Live.count(SubReg))
      // Skip if this sub-register isn't defined.
      continue;
    HandlePhysRegKill(SubReg, MI);
  }

  if (MI)
    Defs.push_back(Reg);  // Remember this def.
}

void LiveVariables::UpdatePhysRegDefs(MachineInstr *MI,
                                      SmallVector<unsigned, 4> &Defs) {
  while (!Defs.empty()) {
    unsigned Reg = Defs.back();
    Defs.pop_back();
    PhysRegDef[Reg]  = MI;
    PhysRegUse[Reg]  = NULL;
    for (const unsigned *SubRegs = TRI->getSubRegisters(Reg);
         unsigned SubReg = *SubRegs; ++SubRegs) {
      PhysRegDef[SubReg]  = MI;
      PhysRegUse[SubReg]  = NULL;
    }
  }
}

namespace {
  struct RegSorter {
    const TargetRegisterInfo *TRI;

    RegSorter(const TargetRegisterInfo *tri) : TRI(tri) { }
    bool operator()(unsigned A, unsigned B) {
      if (TRI->isSubRegister(A, B))
        return true;
      else if (TRI->isSubRegister(B, A))
        return false;
      return A < B;
    }
  };
}

bool LiveVariables::runOnMachineFunction(MachineFunction &mf) {
  MF = &mf;
  MRI = &mf.getRegInfo();
  TRI = MF->getTarget().getRegisterInfo();

  ReservedRegisters = TRI->getReservedRegs(mf);

  unsigned NumRegs = TRI->getNumRegs();
  PhysRegDef  = new MachineInstr*[NumRegs];
  PhysRegUse  = new MachineInstr*[NumRegs];
  PHIVarInfo = new SmallVector<unsigned, 4>[MF->getNumBlockIDs()];
  std::fill(PhysRegDef,  PhysRegDef  + NumRegs, (MachineInstr*)0);
  std::fill(PhysRegUse,  PhysRegUse  + NumRegs, (MachineInstr*)0);
  PHIJoins.clear();

  /// Get some space for a respectable number of registers.
  VirtRegInfo.resize(64);

  analyzePHINodes(mf);

  // Calculate live variable information in depth first order on the CFG of the
  // function.  This guarantees that we will see the definition of a virtual
  // register before its uses due to dominance properties of SSA (except for PHI
  // nodes, which are treated as a special case).
  MachineBasicBlock *Entry = MF->begin();
  SmallPtrSet<MachineBasicBlock*,16> Visited;

  for (df_ext_iterator<MachineBasicBlock*, SmallPtrSet<MachineBasicBlock*,16> >
         DFI = df_ext_begin(Entry, Visited), E = df_ext_end(Entry, Visited);
       DFI != E; ++DFI) {
    MachineBasicBlock *MBB = *DFI;

    // Mark live-in registers as live-in.
    SmallVector<unsigned, 4> Defs;
    for (MachineBasicBlock::const_livein_iterator II = MBB->livein_begin(),
           EE = MBB->livein_end(); II != EE; ++II) {
      assert(TargetRegisterInfo::isPhysicalRegister(*II) &&
             "Cannot have a live-in virtual register!");
      HandlePhysRegDef(*II, 0, Defs);
    }

    // Loop over all of the instructions, processing them.
    DistanceMap.clear();
    unsigned Dist = 0;
    for (MachineBasicBlock::iterator I = MBB->begin(), E = MBB->end();
         I != E; ++I) {
      MachineInstr *MI = I;
      if (MI->isDebugValue())
        continue;
      DistanceMap.insert(std::make_pair(MI, Dist++));

      // Process all of the operands of the instruction...
      unsigned NumOperandsToProcess = MI->getNumOperands();

      // Unless it is a PHI node.  In this case, ONLY process the DEF, not any
      // of the uses.  They will be handled in other basic blocks.
      if (MI->isPHI())
        NumOperandsToProcess = 1;

      // Clear kill and dead markers. LV will recompute them.
      SmallVector<unsigned, 4> UseRegs;
      SmallVector<unsigned, 4> DefRegs;
      for (unsigned i = 0; i != NumOperandsToProcess; ++i) {
        MachineOperand &MO = MI->getOperand(i);
        if (!MO.isReg() || MO.getReg() == 0)
          continue;
        unsigned MOReg = MO.getReg();
        if (MO.isUse()) {
          MO.setIsKill(false);
          UseRegs.push_back(MOReg);
        } else /*MO.isDef()*/ {
          MO.setIsDead(false);
          DefRegs.push_back(MOReg);
        }
      }

      // Process all uses.
      for (unsigned i = 0, e = UseRegs.size(); i != e; ++i) {
        unsigned MOReg = UseRegs[i];
        if (TargetRegisterInfo::isVirtualRegister(MOReg))
          HandleVirtRegUse(MOReg, MBB, MI);
        else if (!ReservedRegisters[MOReg])
          HandlePhysRegUse(MOReg, MI);
      }

      // Process all defs.
      for (unsigned i = 0, e = DefRegs.size(); i != e; ++i) {
        unsigned MOReg = DefRegs[i];
        if (TargetRegisterInfo::isVirtualRegister(MOReg))
          HandleVirtRegDef(MOReg, MI);
        else if (!ReservedRegisters[MOReg])
          HandlePhysRegDef(MOReg, MI, Defs);
      }
      UpdatePhysRegDefs(MI, Defs);
    }

    // Handle any virtual assignments from PHI nodes which might be at the
    // bottom of this basic block.  We check all of our successor blocks to see
    // if they have PHI nodes, and if so, we simulate an assignment at the end
    // of the current block.
    if (!PHIVarInfo[MBB->getNumber()].empty()) {
      SmallVector<unsigned, 4>& VarInfoVec = PHIVarInfo[MBB->getNumber()];

      for (SmallVector<unsigned, 4>::iterator I = VarInfoVec.begin(),
             E = VarInfoVec.end(); I != E; ++I)
        // Mark it alive only in the block we are representing.
        MarkVirtRegAliveInBlock(getVarInfo(*I),MRI->getVRegDef(*I)->getParent(),
                                MBB);
    }

    // Finally, if the last instruction in the block is a return, make sure to
    // mark it as using all of the live-out values in the function.
    if (!MBB->empty() && MBB->back().getDesc().isReturn()) {
      MachineInstr *Ret = &MBB->back();

      for (MachineRegisterInfo::liveout_iterator
           I = MF->getRegInfo().liveout_begin(),
           E = MF->getRegInfo().liveout_end(); I != E; ++I) {
        assert(TargetRegisterInfo::isPhysicalRegister(*I) &&
               "Cannot have a live-out virtual register!");
        HandlePhysRegUse(*I, Ret);

        // Add live-out registers as implicit uses.
        if (!Ret->readsRegister(*I))
          Ret->addOperand(MachineOperand::CreateReg(*I, false, true));
      }
    }

    // Loop over PhysRegDef / PhysRegUse, killing any registers that are
    // available at the end of the basic block.
    for (unsigned i = 0; i != NumRegs; ++i)
      if (PhysRegDef[i] || PhysRegUse[i])
        HandlePhysRegDef(i, 0, Defs);

    std::fill(PhysRegDef,  PhysRegDef  + NumRegs, (MachineInstr*)0);
    std::fill(PhysRegUse,  PhysRegUse  + NumRegs, (MachineInstr*)0);
  }

  // Convert and transfer the dead / killed information we have gathered into
  // VirtRegInfo onto MI's.
  for (unsigned i = 0, e1 = VirtRegInfo.size(); i != e1; ++i)
    for (unsigned j = 0, e2 = VirtRegInfo[i].Kills.size(); j != e2; ++j)
      if (VirtRegInfo[i].Kills[j] ==
          MRI->getVRegDef(i + TargetRegisterInfo::FirstVirtualRegister))
        VirtRegInfo[i]
          .Kills[j]->addRegisterDead(i +
                                     TargetRegisterInfo::FirstVirtualRegister,
                                     TRI);
      else
        VirtRegInfo[i]
          .Kills[j]->addRegisterKilled(i +
                                       TargetRegisterInfo::FirstVirtualRegister,
                                       TRI);

  // Check to make sure there are no unreachable blocks in the MC CFG for the
  // function.  If so, it is due to a bug in the instruction selector or some
  // other part of the code generator if this happens.
#ifndef NDEBUG
  for(MachineFunction::iterator i = MF->begin(), e = MF->end(); i != e; ++i)
    assert(Visited.count(&*i) != 0 && "unreachable basic block found");
#endif

  delete[] PhysRegDef;
  delete[] PhysRegUse;
  delete[] PHIVarInfo;

  return false;
}

/// replaceKillInstruction - Update register kill info by replacing a kill
/// instruction with a new one.
void LiveVariables::replaceKillInstruction(unsigned Reg, MachineInstr *OldMI,
                                           MachineInstr *NewMI) {
  VarInfo &VI = getVarInfo(Reg);
  std::replace(VI.Kills.begin(), VI.Kills.end(), OldMI, NewMI);
}

/// removeVirtualRegistersKilled - Remove all killed info for the specified
/// instruction.
void LiveVariables::removeVirtualRegistersKilled(MachineInstr *MI) {
  for (unsigned i = 0, e = MI->getNumOperands(); i != e; ++i) {
    MachineOperand &MO = MI->getOperand(i);
    if (MO.isReg() && MO.isKill()) {
      MO.setIsKill(false);
      unsigned Reg = MO.getReg();
      if (TargetRegisterInfo::isVirtualRegister(Reg)) {
        bool removed = getVarInfo(Reg).removeKill(MI);
        assert(removed && "kill not in register's VarInfo?");
        removed = true;
      }
    }
  }
}

/// analyzePHINodes - Gather information about the PHI nodes in here. In
/// particular, we want to map the variable information of a virtual register
/// which is used in a PHI node. We map that to the BB the vreg is coming from.
///
void LiveVariables::analyzePHINodes(const MachineFunction& Fn) {
  for (MachineFunction::const_iterator I = Fn.begin(), E = Fn.end();
       I != E; ++I)
    for (MachineBasicBlock::const_iterator BBI = I->begin(), BBE = I->end();
         BBI != BBE && BBI->isPHI(); ++BBI)
      for (unsigned i = 1, e = BBI->getNumOperands(); i != e; i += 2)
        PHIVarInfo[BBI->getOperand(i + 1).getMBB()->getNumber()]
          .push_back(BBI->getOperand(i).getReg());
}

bool LiveVariables::VarInfo::isLiveIn(const MachineBasicBlock &MBB,
                                      unsigned Reg,
                                      MachineRegisterInfo &MRI) {
  unsigned Num = MBB.getNumber();

  // Reg is live-through.
  if (AliveBlocks.test(Num))
    return true;

  // Registers defined in MBB cannot be live in.
  const MachineInstr *Def = MRI.getVRegDef(Reg);
  if (Def && Def->getParent() == &MBB)
    return false;

 // Reg was not defined in MBB, was it killed here?
  return findKill(&MBB);
}

bool LiveVariables::isLiveOut(unsigned Reg, const MachineBasicBlock &MBB) {
  LiveVariables::VarInfo &VI = getVarInfo(Reg);

  // Loop over all of the successors of the basic block, checking to see if
  // the value is either live in the block, or if it is killed in the block.
  std::vector<MachineBasicBlock*> OpSuccBlocks;
  for (MachineBasicBlock::const_succ_iterator SI = MBB.succ_begin(),
         E = MBB.succ_end(); SI != E; ++SI) {
    MachineBasicBlock *SuccMBB = *SI;

    // Is it alive in this successor?
    unsigned SuccIdx = SuccMBB->getNumber();
    if (VI.AliveBlocks.test(SuccIdx))
      return true;
    OpSuccBlocks.push_back(SuccMBB);
  }

  // Check to see if this value is live because there is a use in a successor
  // that kills it.
  switch (OpSuccBlocks.size()) {
  case 1: {
    MachineBasicBlock *SuccMBB = OpSuccBlocks[0];
    for (unsigned i = 0, e = VI.Kills.size(); i != e; ++i)
      if (VI.Kills[i]->getParent() == SuccMBB)
        return true;
    break;
  }
  case 2: {
    MachineBasicBlock *SuccMBB1 = OpSuccBlocks[0], *SuccMBB2 = OpSuccBlocks[1];
    for (unsigned i = 0, e = VI.Kills.size(); i != e; ++i)
      if (VI.Kills[i]->getParent() == SuccMBB1 ||
          VI.Kills[i]->getParent() == SuccMBB2)
        return true;
    break;
  }
  default:
    std::sort(OpSuccBlocks.begin(), OpSuccBlocks.end());
    for (unsigned i = 0, e = VI.Kills.size(); i != e; ++i)
      if (std::binary_search(OpSuccBlocks.begin(), OpSuccBlocks.end(),
                             VI.Kills[i]->getParent()))
        return true;
  }
  return false;
}

/// addNewBlock - Add a new basic block BB as an empty succcessor to DomBB. All
/// variables that are live out of DomBB will be marked as passing live through
/// BB.
void LiveVariables::addNewBlock(MachineBasicBlock *BB,
                                MachineBasicBlock *DomBB,
                                MachineBasicBlock *SuccBB) {
  const unsigned NumNew = BB->getNumber();

  // All registers used by PHI nodes in SuccBB must be live through BB.
  for (MachineBasicBlock::const_iterator BBI = SuccBB->begin(),
         BBE = SuccBB->end(); BBI != BBE && BBI->isPHI(); ++BBI)
    for (unsigned i = 1, e = BBI->getNumOperands(); i != e; i += 2)
      if (BBI->getOperand(i+1).getMBB() == BB)
        getVarInfo(BBI->getOperand(i).getReg()).AliveBlocks.set(NumNew);

  // Update info for all live variables
  for (unsigned Reg = TargetRegisterInfo::FirstVirtualRegister,
         E = MRI->getLastVirtReg()+1; Reg != E; ++Reg) {
    VarInfo &VI = getVarInfo(Reg);
    if (!VI.AliveBlocks.test(NumNew) && VI.isLiveIn(*SuccBB, Reg, *MRI))
      VI.AliveBlocks.set(NumNew);
  }
}
