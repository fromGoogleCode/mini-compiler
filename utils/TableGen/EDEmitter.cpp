//===- EDEmitter.cpp - Generate instruction descriptions for ED -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This tablegen backend is responsible for emitting a description of each
// instruction in a format that the enhanced disassembler can use to tokenize
// and parse instructions.
//
//===----------------------------------------------------------------------===//

#include "EDEmitter.h"

#include "AsmWriterInst.h"
#include "CodeGenTarget.h"
#include "Record.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <string>
#include <vector>

#define MAX_OPERANDS 13
#define MAX_SYNTAXES 2

using namespace llvm;

///////////////////////////////////////////////////////////
// Support classes for emitting nested C data structures //
///////////////////////////////////////////////////////////

namespace {
  
  class EnumEmitter {
  private:
    std::string Name;
    std::vector<std::string> Entries;
  public:
    EnumEmitter(const char *N) : Name(N) { 
    }
    int addEntry(const char *e) { 
      Entries.push_back(std::string(e));
      return Entries.size() - 1; 
    }
    void emit(raw_ostream &o, unsigned int &i) {
      o.indent(i) << "enum " << Name.c_str() << " {" << "\n";
      i += 2;
      
      unsigned int index = 0;
      unsigned int numEntries = Entries.size();
      for (index = 0; index < numEntries; ++index) {
        o.indent(i) << Entries[index];
        if (index < (numEntries - 1))
          o << ",";
        o << "\n";
      }
      
      i -= 2;
      o.indent(i) << "};" << "\n";
    }
    
    void emitAsFlags(raw_ostream &o, unsigned int &i) {
      o.indent(i) << "enum " << Name.c_str() << " {" << "\n";
      i += 2;
      
      unsigned int index = 0;
      unsigned int numEntries = Entries.size();
      unsigned int flag = 1;
      for (index = 0; index < numEntries; ++index) {
        o.indent(i) << Entries[index] << " = " << format("0x%x", flag);
        if (index < (numEntries - 1))
          o << ",";
        o << "\n";
        flag <<= 1;
      }
      
      i -= 2;
      o.indent(i) << "};" << "\n";
    }
  };

  class StructEmitter {
  private:
    std::string Name;
    typedef std::pair<const char*, const char*> member;
    std::vector< member > Members;
  public:
    StructEmitter(const char *N) : Name(N) {
    }
    void addMember(const char *t, const char *n) {
      member m(t, n);
      Members.push_back(m);
    }
    void emit(raw_ostream &o, unsigned int &i) {
      o.indent(i) << "struct " << Name.c_str() << " {" << "\n";
      i += 2;
      
      unsigned int index = 0;
      unsigned int numMembers = Members.size();
      for (index = 0; index < numMembers; ++index) {
        o.indent(i) << Members[index].first << " ";
        o.indent(i) << Members[index].second << ";" << "\n";
      }
      
      i -= 2;
      o.indent(i) << "};" << "\n";
    }
  };
  
  class ConstantEmitter {
  public:
    virtual ~ConstantEmitter() { }
    virtual void emit(raw_ostream &o, unsigned int &i) = 0;
  };
  
  class LiteralConstantEmitter : public ConstantEmitter {
  private:
    bool IsNumber;
    union {
      int Number;
      const char* String;
    };
  public:
    LiteralConstantEmitter(const char *string) : 
      IsNumber(false),
      String(string) {
    }
    LiteralConstantEmitter(int number = 0) : 
      IsNumber(true),
      Number(number) {
    }
    void set(const char *string) {
      IsNumber = false;
      Number = 0;
      String = string;
    }
    void set(int number) {
      IsNumber = true;
      String = NULL;
      Number = number;
    }
    bool is(const char *string) {
      return !strcmp(String, string);
    }
    void emit(raw_ostream &o, unsigned int &i) {
      if (IsNumber)
        o << Number;
      else
        o << String;
    }
  };
  
  class CompoundConstantEmitter : public ConstantEmitter {
  private:
    unsigned int Padding;
    std::vector<ConstantEmitter *> Entries;
  public:
    CompoundConstantEmitter(unsigned int padding = 0) : Padding(padding) {
    }
    CompoundConstantEmitter &addEntry(ConstantEmitter *e) {
      Entries.push_back(e);
      
      return *this;
    }
    ~CompoundConstantEmitter() {
      while (Entries.size()) {
        ConstantEmitter *entry = Entries.back();
        Entries.pop_back();
        delete entry;
      }
    }
    void emit(raw_ostream &o, unsigned int &i) {
      o << "{" << "\n";
      i += 2;
  
      unsigned int index;
      unsigned int numEntries = Entries.size();
      
      unsigned int numToPrint;
      
      if (Padding) {
        if (numEntries > Padding) {
          fprintf(stderr, "%u entries but %u padding\n", numEntries, Padding);
          llvm_unreachable("More entries than padding");
        }
        numToPrint = Padding;
      } else {
        numToPrint = numEntries;
      }
          
      for (index = 0; index < numToPrint; ++index) {
        o.indent(i);
        if (index < numEntries)
          Entries[index]->emit(o, i);
        else
          o << "-1";
        
        if (index < (numToPrint - 1))
          o << ",";
        o << "\n";
      }
      
      i -= 2;
      o.indent(i) << "}";
    }
  };
  
  class FlagsConstantEmitter : public ConstantEmitter {
  private:
    std::vector<std::string> Flags;
  public:
    FlagsConstantEmitter() {
    }
    FlagsConstantEmitter &addEntry(const char *f) {
      Flags.push_back(std::string(f));
      return *this;
    }
    void emit(raw_ostream &o, unsigned int &i) {
      unsigned int index;
      unsigned int numFlags = Flags.size();
      if (numFlags == 0)
        o << "0";
      
      for (index = 0; index < numFlags; ++index) {
        o << Flags[index].c_str();
        if (index < (numFlags - 1))
          o << " | ";
      }
    }
  };
}

EDEmitter::EDEmitter(RecordKeeper &R) : Records(R) {
}

/// populateOperandOrder - Accepts a CodeGenInstruction and generates its
///   AsmWriterInst for the desired assembly syntax, giving an ordered list of
///   operands in the order they appear in the printed instruction.  Then, for
///   each entry in that list, determines the index of the same operand in the
///   CodeGenInstruction, and emits the resulting mapping into an array, filling
///   in unused slots with -1.
///
/// @arg operandOrder - The array that will be populated with the operand
///                     mapping.  Each entry will contain -1 (invalid index
///                     into the operands present in the AsmString) or a number
///                     representing an index in the operand descriptor array.
/// @arg inst         - The instruction to use when looking up the operands
/// @arg syntax       - The syntax to use, according to LLVM's enumeration
void populateOperandOrder(CompoundConstantEmitter *operandOrder,
                          const CodeGenInstruction &inst,
                          unsigned syntax) {
  unsigned int numArgs = 0;
  
  AsmWriterInst awInst(inst, syntax, -1, -1);
  
  std::vector<AsmWriterOperand>::iterator operandIterator;
  
  for (operandIterator = awInst.Operands.begin();
       operandIterator != awInst.Operands.end();
       ++operandIterator) {
    if (operandIterator->OperandType == 
        AsmWriterOperand::isMachineInstrOperand) {
      operandOrder->addEntry(
        new LiteralConstantEmitter(operandIterator->CGIOpNo));
      numArgs++;
    }
  }
}

/////////////////////////////////////////////////////
// Support functions for handling X86 instructions //
/////////////////////////////////////////////////////

#define SET(flag) { type->set(flag); return 0; }

#define REG(str) if (name == str) SET("kOperandTypeRegister");
#define MEM(str) if (name == str) SET("kOperandTypeX86Memory");
#define LEA(str) if (name == str) SET("kOperandTypeX86EffectiveAddress");
#define IMM(str) if (name == str) SET("kOperandTypeImmediate");
#define PCR(str) if (name == str) SET("kOperandTypeX86PCRelative");

/// X86TypeFromOpName - Processes the name of a single X86 operand (which is
///   actually its type) and translates it into an operand type
///
/// @arg flags    - The type object to set
/// @arg name     - The name of the operand
static int X86TypeFromOpName(LiteralConstantEmitter *type,
                             const std::string &name) {
  REG("GR8");
  REG("GR8_NOREX");
  REG("GR16");
  REG("GR32");
  REG("GR32_NOREX");
  REG("GR32_TC");
  REG("FR32");
  REG("RFP32");
  REG("GR64");
  REG("GR64_TC");
  REG("FR64");
  REG("VR64");
  REG("RFP64");
  REG("RFP80");
  REG("VR128");
  REG("RST");
  REG("SEGMENT_REG");
  REG("DEBUG_REG");
  REG("CONTROL_REG_32");
  REG("CONTROL_REG_64");
  
  IMM("i8imm");
  IMM("i16imm");
  IMM("i16i8imm");
  IMM("i32imm");
  IMM("i32imm_pcrel");
  IMM("i32i8imm");
  IMM("i64imm");
  IMM("i64i8imm");
  IMM("i64i32imm");
  IMM("i64i32imm_pcrel");
  IMM("SSECC");
  
  // all R, I, R, I, R
  MEM("i8mem");
  MEM("i8mem_NOREX");
  MEM("i16mem");
  MEM("i32mem");
  MEM("i32mem_TC");
  MEM("f32mem");
  MEM("ssmem");
  MEM("opaque32mem");
  MEM("opaque48mem");
  MEM("i64mem");
  MEM("i64mem_TC");
  MEM("f64mem");
  MEM("sdmem");
  MEM("f80mem");
  MEM("opaque80mem");
  MEM("i128mem");
  MEM("f128mem");
  MEM("opaque512mem");
  
  // all R, I, R, I
  LEA("lea32mem");
  LEA("lea64_32mem");
  LEA("lea64mem");
  
  // all I
  PCR("brtarget8");
  PCR("offset8");
  PCR("offset16");
  PCR("offset32");
  PCR("offset64");
  PCR("brtarget");
  
  return 1;
}

#undef REG
#undef MEM
#undef LEA
#undef IMM
#undef PCR

#undef SET

/// X86PopulateOperands - Handles all the operands in an X86 instruction, adding
///   the appropriate flags to their descriptors
///
/// @operandFlags - A reference the array of operand flag objects
/// @inst         - The instruction to use as a source of information
static void X86PopulateOperands(
  LiteralConstantEmitter *(&operandTypes)[MAX_OPERANDS],
  const CodeGenInstruction &inst) {
  if (!inst.TheDef->isSubClassOf("X86Inst"))
    return;
  
  unsigned int index;
  unsigned int numOperands = inst.OperandList.size();
  
  for (index = 0; index < numOperands; ++index) {
    const CodeGenInstruction::OperandInfo &operandInfo = 
      inst.OperandList[index];
    Record &rec = *operandInfo.Rec;
    
    if (X86TypeFromOpName(operandTypes[index], rec.getName())) {
      errs() << "Operand type: " << rec.getName().c_str() << "\n";
      errs() << "Operand name: " << operandInfo.Name.c_str() << "\n";
      errs() << "Instruction mame: " << inst.TheDef->getName().c_str() << "\n";
      llvm_unreachable("Unhandled type");
    }
  }
}

/// decorate1 - Decorates a named operand with a new flag
///
/// @operandFlags - The array of operand flag objects, which don't have names
/// @inst         - The CodeGenInstruction, which provides a way to translate
///                 between names and operand indices
/// @opName       - The name of the operand
/// @flag         - The name of the flag to add
static inline void decorate1(
  FlagsConstantEmitter *(&operandFlags)[MAX_OPERANDS],
  const CodeGenInstruction &inst,
  const char *opName,
  const char *opFlag) {
  unsigned opIndex;
  
  opIndex = inst.getOperandNamed(std::string(opName));
  
  operandFlags[opIndex]->addEntry(opFlag);
}

#define DECORATE1(opName, opFlag) decorate1(operandFlags, inst, opName, opFlag)

#define MOV(source, target) {               \
  instType.set("kInstructionTypeMove");     \
  DECORATE1(source, "kOperandFlagSource");  \
  DECORATE1(target, "kOperandFlagTarget");  \
}

#define BRANCH(target) {                    \
  instType.set("kInstructionTypeBranch");   \
  DECORATE1(target, "kOperandFlagTarget");  \
}

#define PUSH(source) {                      \
  instType.set("kInstructionTypePush");     \
  DECORATE1(source, "kOperandFlagSource");  \
}

#define POP(target) {                       \
  instType.set("kInstructionTypePop");      \
  DECORATE1(target, "kOperandFlagTarget");  \
}

#define CALL(target) {                      \
  instType.set("kInstructionTypeCall");     \
  DECORATE1(target, "kOperandFlagTarget");  \
}

#define RETURN() {                          \
  instType.set("kInstructionTypeReturn");   \
}

/// X86ExtractSemantics - Performs various checks on the name of an X86
///   instruction to determine what sort of an instruction it is and then adds 
///   the appropriate flags to the instruction and its operands
///
/// @arg instType     - A reference to the type for the instruction as a whole
/// @arg operandFlags - A reference to the array of operand flag object pointers
/// @arg inst         - A reference to the original instruction
static void X86ExtractSemantics(
  LiteralConstantEmitter &instType,
  FlagsConstantEmitter *(&operandFlags)[MAX_OPERANDS],
  const CodeGenInstruction &inst) {
  const std::string &name = inst.TheDef->getName();
    
  if (name.find("MOV") != name.npos) {
    if (name.find("MOV_V") != name.npos) {
      // ignore (this is a pseudoinstruction)
    } else if (name.find("MASK") != name.npos) {
      // ignore (this is a masking move)
    } else if (name.find("r0") != name.npos) {
      // ignore (this is a pseudoinstruction)
    } else if (name.find("PS") != name.npos ||
             name.find("PD") != name.npos) {
      // ignore (this is a shuffling move)
    } else if (name.find("MOVS") != name.npos) {
      // ignore (this is a string move)
    } else if (name.find("_F") != name.npos) {
      // TODO handle _F moves to ST(0)
    } else if (name.find("a") != name.npos) {
      // TODO handle moves to/from %ax
    } else if (name.find("CMOV") != name.npos) {
      MOV("src2", "dst");
    } else if (name.find("PC") != name.npos) {
      MOV("label", "reg")
    } else {
      MOV("src", "dst");
    }
  }
  
  if (name.find("JMP") != name.npos ||
      name.find("J") == 0) {
    if (name.find("FAR") != name.npos && name.find("i") != name.npos) {
      BRANCH("off");
    } else {
      BRANCH("dst");
    }
  }
  
  if (name.find("PUSH") != name.npos) {
    if (name.find("FS") != name.npos ||
        name.find("GS") != name.npos) {
      instType.set("kInstructionTypePush");
      // TODO add support for fixed operands
    } else if (name.find("F") != name.npos) {
      // ignore (this pushes onto the FP stack)
    } else if (name[name.length() - 1] == 'm') {
      PUSH("src");
    } else if (name.find("i") != name.npos) {
      PUSH("imm");
    } else {
      PUSH("reg");
    }
  }
  
  if (name.find("POP") != name.npos) {
    if (name.find("POPCNT") != name.npos) {
      // ignore (not a real pop)
    } else if (name.find("FS") != name.npos ||
             name.find("GS") != name.npos) {
      instType.set("kInstructionTypePop");
      // TODO add support for fixed operands
    } else if (name.find("F") != name.npos) {
      // ignore (this pops from the FP stack)
    } else if (name[name.length() - 1] == 'm') {
      POP("dst");
    } else {
      POP("reg");
    }
  }
  
  if (name.find("CALL") != name.npos) {
    if (name.find("ADJ") != name.npos) {
      // ignore (not a call)
    } else if (name.find("SYSCALL") != name.npos) {
      // ignore (doesn't go anywhere we know about)
    } else if (name.find("VMCALL") != name.npos) {
      // ignore (rather different semantics than a regular call)
    } else if (name.find("FAR") != name.npos && name.find("i") != name.npos) {
      CALL("off");
    } else {
      CALL("dst");
    }
  }
  
  if (name.find("RET") != name.npos) {
    RETURN();
  }
}

#undef MOV
#undef BRANCH
#undef PUSH
#undef POP
#undef CALL
#undef RETURN

/////////////////////////////////////////////////////
// Support functions for handling ARM instructions //
/////////////////////////////////////////////////////

#define SET(flag) { type->set(flag); return 0; }

#define REG(str)    if (name == str) SET("kOperandTypeRegister");
#define IMM(str)    if (name == str) SET("kOperandTypeImmediate");

#define MISC(str, type)   if (name == str) SET(type);

/// ARMFlagFromOpName - Processes the name of a single ARM operand (which is
///   actually its type) and translates it into an operand type
///
/// @arg type     - The type object to set
/// @arg name     - The name of the operand
static int ARMFlagFromOpName(LiteralConstantEmitter *type,
                             const std::string &name) {
  REG("GPR");
  REG("cc_out");
  REG("s_cc_out");
  REG("tGPR");
  REG("DPR");
  REG("SPR");
  REG("QPR");
  REG("DPR_VFP2");
  REG("DPR_8");
  
  IMM("i32imm");
  IMM("bf_inv_mask_imm");
  IMM("jtblock_operand");
  IMM("nohash_imm");
  IMM("cpinst_operand");
  IMM("cps_opt");
  IMM("vfp_f64imm");
  IMM("vfp_f32imm");
  IMM("msr_mask");
  IMM("neg_zero");
  IMM("imm0_31");
  IMM("h8imm");
  IMM("h16imm");
  IMM("h32imm");
  IMM("h64imm");
  IMM("imm0_4095");
  IMM("jt2block_operand");
  IMM("t_imm_s4");
  IMM("pclabel");
  
  MISC("brtarget", "kOperandTypeARMBranchTarget");                // ?
  MISC("so_reg", "kOperandTypeARMSoReg");                         // R, R, I
  MISC("t2_so_reg", "kOperandTypeThumb2SoReg");                   // R, I
  MISC("so_imm", "kOperandTypeARMSoImm");                         // I
  MISC("t2_so_imm", "kOperandTypeThumb2SoImm");                   // I
  MISC("so_imm2part", "kOperandTypeARMSoImm2Part");               // I
  MISC("pred", "kOperandTypeARMPredicate");                       // I, R
  MISC("it_pred", "kOperandTypeARMPredicate");                    // I
  MISC("addrmode2", "kOperandTypeARMAddrMode2");                  // R, R, I
  MISC("am2offset", "kOperandTypeARMAddrMode2Offset");            // R, I
  MISC("addrmode3", "kOperandTypeARMAddrMode3");                  // R, R, I
  MISC("am3offset", "kOperandTypeARMAddrMode3Offset");            // R, I
  MISC("addrmode4", "kOperandTypeARMAddrMode4");                  // R, I
  MISC("addrmode5", "kOperandTypeARMAddrMode5");                  // R, I
  MISC("addrmode6", "kOperandTypeARMAddrMode6");                  // R, R, I, I
  MISC("am6offset", "kOperandTypeARMAddrMode6Offset");            // R, I, I
  MISC("addrmodepc", "kOperandTypeARMAddrModePC");                // R, I
  MISC("reglist", "kOperandTypeARMRegisterList");                 // I, R, ...
  MISC("it_mask", "kOperandTypeThumbITMask");                     // I
  MISC("t2addrmode_imm8", "kOperandTypeThumb2AddrModeImm8");      // R, I
  MISC("t2am_imm8_offset", "kOperandTypeThumb2AddrModeImm8Offset");//I
  MISC("t2addrmode_imm12", "kOperandTypeThumb2AddrModeImm12");    // R, I
  MISC("t2addrmode_so_reg", "kOperandTypeThumb2AddrModeSoReg");   // R, R, I
  MISC("t2addrmode_imm8s4", "kOperandTypeThumb2AddrModeImm8s4");  // R, I
  MISC("t2am_imm8s4_offset", "kOperandTypeThumb2AddrModeImm8s4Offset");  
                                                                  // R, I
  MISC("tb_addrmode", "kOperandTypeARMTBAddrMode");               // I
  MISC("t_addrmode_s1", "kOperandTypeThumbAddrModeS1");           // R, I, R
  MISC("t_addrmode_s2", "kOperandTypeThumbAddrModeS2");           // R, I, R
  MISC("t_addrmode_s4", "kOperandTypeThumbAddrModeS4");           // R, I, R
  MISC("t_addrmode_rr", "kOperandTypeThumbAddrModeRR");           // R, R
  MISC("t_addrmode_sp", "kOperandTypeThumbAddrModeSP");           // R, I
  
  return 1;
}

#undef SOREG
#undef SOIMM
#undef PRED
#undef REG
#undef MEM
#undef LEA
#undef IMM
#undef PCR

#undef SET

/// ARMPopulateOperands - Handles all the operands in an ARM instruction, adding
///   the appropriate flags to their descriptors
///
/// @operandFlags - A reference the array of operand flag objects
/// @inst         - The instruction to use as a source of information
static void ARMPopulateOperands(
  LiteralConstantEmitter *(&operandTypes)[MAX_OPERANDS],
  const CodeGenInstruction &inst) {
  if (!inst.TheDef->isSubClassOf("InstARM") &&
      !inst.TheDef->isSubClassOf("InstThumb"))
    return;
  
  unsigned int index;
  unsigned int numOperands = inst.OperandList.size();
  
  if (numOperands > MAX_OPERANDS) {
    errs() << "numOperands == " << numOperands << " > " << MAX_OPERANDS << '\n';
    llvm_unreachable("Too many operands");
  }
  
  for (index = 0; index < numOperands; ++index) {
    const CodeGenInstruction::OperandInfo &operandInfo = 
    inst.OperandList[index];
    Record &rec = *operandInfo.Rec;
    
    if (ARMFlagFromOpName(operandTypes[index], rec.getName())) {
      errs() << "Operand type: " << rec.getName() << '\n';
      errs() << "Operand name: " << operandInfo.Name << '\n';
      errs() << "Instruction mame: " << inst.TheDef->getName() << '\n';
      llvm_unreachable("Unhandled type");
    }
  }
}

#define BRANCH(target) {                    \
  instType.set("kInstructionTypeBranch");   \
  DECORATE1(target, "kOperandFlagTarget");  \
}

/// ARMExtractSemantics - Performs various checks on the name of an ARM
///   instruction to determine what sort of an instruction it is and then adds 
///   the appropriate flags to the instruction and its operands
///
/// @arg instType     - A reference to the type for the instruction as a whole
/// @arg operandTypes - A reference to the array of operand type object pointers
/// @arg operandFlags - A reference to the array of operand flag object pointers
/// @arg inst         - A reference to the original instruction
static void ARMExtractSemantics(
  LiteralConstantEmitter &instType,
  LiteralConstantEmitter *(&operandTypes)[MAX_OPERANDS],
  FlagsConstantEmitter *(&operandFlags)[MAX_OPERANDS],
  const CodeGenInstruction &inst) {
  const std::string &name = inst.TheDef->getName();
  
  if (name == "tBcc"   ||
      name == "tB"     ||
      name == "t2Bcc"  ||
      name == "Bcc"    ||
      name == "tCBZ"   ||
      name == "tCBNZ") {
    BRANCH("target");
  }
  
  if (name == "tBLr9"      ||
      name == "BLr9_pred"  ||
      name == "tBLXi_r9"   ||
      name == "tBLXr_r9"   ||
      name == "BLXr9"      ||
      name == "t2BXJ"      ||
      name == "BXJ") {
    BRANCH("func");
    
    unsigned opIndex;
    opIndex = inst.getOperandNamed("func");
    if (operandTypes[opIndex]->is("kOperandTypeImmediate"))
      operandTypes[opIndex]->set("kOperandTypeARMBranchTarget");
  }
}

#undef BRANCH

/// populateInstInfo - Fills an array of InstInfos with information about each 
///   instruction in a target
///
/// @arg infoArray  - The array of InstInfo objects to populate
/// @arg target     - The CodeGenTarget to use as a source of instructions
static void populateInstInfo(CompoundConstantEmitter &infoArray,
                             CodeGenTarget &target) {
  const std::vector<const CodeGenInstruction*> &numberedInstructions =
    target.getInstructionsByEnumValue();
  
  unsigned int index;
  unsigned int numInstructions = numberedInstructions.size();
  
  for (index = 0; index < numInstructions; ++index) {
    const CodeGenInstruction& inst = *numberedInstructions[index];
    
    CompoundConstantEmitter *infoStruct = new CompoundConstantEmitter;
    infoArray.addEntry(infoStruct);
    
    LiteralConstantEmitter *instType = new LiteralConstantEmitter;
    infoStruct->addEntry(instType);
    
    LiteralConstantEmitter *numOperandsEmitter = 
      new LiteralConstantEmitter(inst.OperandList.size());
    infoStruct->addEntry(numOperandsEmitter);
    
    CompoundConstantEmitter *operandTypeArray = new CompoundConstantEmitter;
    infoStruct->addEntry(operandTypeArray);
    
    LiteralConstantEmitter *operandTypes[MAX_OPERANDS];
                         
    CompoundConstantEmitter *operandFlagArray = new CompoundConstantEmitter;
    infoStruct->addEntry(operandFlagArray);
        
    FlagsConstantEmitter *operandFlags[MAX_OPERANDS];
    
    for (unsigned operandIndex = 0; 
         operandIndex < MAX_OPERANDS; 
         ++operandIndex) {
      operandTypes[operandIndex] = new LiteralConstantEmitter;
      operandTypeArray->addEntry(operandTypes[operandIndex]);
      
      operandFlags[operandIndex] = new FlagsConstantEmitter;
      operandFlagArray->addEntry(operandFlags[operandIndex]);
    }
 
    unsigned numSyntaxes = 0;
    
    if (target.getName() == "X86") {
      X86PopulateOperands(operandTypes, inst);
      X86ExtractSemantics(*instType, operandFlags, inst);
      numSyntaxes = 2;
    }
    else if (target.getName() == "ARM") {
      ARMPopulateOperands(operandTypes, inst);
      ARMExtractSemantics(*instType, operandTypes, operandFlags, inst);
      numSyntaxes = 1;
    }
    
    CompoundConstantEmitter *operandOrderArray = new CompoundConstantEmitter;    
    
    infoStruct->addEntry(operandOrderArray);
    
    for (unsigned syntaxIndex = 0; syntaxIndex < MAX_SYNTAXES; ++syntaxIndex) {
      CompoundConstantEmitter *operandOrder = 
        new CompoundConstantEmitter(MAX_OPERANDS);
      
      operandOrderArray->addEntry(operandOrder);
      
      if (syntaxIndex < numSyntaxes) {
        populateOperandOrder(operandOrder, inst, syntaxIndex);
      }
    }
    
    infoStruct = NULL;
  }
}

void EDEmitter::run(raw_ostream &o) {
  unsigned int i = 0;
  
  CompoundConstantEmitter infoArray;
  CodeGenTarget target;
  
  populateInstInfo(infoArray, target);
  
  o << "InstInfo instInfo" << target.getName().c_str() << "[] = ";
  infoArray.emit(o, i);
  o << ";" << "\n";
}

void EDEmitter::runHeader(raw_ostream &o) {
  EmitSourceFileHeader("Enhanced Disassembly Info Header", o);
  
  o << "#ifndef EDInfo_" << "\n";
  o << "#define EDInfo_" << "\n";
  o << "\n";
  o << "#include <inttypes.h>" << "\n";
  o << "\n";
  o << "#define MAX_OPERANDS " << format("%d", MAX_OPERANDS) << "\n";
  o << "#define MAX_SYNTAXES " << format("%d", MAX_SYNTAXES) << "\n";
  o << "\n";
  
  unsigned int i = 0;
  
  EnumEmitter operandTypes("OperandTypes");
  operandTypes.addEntry("kOperandTypeNone");
  operandTypes.addEntry("kOperandTypeImmediate");
  operandTypes.addEntry("kOperandTypeRegister");
  operandTypes.addEntry("kOperandTypeX86Memory");
  operandTypes.addEntry("kOperandTypeX86EffectiveAddress");
  operandTypes.addEntry("kOperandTypeX86PCRelative");
  operandTypes.addEntry("kOperandTypeARMBranchTarget");
  operandTypes.addEntry("kOperandTypeARMSoReg");
  operandTypes.addEntry("kOperandTypeARMSoImm");
  operandTypes.addEntry("kOperandTypeARMSoImm2Part");
  operandTypes.addEntry("kOperandTypeARMPredicate");
  operandTypes.addEntry("kOperandTypeARMAddrMode2");
  operandTypes.addEntry("kOperandTypeARMAddrMode2Offset");
  operandTypes.addEntry("kOperandTypeARMAddrMode3");
  operandTypes.addEntry("kOperandTypeARMAddrMode3Offset");
  operandTypes.addEntry("kOperandTypeARMAddrMode4");
  operandTypes.addEntry("kOperandTypeARMAddrMode5");
  operandTypes.addEntry("kOperandTypeARMAddrMode6");
  operandTypes.addEntry("kOperandTypeARMAddrMode6Offset");
  operandTypes.addEntry("kOperandTypeARMAddrModePC");
  operandTypes.addEntry("kOperandTypeARMRegisterList");
  operandTypes.addEntry("kOperandTypeARMTBAddrMode");
  operandTypes.addEntry("kOperandTypeThumbITMask");
  operandTypes.addEntry("kOperandTypeThumbAddrModeS1");
  operandTypes.addEntry("kOperandTypeThumbAddrModeS2");
  operandTypes.addEntry("kOperandTypeThumbAddrModeS4");
  operandTypes.addEntry("kOperandTypeThumbAddrModeRR");
  operandTypes.addEntry("kOperandTypeThumbAddrModeSP");
  operandTypes.addEntry("kOperandTypeThumb2SoReg");
  operandTypes.addEntry("kOperandTypeThumb2SoImm");
  operandTypes.addEntry("kOperandTypeThumb2AddrModeImm8");
  operandTypes.addEntry("kOperandTypeThumb2AddrModeImm8Offset");
  operandTypes.addEntry("kOperandTypeThumb2AddrModeImm12");
  operandTypes.addEntry("kOperandTypeThumb2AddrModeSoReg");
  operandTypes.addEntry("kOperandTypeThumb2AddrModeImm8s4");
  operandTypes.addEntry("kOperandTypeThumb2AddrModeImm8s4Offset");
  
  operandTypes.emit(o, i);
  
  o << "\n";
  
  EnumEmitter operandFlags("OperandFlags");
  operandFlags.addEntry("kOperandFlagSource");
  operandFlags.addEntry("kOperandFlagTarget");
  operandFlags.emitAsFlags(o, i);
  
  o << "\n";
  
  EnumEmitter instructionTypes("InstructionTypes");
  instructionTypes.addEntry("kInstructionTypeNone");
  instructionTypes.addEntry("kInstructionTypeMove");
  instructionTypes.addEntry("kInstructionTypeBranch");
  instructionTypes.addEntry("kInstructionTypePush");
  instructionTypes.addEntry("kInstructionTypePop");
  instructionTypes.addEntry("kInstructionTypeCall");
  instructionTypes.addEntry("kInstructionTypeReturn");
  instructionTypes.emit(o, i);
  
  o << "\n";
  
  StructEmitter instInfo("InstInfo");
  instInfo.addMember("uint8_t", "instructionType");
  instInfo.addMember("uint8_t", "numOperands");
  instInfo.addMember("uint8_t", "operandTypes[MAX_OPERANDS]");
  instInfo.addMember("uint8_t", "operandFlags[MAX_OPERANDS]");
  instInfo.addMember("const char", "operandOrders[MAX_SYNTAXES][MAX_OPERANDS]");
  instInfo.emit(o, i);
  
  o << "\n";
  o << "#endif" << "\n";
}
