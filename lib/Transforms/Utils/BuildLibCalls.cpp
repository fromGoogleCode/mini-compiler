//===- BuildLibCalls.cpp - Utility builder for libcalls -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements some functions that will create standard C libcalls.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/BuildLibCalls.h"
#include "llvm/Type.h"
#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/Module.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Target/TargetData.h"
#include "llvm/LLVMContext.h"
#include "llvm/Intrinsics.h"

using namespace llvm;

/// CastToCStr - Return V if it is an i8*, otherwise cast it to i8*.
Value *llvm::CastToCStr(Value *V, IRBuilder<> &B) {
  return B.CreateBitCast(V, B.getInt8PtrTy(), "cstr");
}

/// EmitStrLen - Emit a call to the strlen function to the builder, for the
/// specified pointer.  This always returns an integer value of size intptr_t.
Value *llvm::EmitStrLen(Value *Ptr, IRBuilder<> &B, const TargetData *TD) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  AttributeWithIndex AWI[2];
  AWI[0] = AttributeWithIndex::get(1, Attribute::NoCapture);
  AWI[1] = AttributeWithIndex::get(~0u, Attribute::ReadOnly |
                                   Attribute::NoUnwind);

  LLVMContext &Context = B.GetInsertBlock()->getContext();
  Constant *StrLen = M->getOrInsertFunction("strlen", AttrListPtr::get(AWI, 2),
                                            TD->getIntPtrType(Context),
                                            B.getInt8PtrTy(),
                                            NULL);
  CallInst *CI = B.CreateCall(StrLen, CastToCStr(Ptr, B), "strlen");
  if (const Function *F = dyn_cast<Function>(StrLen->stripPointerCasts()))
    CI->setCallingConv(F->getCallingConv());

  return CI;
}

/// EmitStrChr - Emit a call to the strchr function to the builder, for the
/// specified pointer and character.  Ptr is required to be some pointer type,
/// and the return value has 'i8*' type.
Value *llvm::EmitStrChr(Value *Ptr, char C, IRBuilder<> &B,
                        const TargetData *TD) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  AttributeWithIndex AWI =
    AttributeWithIndex::get(~0u, Attribute::ReadOnly | Attribute::NoUnwind);

  const Type *I8Ptr = B.getInt8PtrTy();
  const Type *I32Ty = B.getInt32Ty();
  Constant *StrChr = M->getOrInsertFunction("strchr", AttrListPtr::get(&AWI, 1),
                                            I8Ptr, I8Ptr, I32Ty, NULL);
  CallInst *CI = B.CreateCall2(StrChr, CastToCStr(Ptr, B),
                               ConstantInt::get(I32Ty, C), "strchr");
  if (const Function *F = dyn_cast<Function>(StrChr->stripPointerCasts()))
    CI->setCallingConv(F->getCallingConv());
  return CI;
}

/// EmitStrCpy - Emit a call to the strcpy function to the builder, for the
/// specified pointer arguments.
Value *llvm::EmitStrCpy(Value *Dst, Value *Src, IRBuilder<> &B,
                        const TargetData *TD, StringRef Name) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  AttributeWithIndex AWI[2];
  AWI[0] = AttributeWithIndex::get(2, Attribute::NoCapture);
  AWI[1] = AttributeWithIndex::get(~0u, Attribute::NoUnwind);
  const Type *I8Ptr = B.getInt8PtrTy();
  Value *StrCpy = M->getOrInsertFunction(Name, AttrListPtr::get(AWI, 2),
                                         I8Ptr, I8Ptr, I8Ptr, NULL);
  CallInst *CI = B.CreateCall2(StrCpy, CastToCStr(Dst, B), CastToCStr(Src, B),
                               Name);
  if (const Function *F = dyn_cast<Function>(StrCpy->stripPointerCasts()))
    CI->setCallingConv(F->getCallingConv());
  return CI;
}

/// EmitStrNCpy - Emit a call to the strncpy function to the builder, for the
/// specified pointer arguments.
Value *llvm::EmitStrNCpy(Value *Dst, Value *Src, Value *Len,
                         IRBuilder<> &B, const TargetData *TD, StringRef Name) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  AttributeWithIndex AWI[2];
  AWI[0] = AttributeWithIndex::get(2, Attribute::NoCapture);
  AWI[1] = AttributeWithIndex::get(~0u, Attribute::NoUnwind);
  const Type *I8Ptr = B.getInt8PtrTy();
  Value *StrNCpy = M->getOrInsertFunction(Name, AttrListPtr::get(AWI, 2),
                                          I8Ptr, I8Ptr, I8Ptr,
                                          Len->getType(), NULL);
  CallInst *CI = B.CreateCall3(StrNCpy, CastToCStr(Dst, B), CastToCStr(Src, B),
                               Len, "strncpy");
  if (const Function *F = dyn_cast<Function>(StrNCpy->stripPointerCasts()))
    CI->setCallingConv(F->getCallingConv());
  return CI;
}


/// EmitMemCpy - Emit a call to the memcpy function to the builder.  This always
/// expects that Len has type 'intptr_t' and Dst/Src are pointers.
Value *llvm::EmitMemCpy(Value *Dst, Value *Src, Value *Len, unsigned Align,
                        bool isVolatile, IRBuilder<> &B, const TargetData *TD) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  const Type *ArgTys[3] = { Dst->getType(), Src->getType(), Len->getType() };
  Value *MemCpy = Intrinsic::getDeclaration(M, Intrinsic::memcpy, ArgTys, 3);
  Dst = CastToCStr(Dst, B);
  Src = CastToCStr(Src, B);
  return B.CreateCall5(MemCpy, Dst, Src, Len,
                       ConstantInt::get(B.getInt32Ty(), Align),
                       ConstantInt::get(B.getInt1Ty(), isVolatile));
}

/// EmitMemCpyChk - Emit a call to the __memcpy_chk function to the builder.
/// This expects that the Len and ObjSize have type 'intptr_t' and Dst/Src
/// are pointers.
Value *llvm::EmitMemCpyChk(Value *Dst, Value *Src, Value *Len, Value *ObjSize,
                           IRBuilder<> &B, const TargetData *TD) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  AttributeWithIndex AWI;
  AWI = AttributeWithIndex::get(~0u, Attribute::NoUnwind);
  LLVMContext &Context = B.GetInsertBlock()->getContext();
  Value *MemCpy = M->getOrInsertFunction("__memcpy_chk",
                                         AttrListPtr::get(&AWI, 1),
                                         B.getInt8PtrTy(),
                                         B.getInt8PtrTy(),
                                         B.getInt8PtrTy(),
                                         TD->getIntPtrType(Context),
                                         TD->getIntPtrType(Context), NULL);
  Dst = CastToCStr(Dst, B);
  Src = CastToCStr(Src, B);
  CallInst *CI = B.CreateCall4(MemCpy, Dst, Src, Len, ObjSize);
  if (const Function *F = dyn_cast<Function>(MemCpy->stripPointerCasts()))
    CI->setCallingConv(F->getCallingConv());
  return CI;
}

/// EmitMemMove - Emit a call to the memmove function to the builder.  This
/// always expects that the size has type 'intptr_t' and Dst/Src are pointers.
Value *llvm::EmitMemMove(Value *Dst, Value *Src, Value *Len, unsigned Align,
                         bool isVolatile, IRBuilder<> &B, const TargetData *TD) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  LLVMContext &Context = B.GetInsertBlock()->getContext();
  const Type *ArgTys[3] = { Dst->getType(), Src->getType(),
                            TD->getIntPtrType(Context) };
  Value *MemMove = Intrinsic::getDeclaration(M, Intrinsic::memmove, ArgTys, 3);
  Dst = CastToCStr(Dst, B);
  Src = CastToCStr(Src, B);
  Value *A = ConstantInt::get(B.getInt32Ty(), Align);
  Value *Vol = ConstantInt::get(B.getInt1Ty(), isVolatile);
  return B.CreateCall5(MemMove, Dst, Src, Len, A, Vol);
}

/// EmitMemChr - Emit a call to the memchr function.  This assumes that Ptr is
/// a pointer, Val is an i32 value, and Len is an 'intptr_t' value.
Value *llvm::EmitMemChr(Value *Ptr, Value *Val,
                        Value *Len, IRBuilder<> &B, const TargetData *TD) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  AttributeWithIndex AWI;
  AWI = AttributeWithIndex::get(~0u, Attribute::ReadOnly | Attribute::NoUnwind);
  LLVMContext &Context = B.GetInsertBlock()->getContext();
  Value *MemChr = M->getOrInsertFunction("memchr", AttrListPtr::get(&AWI, 1),
                                         B.getInt8PtrTy(),
                                         B.getInt8PtrTy(),
                                         B.getInt32Ty(),
                                         TD->getIntPtrType(Context),
                                         NULL);
  CallInst *CI = B.CreateCall3(MemChr, CastToCStr(Ptr, B), Val, Len, "memchr");

  if (const Function *F = dyn_cast<Function>(MemChr->stripPointerCasts()))
    CI->setCallingConv(F->getCallingConv());

  return CI;
}

/// EmitMemCmp - Emit a call to the memcmp function.
Value *llvm::EmitMemCmp(Value *Ptr1, Value *Ptr2,
                        Value *Len, IRBuilder<> &B, const TargetData *TD) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  AttributeWithIndex AWI[3];
  AWI[0] = AttributeWithIndex::get(1, Attribute::NoCapture);
  AWI[1] = AttributeWithIndex::get(2, Attribute::NoCapture);
  AWI[2] = AttributeWithIndex::get(~0u, Attribute::ReadOnly |
                                   Attribute::NoUnwind);

  LLVMContext &Context = B.GetInsertBlock()->getContext();
  Value *MemCmp = M->getOrInsertFunction("memcmp", AttrListPtr::get(AWI, 3),
                                         B.getInt32Ty(),
                                         B.getInt8PtrTy(),
                                         B.getInt8PtrTy(),
                                         TD->getIntPtrType(Context), NULL);
  CallInst *CI = B.CreateCall3(MemCmp, CastToCStr(Ptr1, B), CastToCStr(Ptr2, B),
                               Len, "memcmp");

  if (const Function *F = dyn_cast<Function>(MemCmp->stripPointerCasts()))
    CI->setCallingConv(F->getCallingConv());

  return CI;
}

/// EmitMemSet - Emit a call to the memset function
Value *llvm::EmitMemSet(Value *Dst, Value *Val, Value *Len, bool isVolatile,
                        IRBuilder<> &B, const TargetData *TD) {
 Module *M = B.GetInsertBlock()->getParent()->getParent();
 Intrinsic::ID IID = Intrinsic::memset;
 const Type *Tys[2] = { Dst->getType(), Len->getType() };
 Value *MemSet = Intrinsic::getDeclaration(M, IID, Tys, 2);
 Value *Align = ConstantInt::get(B.getInt32Ty(), 1);
 Value *Vol = ConstantInt::get(B.getInt1Ty(), isVolatile);
 return B.CreateCall5(MemSet, CastToCStr(Dst, B), Val, Len, Align, Vol);
}

/// EmitUnaryFloatFnCall - Emit a call to the unary function named 'Name' (e.g.
/// 'floor').  This function is known to take a single of type matching 'Op' and
/// returns one value with the same type.  If 'Op' is a long double, 'l' is
/// added as the suffix of name, if 'Op' is a float, we add a 'f' suffix.
Value *llvm::EmitUnaryFloatFnCall(Value *Op, const char *Name,
                                  IRBuilder<> &B, const AttrListPtr &Attrs) {
  char NameBuffer[20];
  if (!Op->getType()->isDoubleTy()) {
    // If we need to add a suffix, copy into NameBuffer.
    unsigned NameLen = strlen(Name);
    assert(NameLen < sizeof(NameBuffer)-2);
    memcpy(NameBuffer, Name, NameLen);
    if (Op->getType()->isFloatTy())
      NameBuffer[NameLen] = 'f';  // floorf
    else
      NameBuffer[NameLen] = 'l';  // floorl
    NameBuffer[NameLen+1] = 0;
    Name = NameBuffer;
  }

  Module *M = B.GetInsertBlock()->getParent()->getParent();
  Value *Callee = M->getOrInsertFunction(Name, Op->getType(),
                                         Op->getType(), NULL);
  CallInst *CI = B.CreateCall(Callee, Op, Name);
  CI->setAttributes(Attrs);
  if (const Function *F = dyn_cast<Function>(Callee->stripPointerCasts()))
    CI->setCallingConv(F->getCallingConv());

  return CI;
}

/// EmitPutChar - Emit a call to the putchar function.  This assumes that Char
/// is an integer.
Value *llvm::EmitPutChar(Value *Char, IRBuilder<> &B, const TargetData *TD) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  Value *PutChar = M->getOrInsertFunction("putchar", B.getInt32Ty(),
                                          B.getInt32Ty(), NULL);
  CallInst *CI = B.CreateCall(PutChar,
                              B.CreateIntCast(Char,
                              B.getInt32Ty(),
                              /*isSigned*/true,
                              "chari"),
                              "putchar");

  if (const Function *F = dyn_cast<Function>(PutChar->stripPointerCasts()))
    CI->setCallingConv(F->getCallingConv());
  return CI;
}

/// EmitPutS - Emit a call to the puts function.  This assumes that Str is
/// some pointer.
void llvm::EmitPutS(Value *Str, IRBuilder<> &B, const TargetData *TD) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  AttributeWithIndex AWI[2];
  AWI[0] = AttributeWithIndex::get(1, Attribute::NoCapture);
  AWI[1] = AttributeWithIndex::get(~0u, Attribute::NoUnwind);

  Value *PutS = M->getOrInsertFunction("puts", AttrListPtr::get(AWI, 2),
                                       B.getInt32Ty(),
                                       B.getInt8PtrTy(),
                                       NULL);
  CallInst *CI = B.CreateCall(PutS, CastToCStr(Str, B), "puts");
  if (const Function *F = dyn_cast<Function>(PutS->stripPointerCasts()))
    CI->setCallingConv(F->getCallingConv());

}

/// EmitFPutC - Emit a call to the fputc function.  This assumes that Char is
/// an integer and File is a pointer to FILE.
void llvm::EmitFPutC(Value *Char, Value *File, IRBuilder<> &B,
                     const TargetData *TD) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  AttributeWithIndex AWI[2];
  AWI[0] = AttributeWithIndex::get(2, Attribute::NoCapture);
  AWI[1] = AttributeWithIndex::get(~0u, Attribute::NoUnwind);
  Constant *F;
  if (File->getType()->isPointerTy())
    F = M->getOrInsertFunction("fputc", AttrListPtr::get(AWI, 2),
                               B.getInt32Ty(),
                               B.getInt32Ty(), File->getType(),
                               NULL);
  else
    F = M->getOrInsertFunction("fputc",
                               B.getInt32Ty(),
                               B.getInt32Ty(),
                               File->getType(), NULL);
  Char = B.CreateIntCast(Char, B.getInt32Ty(), /*isSigned*/true,
                         "chari");
  CallInst *CI = B.CreateCall2(F, Char, File, "fputc");

  if (const Function *Fn = dyn_cast<Function>(F->stripPointerCasts()))
    CI->setCallingConv(Fn->getCallingConv());
}

/// EmitFPutS - Emit a call to the puts function.  Str is required to be a
/// pointer and File is a pointer to FILE.
void llvm::EmitFPutS(Value *Str, Value *File, IRBuilder<> &B,
                     const TargetData *TD) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  AttributeWithIndex AWI[3];
  AWI[0] = AttributeWithIndex::get(1, Attribute::NoCapture);
  AWI[1] = AttributeWithIndex::get(2, Attribute::NoCapture);
  AWI[2] = AttributeWithIndex::get(~0u, Attribute::NoUnwind);
  Constant *F;
  if (File->getType()->isPointerTy())
    F = M->getOrInsertFunction("fputs", AttrListPtr::get(AWI, 3),
                               B.getInt32Ty(),
                               B.getInt8PtrTy(),
                               File->getType(), NULL);
  else
    F = M->getOrInsertFunction("fputs", B.getInt32Ty(),
                               B.getInt8PtrTy(),
                               File->getType(), NULL);
  CallInst *CI = B.CreateCall2(F, CastToCStr(Str, B), File, "fputs");

  if (const Function *Fn = dyn_cast<Function>(F->stripPointerCasts()))
    CI->setCallingConv(Fn->getCallingConv());
}

/// EmitFWrite - Emit a call to the fwrite function.  This assumes that Ptr is
/// a pointer, Size is an 'intptr_t', and File is a pointer to FILE.
void llvm::EmitFWrite(Value *Ptr, Value *Size, Value *File,
                      IRBuilder<> &B, const TargetData *TD) {
  Module *M = B.GetInsertBlock()->getParent()->getParent();
  AttributeWithIndex AWI[3];
  AWI[0] = AttributeWithIndex::get(1, Attribute::NoCapture);
  AWI[1] = AttributeWithIndex::get(4, Attribute::NoCapture);
  AWI[2] = AttributeWithIndex::get(~0u, Attribute::NoUnwind);
  LLVMContext &Context = B.GetInsertBlock()->getContext();
  Constant *F;
  if (File->getType()->isPointerTy())
    F = M->getOrInsertFunction("fwrite", AttrListPtr::get(AWI, 3),
                               TD->getIntPtrType(Context),
                               B.getInt8PtrTy(),
                               TD->getIntPtrType(Context),
                               TD->getIntPtrType(Context),
                               File->getType(), NULL);
  else
    F = M->getOrInsertFunction("fwrite", TD->getIntPtrType(Context),
                               B.getInt8PtrTy(),
                               TD->getIntPtrType(Context),
                               TD->getIntPtrType(Context),
                               File->getType(), NULL);
  CallInst *CI = B.CreateCall4(F, CastToCStr(Ptr, B), Size,
                        ConstantInt::get(TD->getIntPtrType(Context), 1), File);

  if (const Function *Fn = dyn_cast<Function>(F->stripPointerCasts()))
    CI->setCallingConv(Fn->getCallingConv());
}

SimplifyFortifiedLibCalls::~SimplifyFortifiedLibCalls() { }

bool SimplifyFortifiedLibCalls::fold(CallInst *CI, const TargetData *TD) {
  this->CI = CI;
  StringRef Name = CI->getCalledFunction()->getName();
  BasicBlock *BB = CI->getParent();
  IRBuilder<> B(CI->getParent()->getContext());

  // Set the builder to the instruction after the call.
  B.SetInsertPoint(BB, CI);

  if (Name == "__memcpy_chk") {
    if (isFoldable(4, 3, false)) {
      EmitMemCpy(CI->getOperand(1), CI->getOperand(2), CI->getOperand(3),
                 1, false, B, TD);
      replaceCall(CI->getOperand(1));
      return true;
    }
    return false;
  }

  // Should be similar to memcpy.
  if (Name == "__mempcpy_chk") {
    return false;
  }

  if (Name == "__memmove_chk") {
    if (isFoldable(4, 3, false)) {
      EmitMemMove(CI->getOperand(1), CI->getOperand(2), CI->getOperand(3),
                  1, false, B, TD);
      replaceCall(CI->getOperand(1));
      return true;
    }
    return false;
  }

  if (Name == "__memset_chk") {
    if (isFoldable(4, 3, false)) {
      Value *Val = B.CreateIntCast(CI->getOperand(2), B.getInt8Ty(),
                                   false);
      EmitMemSet(CI->getOperand(1), Val,  CI->getOperand(3), false, B, TD);
      replaceCall(CI->getOperand(1));
      return true;
    }
    return false;
  }

  if (Name == "__strcpy_chk" || Name == "__stpcpy_chk") {
    // If a) we don't have any length information, or b) we know this will
    // fit then just lower to a plain st[rp]cpy. Otherwise we'll keep our
    // st[rp]cpy_chk call which may fail at runtime if the size is too long.
    // TODO: It might be nice to get a maximum length out of the possible
    // string lengths for varying.
    if (isFoldable(3, 2, true)) {
      Value *Ret = EmitStrCpy(CI->getOperand(1), CI->getOperand(2), B, TD,
                              Name.substr(2, 6));
      replaceCall(Ret);
      return true;
    }
    return false;
  }

  if (Name == "__strncpy_chk" || Name == "__stpncpy_chk") {
    if (isFoldable(4, 3, false)) {
      Value *Ret = EmitStrNCpy(CI->getOperand(1), CI->getOperand(2),
                               CI->getOperand(3), B, TD, Name.substr(2, 7));
      replaceCall(Ret);
      return true;
    }
    return false;
  }

  if (Name == "__strcat_chk") {
    return false;
  }

  if (Name == "__strncat_chk") {
    return false;
  }

  return false;
}
