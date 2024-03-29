//===-- PPCCodeEmitter.cpp - JIT Code Emitter for PowerPC32 -------*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the PowerPC 32-bit CodeEmitter and associated machinery to
// JIT-compile bitcode to native PowerPC.
//
//===----------------------------------------------------------------------===//

#include "PPCTargetMachine.h"
#include "PPCRelocations.h"
#include "PPC.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/CodeGen/JITCodeEmitter.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetOptions.h"
using namespace llvm;

namespace {
  class PPCCodeEmitter : public MachineFunctionPass {
    TargetMachine &TM;
    JITCodeEmitter &MCE;
    MachineModuleInfo *MMI;
    
    void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<MachineModuleInfo>();
      MachineFunctionPass::getAnalysisUsage(AU);
    }
    
    static char ID;
    
    /// MovePCtoLROffset - When/if we see a MovePCtoLR instruction, we record
    /// its address in the function into this pointer.
    void *MovePCtoLROffset;
  public:
    
    PPCCodeEmitter(TargetMachine &tm, JITCodeEmitter &mce)
      : MachineFunctionPass(&ID), TM(tm), MCE(mce) {}

    /// getBinaryCodeForInstr - This function, generated by the
    /// CodeEmitterGenerator using TableGen, produces the binary encoding for
    /// machine instructions.

    unsigned getBinaryCodeForInstr(const MachineInstr &MI);

    /// getMachineOpValue - evaluates the MachineOperand of a given MachineInstr

    unsigned getMachineOpValue(const MachineInstr &MI,
                               const MachineOperand &MO);

    const char *getPassName() const { return "PowerPC Machine Code Emitter"; }

    /// runOnMachineFunction - emits the given MachineFunction to memory
    ///
    bool runOnMachineFunction(MachineFunction &MF);

    /// emitBasicBlock - emits the given MachineBasicBlock to memory
    ///
    void emitBasicBlock(MachineBasicBlock &MBB);

    /// getValueBit - return the particular bit of Val
    ///
    unsigned getValueBit(int64_t Val, unsigned bit) { return (Val >> bit) & 1; }
  };
}

char PPCCodeEmitter::ID = 0;

/// createPPCCodeEmitterPass - Return a pass that emits the collected PPC code
/// to the specified MCE object.
FunctionPass *llvm::createPPCJITCodeEmitterPass(PPCTargetMachine &TM,
                                                JITCodeEmitter &JCE) {
  return new PPCCodeEmitter(TM, JCE);
}

bool PPCCodeEmitter::runOnMachineFunction(MachineFunction &MF) {
  assert((MF.getTarget().getRelocationModel() != Reloc::Default ||
          MF.getTarget().getRelocationModel() != Reloc::Static) &&
         "JIT relocation model must be set to static or default!");

  MMI = &getAnalysis<MachineModuleInfo>();
  MCE.setModuleInfo(MMI);
  do {
    MovePCtoLROffset = 0;
    MCE.startFunction(MF);
    for (MachineFunction::iterator BB = MF.begin(), E = MF.end(); BB != E; ++BB)
      emitBasicBlock(*BB);
  } while (MCE.finishFunction(MF));

  return false;
}

void PPCCodeEmitter::emitBasicBlock(MachineBasicBlock &MBB) {
  MCE.StartMachineBasicBlock(&MBB);

  for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end(); I != E; ++I){
    const MachineInstr &MI = *I;
    MCE.processDebugLoc(MI.getDebugLoc(), true);
    switch (MI.getOpcode()) {
    default:
      MCE.emitWordBE(getBinaryCodeForInstr(MI));
      break;
    case TargetOpcode::DBG_LABEL:
    case TargetOpcode::EH_LABEL:
      MCE.emitLabel(MI.getOperand(0).getMCSymbol());
      break;
    case TargetOpcode::IMPLICIT_DEF:
    case TargetOpcode::KILL:
      break; // pseudo opcode, no side effects
    case PPC::MovePCtoLR:
    case PPC::MovePCtoLR8:
      assert(TM.getRelocationModel() == Reloc::PIC_);
      MovePCtoLROffset = (void*)MCE.getCurrentPCValue();
      MCE.emitWordBE(0x48000005);   // bl 1
      break;
    }
    MCE.processDebugLoc(MI.getDebugLoc(), false);
  }
}

unsigned PPCCodeEmitter::getMachineOpValue(const MachineInstr &MI,
                                           const MachineOperand &MO) {

  unsigned rv = 0; // Return value; defaults to 0 for unhandled cases
                   // or things that get fixed up later by the JIT.
  if (MO.isReg()) {
    rv = PPCRegisterInfo::getRegisterNumbering(MO.getReg());

    // Special encoding for MTCRF and MFOCRF, which uses a bit mask for the
    // register, not the register number directly.
    if ((MI.getOpcode() == PPC::MTCRF || MI.getOpcode() == PPC::MFOCRF) &&
        (MO.getReg() >= PPC::CR0 && MO.getReg() <= PPC::CR7)) {
      rv = 0x80 >> rv;
    }
  } else if (MO.isImm()) {
    rv = MO.getImm();
  } else if (MO.isGlobal() || MO.isSymbol() ||
             MO.isCPI() || MO.isJTI()) {
    unsigned Reloc = 0;
    if (MI.getOpcode() == PPC::BL_Darwin || MI.getOpcode() == PPC::BL8_Darwin ||
        MI.getOpcode() == PPC::BL_SVR4 || MI.getOpcode() == PPC::BL8_ELF ||
        MI.getOpcode() == PPC::TAILB || MI.getOpcode() == PPC::TAILB8)
      Reloc = PPC::reloc_pcrel_bx;
    else {
      if (TM.getRelocationModel() == Reloc::PIC_) {
        assert(MovePCtoLROffset && "MovePCtoLR not seen yet?");
      }
      switch (MI.getOpcode()) {
      default: MI.dump(); llvm_unreachable("Unknown instruction for relocation!");
      case PPC::LIS:
      case PPC::LIS8:
      case PPC::ADDIS:
      case PPC::ADDIS8:
        Reloc = PPC::reloc_absolute_high;       // Pointer to symbol
        break;
      case PPC::LI:
      case PPC::LI8:
      case PPC::LA:
      // Loads.
      case PPC::LBZ:
      case PPC::LBZ8:
      case PPC::LHA:
      case PPC::LHA8:
      case PPC::LHZ:
      case PPC::LHZ8:
      case PPC::LWZ:
      case PPC::LWZ8:
      case PPC::LFS:
      case PPC::LFD:

      // Stores.
      case PPC::STB:
      case PPC::STB8:
      case PPC::STH:
      case PPC::STH8:
      case PPC::STW:
      case PPC::STW8:
      case PPC::STFS:
      case PPC::STFD:
        Reloc = PPC::reloc_absolute_low;
        break;

      case PPC::LWA:
      case PPC::LD:
      case PPC::STD:
      case PPC::STD_32:
        Reloc = PPC::reloc_absolute_low_ix;
        break;
      }
    }

    MachineRelocation R;
    if (MO.isGlobal()) {
      R = MachineRelocation::getGV(MCE.getCurrentPCOffset(), Reloc,
                                   MO.getGlobal(), 0,
                                   isa<Function>(MO.getGlobal()));
    } else if (MO.isSymbol()) {
      R = MachineRelocation::getExtSym(MCE.getCurrentPCOffset(),
                                       Reloc, MO.getSymbolName(), 0);
    } else if (MO.isCPI()) {
      R = MachineRelocation::getConstPool(MCE.getCurrentPCOffset(),
                                          Reloc, MO.getIndex(), 0);
    } else {
      assert(MO.isJTI());
      R = MachineRelocation::getJumpTable(MCE.getCurrentPCOffset(),
                                          Reloc, MO.getIndex(), 0);
    }

    // If in PIC mode, we need to encode the negated address of the
    // 'movepctolr' into the unrelocated field.  After relocation, we'll have
    // &gv-&movepctolr-4 in the imm field.  Once &movepctolr is added to the imm
    // field, we get &gv.  This doesn't happen for branch relocations, which are
    // always implicitly pc relative.
    if (TM.getRelocationModel() == Reloc::PIC_ && Reloc != PPC::reloc_pcrel_bx){
      assert(MovePCtoLROffset && "MovePCtoLR not seen yet?");
      R.setConstantVal(-(intptr_t)MovePCtoLROffset - 4);
    }
    MCE.addRelocation(R);

  } else if (MO.isMBB()) {
    unsigned Reloc = 0;
    unsigned Opcode = MI.getOpcode();
    if (Opcode == PPC::B || Opcode == PPC::BL_Darwin ||
        Opcode == PPC::BLA_Darwin|| Opcode == PPC::BL_SVR4 ||
        Opcode == PPC::BLA_SVR4)
      Reloc = PPC::reloc_pcrel_bx;
    else // BCC instruction
      Reloc = PPC::reloc_pcrel_bcx;

    MCE.addRelocation(MachineRelocation::getBB(MCE.getCurrentPCOffset(),
                                               Reloc, MO.getMBB()));
  } else {
#ifndef NDEBUG
    errs() << "ERROR: Unknown type of MachineOperand: " << MO << "\n";
#endif
    llvm_unreachable(0);
  }

  return rv;
}

#include "PPCGenCodeEmitter.inc"
