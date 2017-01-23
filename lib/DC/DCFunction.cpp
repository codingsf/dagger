//===-- lib/DC/DCFunction.cpp - Function Translation ------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DC/DCFunction.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/DC/DCRegisterSema.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/MC/MCAnalysis/MCFunction.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "dc-sema"

static cl::opt<bool> EnableRegSetDiff("enable-dc-regset-diff", cl::desc(""),
                                      cl::init(false));

static cl::opt<bool> EnableInstAddrSave("enable-dc-pc-save", cl::desc(""),
                                        cl::init(false));

static cl::opt<bool> TranslateUnknownToUndef(
    "dc-translate-unknown-to-undef",
    cl::desc("Translate unknown instruction or unknown opcode in an "
             "instruction's semantics with undef+unreachable. If false, "
             "abort."),
    cl::init(false));

DCFunction::DCFunction(DCModule &DCM, const MCFunction &MCF,
                       const unsigned *OpcodeToSemaIdx,
                       const unsigned *SemanticsArray,
                       const uint64_t *ConstantArray, DCRegisterSema &DRS)
    : OpcodeToSemaIdx(OpcodeToSemaIdx), SemanticsArray(SemanticsArray),
      ConstantArray(ConstantArray), DCM(DCM), DRS(DRS),
      TheFunction(*DCM.getOrCreateFunction(MCF.getStartAddr())),
      TheMCFunction(MCF), BBByAddr(), ExitBB(0), CallBBs(), TheBB(0),
      TheMCBB(0), Builder(new DCIRBuilder(getContext())), Idx(0), ResEVT(),
      Opcode(0), Vals(), CurrentInst(0) {

  assert(!MCF.empty() && "Trying to translate empty MC function");
  const uint64_t StartAddr = MCF.getStartAddr();

  assert(getFunction()->empty() && "Translating into non-empty function!");

  getFunction()->setDoesNotAlias(1);
  getFunction()->setDoesNotCapture(1);

  // Create the entry and exit basic blocks.
  TheBB = BasicBlock::Create(getContext(), "entry_fn_" + utohexstr(StartAddr),
                             getFunction());
  ExitBB = BasicBlock::Create(getContext(), "exit_fn_" + utohexstr(StartAddr),
                              getFunction());

  // From now on we insert in the entry basic block.
  Builder->SetInsertPoint(TheBB);

  if (EnableRegSetDiff) {
    Value *SavedRegSet = Builder->CreateAlloca(DRS.getRegSetType());
    Value *RegSetArg = &getFunction()->getArgumentList().front();

    // First, save the previous regset in the entry block.
    Builder->CreateStore(Builder->CreateLoad(RegSetArg), SavedRegSet);

    // Second, insert a call to the diff function, in a separate exit block.
    // Move the return to that block, and branch to it from ExitBB.
    BasicBlock *DiffExitBB = BasicBlock::Create(
        getContext(), "diff_exit_fn_" + utohexstr(StartAddr), getFunction());

    DCIRBuilder ExitBBBuilder(DiffExitBB);

    Value *FnAddr = ExitBBBuilder.CreateIntToPtr(
        ExitBBBuilder.getInt64(reinterpret_cast<uint64_t>(StartAddr)),
        ExitBBBuilder.getInt8PtrTy());

    ExitBBBuilder.CreateCall(DRS.getOrCreateRegSetDiffFunction(),
                             {FnAddr, SavedRegSet, RegSetArg});
    ReturnInst::Create(getContext(), DiffExitBB);
    BranchInst::Create(DiffExitBB, ExitBB);
  } else {
    // Create a ret void in the exit basic block.
    ReturnInst::Create(getContext(), ExitBB);
  }

  // Create a br from the entry basic block to the first basic block, at
  // StartAddr.
  Builder->CreateBr(getOrCreateBasicBlock(StartAddr));
}

DCFunction::~DCFunction() {
  for (auto *CallBB : CallBBs) {
    assert(CallBB->size() == 2 &&
           "Call basic block has wrong number of instructions!");
    auto CallI = CallBB->begin();
    DRS.saveAllLocalRegs(CallBB, CallI);
    DRS.restoreLocalRegs(CallBB, ++CallI);
  }

  DRS.FinalizeFunction(ExitBB);
}

void DCFunction::FinalizeBasicBlock() {
  if (!TheBB->getTerminator())
    BranchInst::Create(getOrCreateBasicBlock(getBasicBlockEndAddress()), TheBB);
  DRS.FinalizeBasicBlock();
  TheBB = nullptr;
  TheMCBB = nullptr;
}

void DCFunction::createExternalTailCallBB(uint64_t Addr) {
  // First create a basic block for the tail call.
  SwitchToBasicBlock(Addr);
  // Now do the call to that function.
  insertCallBB(DCM.getOrCreateFunction(Addr));
  // FIXME: should this still insert a regset diffing call?
  // Finally, return directly, bypassing the ExitBB.
  Builder->CreateRetVoid();
}

extern "C" uintptr_t __llvm_dc_current_fn = 0;
extern "C" uintptr_t __llvm_dc_current_bb = 0;
extern "C" uintptr_t __llvm_dc_current_instr = 0;

void DCFunction::prepareBasicBlockForInsertion(BasicBlock *BB) {
  assert((BB->size() == 2 && isa<UnreachableInst>(std::next(BB->begin()))) &&
         "Several BBs at the same address?");
  BB->begin()->eraseFromParent();
  BB->begin()->eraseFromParent();
}

void DCFunction::SwitchToBasicBlock(const MCBasicBlock *MCBB) {
  TheMCBB = MCBB;
  SwitchToBasicBlock(getBasicBlockStartAddress());
}

void DCFunction::SwitchToBasicBlock(uint64_t BeginAddr) {
  TheBB = getOrCreateBasicBlock(BeginAddr);
  prepareBasicBlockForInsertion(TheBB);

  Builder->SetInsertPoint(TheBB);

  DRS.SwitchToBasicBlock(TheBB);
  // FIXME: we need to keep the unreachable+trap when the basic block is 0-inst.

  // The PC at the start of the basic block is known, just set it.
  unsigned PC = DRS.MRI.getProgramCounter();
  setReg(PC, ConstantInt::get(DRS.getRegType(PC), BeginAddr));
}

uint64_t DCFunction::getBasicBlockStartAddress() const {
  assert(TheMCBB && "Getting start address without an MC BasicBlock");
  return TheMCBB->getStartAddr();
}

uint64_t DCFunction::getBasicBlockEndAddress() const {
  assert(TheMCBB && "Getting end address without an MC BasicBlock");
  return TheMCBB->getEndAddr();
}

BasicBlock *DCFunction::getOrCreateBasicBlock(uint64_t Addr) {
  BasicBlock *&BB = BBByAddr[Addr];
  if (!BB) {
    BB = BasicBlock::Create(getContext(), "bb_" + utohexstr(Addr),
                            getFunction());
    DCIRBuilder BBBuilder(BB);
    BBBuilder.CreateCall(
        Intrinsic::getDeclaration(getModule(), Intrinsic::trap));
    BBBuilder.CreateUnreachable();
  }
  return BB;
}

BasicBlock *DCFunction::insertCallBB(Value *Target) {
  BasicBlock *CallBB = BasicBlock::Create(
      getContext(), TheBB->getName() + "_call", getFunction());
  Value *RegSetArg = &getFunction()->getArgumentList().front();
  DCIRBuilder CallBuilder(CallBB);
  CallBuilder.CreateCall(Target, {RegSetArg});
  Builder->CreateBr(CallBB);
  assert(Builder->GetInsertPoint() == TheBB->end() &&
         "Call basic blocks can't be inserted at the middle of a basic block!");
  StringRef BBName = TheBB->getName();
  BBName = BBName.substr(0, BBName.find_first_of("_c"));
  std::string CallInstAddr = CurrentInst ? utohexstr(CurrentInst->Address) : "";
  TheBB = BasicBlock::Create(getContext(), BBName + "_c" + CallInstAddr,
                             getFunction());
  DRS.FinalizeBasicBlock();
  DRS.SwitchToBasicBlock(TheBB);
  Builder->SetInsertPoint(TheBB);
  CallBuilder.CreateBr(TheBB);
  CallBBs.push_back(CallBB);
  // FIXME: Insert return address checking, to unwind back to the translator if
  // the call returned to an unexpected address.
  return CallBB;
}

Value *DCFunction::insertTranslateAt(Value *OrigTarget) {
  Value *Ptr = Builder->CreateCall(
      Intrinsic::getDeclaration(getModule(), Intrinsic::dc_translate_at),
      {Builder->CreateIntToPtr(OrigTarget, Builder->getInt8PtrTy())});
  return Builder->CreateBitCast(Ptr, DCM.getFuncTy()->getPointerTo());
}

void DCFunction::insertCall(Value *CallTarget) {
  if (ConstantInt *CI = dyn_cast<ConstantInt>(CallTarget)) {
    uint64_t Target = CI->getValue().getZExtValue();
    CallTarget = DCM.getOrCreateFunction(Target);
  } else {
    CallTarget = insertTranslateAt(CallTarget);
  }
  insertCallBB(CallTarget);
}

void DCFunction::translateBinOp(Instruction::BinaryOps Opc) {
  Value *V1 = getNextOperand();
  Value *V2 = getNextOperand();
  if (Instruction::isShift(Opc) && V2->getType() != V1->getType())
    V2 = Builder->CreateZExt(V2, V1->getType());
  registerResult(Builder->CreateBinOp(Opc, V1, V2));
}

void DCFunction::translateCastOp(Instruction::CastOps Opc) {
  Type *ResType = ResEVT.getTypeForEVT(getContext());
  Value *Val = getNextOperand();
  registerResult(Builder->CreateCast(Opc, Val, ResType));
}

bool DCFunction::translateInst(const MCDecodedInst &DecodedInst) {
  CurrentInst = &DecodedInst;
  DRS.SwitchToInst(DecodedInst);

  if (EnableInstAddrSave) {
    ConstantInt *CurIVal =
        Builder->getInt64(reinterpret_cast<uint64_t>(CurrentInst->Address));
    Value *CurIPtr = ConstantExpr::getIntToPtr(
        Builder->getInt64(reinterpret_cast<uint64_t>(&__llvm_dc_current_instr)),
        Builder->getInt64Ty()->getPointerTo());
    Builder->CreateStore(CurIVal, CurIPtr, true);
  }

  bool Success = tryTranslateInst();

  if (!Success && TranslateUnknownToUndef) {
    errs() << "Couldn't translate instruction: \n  ";
    errs() << "  " << DRS.MII.getName(CurrentInst->Inst.getOpcode()) << ": "
           << CurrentInst->Inst << "\n";
    Builder->CreateCall(
        Intrinsic::getDeclaration(getModule(), Intrinsic::trap));
    Builder->CreateUnreachable();
    Success = true;
  }

  Vals.clear();
  CurrentInst = nullptr;
  return Success;
}

bool DCFunction::tryTranslateInst() {
  if (translateTargetInst())
    return true;

  Idx = OpcodeToSemaIdx[CurrentInst->Inst.getOpcode()];
  if (Idx == ~0U)
    return false;

  {
    // Increment the PC before anything.
    Value *OldPC = getReg(DRS.MRI.getProgramCounter());
    setReg(DRS.MRI.getProgramCounter(),
           Builder->CreateAdd(
               OldPC, ConstantInt::get(OldPC->getType(), CurrentInst->Size)));
  }

  while ((Opcode = Next()) != DCINS::END_OF_INSTRUCTION)
    if (!translateOpcode(Opcode))
      return false;

  return true;
}

bool DCFunction::translateOpcode(unsigned Opcode) {
  ResEVT = NextVT();
  if (Opcode >= ISD::BUILTIN_OP_END && Opcode < DCINS::DC_OPCODE_START)
    return translateTargetOpcode(Opcode);

  switch (Opcode) {
  case ISD::ADD:
    translateBinOp(Instruction::Add);
    break;
  case ISD::FADD:
    translateBinOp(Instruction::FAdd);
    break;
  case ISD::SUB:
    translateBinOp(Instruction::Sub);
    break;
  case ISD::FSUB:
    translateBinOp(Instruction::FSub);
    break;
  case ISD::MUL:
    translateBinOp(Instruction::Mul);
    break;
  case ISD::FMUL:
    translateBinOp(Instruction::FMul);
    break;
  case ISD::UDIV:
    translateBinOp(Instruction::UDiv);
    break;
  case ISD::SDIV:
    translateBinOp(Instruction::SDiv);
    break;
  case ISD::FDIV:
    translateBinOp(Instruction::FDiv);
    break;
  case ISD::UREM:
    translateBinOp(Instruction::URem);
    break;
  case ISD::SREM:
    translateBinOp(Instruction::SRem);
    break;
  case ISD::FREM:
    translateBinOp(Instruction::FRem);
    break;
  case ISD::SHL:
    translateBinOp(Instruction::Shl);
    break;
  case ISD::SRL:
    translateBinOp(Instruction::LShr);
    break;
  case ISD::SRA:
    translateBinOp(Instruction::AShr);
    break;
  case ISD::AND:
    translateBinOp(Instruction::And);
    break;
  case ISD::OR:
    translateBinOp(Instruction::Or);
    break;
  case ISD::XOR:
    translateBinOp(Instruction::Xor);
    break;

  case ISD::TRUNCATE:
    translateCastOp(Instruction::Trunc);
    break;
  case ISD::BITCAST:
    translateCastOp(Instruction::BitCast);
    break;
  case ISD::ZERO_EXTEND:
    translateCastOp(Instruction::ZExt);
    break;
  case ISD::SIGN_EXTEND:
    translateCastOp(Instruction::SExt);
    break;
  case ISD::FP_TO_UINT:
    translateCastOp(Instruction::FPToUI);
    break;
  case ISD::FP_TO_SINT:
    translateCastOp(Instruction::FPToSI);
    break;
  case ISD::UINT_TO_FP:
    translateCastOp(Instruction::UIToFP);
    break;
  case ISD::SINT_TO_FP:
    translateCastOp(Instruction::SIToFP);
    break;
  case ISD::FP_ROUND:
    translateCastOp(Instruction::FPTrunc);
    break;
  case ISD::FP_EXTEND:
    translateCastOp(Instruction::FPExt);
    break;

  case ISD::FSQRT: {
    Value *V = getNextOperand();
    registerResult(Builder->CreateCall(
        Intrinsic::getDeclaration(getModule(), Intrinsic::sqrt, V->getType()),
        {V}));
    break;
  }

  case ISD::ROTL: {
    Value *LHS = getNextOperand();
    Type *Ty = LHS->getType();
    assert(Ty->isIntegerTy());
    Value *RHS = Builder->CreateZExt(getNextOperand(), Ty);
    // FIXME: RHS needs to be tweaked to avoid undefined results.
    Value *Shl = Builder->CreateShl(LHS, RHS);
    registerResult(Builder->CreateOr(
        Shl,
        Builder->CreateLShr(
            LHS, Builder->CreateSub(
                     ConstantInt::get(Ty, Ty->getScalarSizeInBits()), RHS))));
    break;
  }

  case ISD::INSERT_VECTOR_ELT: {
    Value *Vec = getNextOperand();
    Value *Val = getNextOperand();
    Value *Idx = getNextOperand();
    registerResult(Builder->CreateInsertElement(Vec, Val, Idx));
    break;
  }

  case ISD::EXTRACT_VECTOR_ELT: {
    Value *Val = getNextOperand();
    Value *Idx = getNextOperand();
    registerResult(Builder->CreateExtractElement(Val, Idx));
    break;
  }

  case ISD::SMUL_LOHI: {
    EVT Re2EVT = NextVT();
    IntegerType *LoResType =
        cast<IntegerType>(ResEVT.getTypeForEVT(getContext()));
    IntegerType *HiResType =
        cast<IntegerType>(Re2EVT.getTypeForEVT(getContext()));
    IntegerType *ResType = IntegerType::get(
        getContext(), LoResType->getBitWidth() + HiResType->getBitWidth());
    Value *Op1 = Builder->CreateSExt(getNextOperand(), ResType);
    Value *Op2 = Builder->CreateSExt(getNextOperand(), ResType);
    Value *Full = Builder->CreateMul(Op1, Op2);
    registerResult(Builder->CreateTrunc(Full, LoResType));
    registerResult(Builder->CreateTrunc(
        Builder->CreateLShr(Full, LoResType->getBitWidth()), HiResType));
    break;
  }
  case ISD::UMUL_LOHI: {
    EVT Re2EVT = NextVT();
    IntegerType *LoResType =
        cast<IntegerType>(ResEVT.getTypeForEVT(getContext()));
    IntegerType *HiResType =
        cast<IntegerType>(Re2EVT.getTypeForEVT(getContext()));
    IntegerType *ResType = IntegerType::get(
        getContext(), LoResType->getBitWidth() + HiResType->getBitWidth());
    Value *Op1 = Builder->CreateZExt(getNextOperand(), ResType);
    Value *Op2 = Builder->CreateZExt(getNextOperand(), ResType);
    Value *Full = Builder->CreateMul(Op1, Op2);
    registerResult(Builder->CreateTrunc(Full, LoResType));
    registerResult(Builder->CreateTrunc(
        Builder->CreateLShr(Full, LoResType->getBitWidth()), HiResType));
    break;
  }
  case ISD::LOAD: {
    Type *ResPtrTy = ResEVT.getTypeForEVT(getContext())->getPointerTo();
    Value *Ptr = getNextOperand();
    if (!Ptr->getType()->isPointerTy())
      Ptr = Builder->CreateIntToPtr(Ptr, ResPtrTy);
    else if (Ptr->getType() != ResPtrTy)
      Ptr = Builder->CreateBitCast(Ptr, ResPtrTy);
    registerResult(Builder->CreateAlignedLoad(Ptr, 1));
    break;
  }
  case ISD::STORE: {
    Value *Val = getNextOperand();
    Value *Ptr = getNextOperand();
    Type *ValPtrTy = Val->getType()->getPointerTo();
    Type *PtrTy = Ptr->getType();
    if (!PtrTy->isPointerTy())
      Ptr = Builder->CreateIntToPtr(Ptr, ValPtrTy);
    else if (PtrTy != ValPtrTy)
      Ptr = Builder->CreateBitCast(Ptr, ValPtrTy);
    Builder->CreateAlignedStore(Val, Ptr, 1);
    break;
  }
  case ISD::BRIND: {
    Value *Op1 = getNextOperand();
    setReg(DRS.MRI.getProgramCounter(), Op1);
    insertCall(Op1);
    Builder->CreateBr(ExitBB);
    break;
  }
  case ISD::BR: {
    Value *Op1 = getNextOperand();
    uint64_t Target = cast<ConstantInt>(Op1)->getValue().getZExtValue();
    setReg(DRS.MRI.getProgramCounter(), Op1);
    Builder->CreateBr(getOrCreateBasicBlock(Target));
    break;
  }
  case ISD::TRAP: {
    Builder->CreateCall(
        Intrinsic::getDeclaration(getModule(), Intrinsic::trap));
    break;
  }
  case DCINS::PUT_RC: {
    unsigned MIOperandNo = Next();
    unsigned RegNo = getRegOp(MIOperandNo);
    Value *Res = getNextOperand();
    IntegerType *RegType = DRS.getRegIntType(RegNo);
    if (Res->getType()->isPointerTy())
      Res = Builder->CreatePtrToInt(Res, RegType);
    if (!Res->getType()->isIntegerTy())
      Res = Builder->CreateBitCast(
          Res, IntegerType::get(getContext(),
                                Res->getType()->getPrimitiveSizeInBits()));
    if (Res->getType()->getPrimitiveSizeInBits() < RegType->getBitWidth())
      Res = DRS.insertBitsInValue(DRS.getRegAsInt(RegNo), Res);
    assert(Res->getType() == RegType);
    setReg(RegNo, Res);
    break;
  }
  case DCINS::PUT_REG: {
    unsigned RegNo = Next();
    Value *Res = getNextOperand();
    setReg(RegNo, Res);
    break;
  }
  case DCINS::GET_RC: {
    unsigned MIOperandNo = Next();
    Type *ResType = ResEVT.getTypeForEVT(getContext());
    Value *Reg = DRS.getRegAsInt(getRegOp(MIOperandNo));
    if (ResType->getPrimitiveSizeInBits() <
        Reg->getType()->getPrimitiveSizeInBits())
      Reg = Builder->CreateTrunc(
          Reg,
          IntegerType::get(getContext(), ResType->getPrimitiveSizeInBits()));
    if (!ResType->isIntegerTy())
      Reg = Builder->CreateBitCast(Reg, ResType);
    registerResult(Reg);
    break;
  }
  case DCINS::GET_REG: {
    unsigned RegNo = Next();
    Value *RegVal = getReg(RegNo);
    registerResult(RegVal);
    break;
  }
  case DCINS::CUSTOM_OP: {
    unsigned OperandType = Next(), MIOperandNo = Next();
    Value *Op = translateCustomOperand(OperandType, MIOperandNo);
    if (!Op)
      return false;
    registerResult(Op);
    break;
  }
  case DCINS::COMPLEX_PATTERN: {
    unsigned Pattern = Next();
    Value *Op = translateComplexPattern(Pattern);
    if (!Op)
      return false;
    registerResult(Op);
    break;
  }
  case DCINS::PREDICATE: {
    unsigned Pred = Next();
    if (!translatePredicate(Pred))
      return false;
    break;
  }
  case DCINS::CONSTANT_OP: {
    unsigned MIOperandNo = Next();
    Type *ResType = ResEVT.getTypeForEVT(getContext());
    Value *Cst =
        ConstantInt::get(cast<IntegerType>(ResType), getImmOp(MIOperandNo));
    registerResult(Cst);
    break;
  }
  case DCINS::MOV_CONSTANT: {
    uint64_t ValIdx = Next();
    Type *ResType = nullptr;
    if (ResEVT.getSimpleVT() == MVT::iPTR)
      // FIXME: what should we do here? Maybe use DL's intptr type?
      ResType = Builder->getInt64Ty();
    else
      ResType = ResEVT.getTypeForEVT(getContext());
    registerResult(ConstantInt::get(ResType, ConstantArray[ValIdx]));
    break;
  }
  case DCINS::IMPLICIT: {
    translateImplicit(Next());
    break;
  }
  case ISD::BSWAP: {
    Type *ResType = ResEVT.getTypeForEVT(getContext());
    Value *Op = getNextOperand();
    Value *IntDecl =
        Intrinsic::getDeclaration(getModule(), Intrinsic::bswap, ResType);
    registerResult(Builder->CreateCall(IntDecl, Op));
    break;
  }

  case ISD::ATOMIC_FENCE: {
    uint64_t OrdV = cast<ConstantInt>(getNextOperand())->getZExtValue();
    uint64_t ScopeV = cast<ConstantInt>(getNextOperand())->getZExtValue();

    if (OrdV <= (uint64_t)AtomicOrdering::NotAtomic ||
        OrdV > (uint64_t)AtomicOrdering::SequentiallyConsistent)
      llvm_unreachable("Invalid atomic ordering");
    if (ScopeV != (uint64_t)SingleThread && ScopeV != (uint64_t)CrossThread)
      llvm_unreachable("Invalid synchronization scope");
    const AtomicOrdering Ord = (AtomicOrdering)OrdV;
    const SynchronizationScope Scope = (SynchronizationScope)ScopeV;

    Builder->CreateFence(Ord, Scope);
    break;
  }

  default:
    errs() << "Couldn't translate opcode for instruction: \n  ";
    errs() << "  " << DRS.MII.getName(CurrentInst->Inst.getOpcode()) << ": "
           << CurrentInst->Inst << "\n";
    errs() << "Opcode: " << Opcode << "\n";
    return false;
  }
  return true;
}

Value *DCFunction::translateComplexPattern(unsigned Pattern) {
  (void)Pattern;
  return nullptr;
}

bool DCFunction::translateExtLoad(Type *MemTy, bool isSExt) {
  Value *Ptr = getNextOperand();
  Ptr = Builder->CreateBitOrPointerCast(Ptr, MemTy->getPointerTo());
  Value *V = Builder->CreateLoad(MemTy, Ptr);
  Type *ResType = ResEVT.getTypeForEVT(getContext());
  registerResult(isSExt ? Builder->CreateSExt(V, ResType)
                        : Builder->CreateZExt(V, ResType));
  return true;
}

bool DCFunction::translatePredicate(unsigned Pred) {
  switch (Pred) {
  case TargetOpcode::Predicate::memop:
  case TargetOpcode::Predicate::loadi16:
  case TargetOpcode::Predicate::loadi32:
  case TargetOpcode::Predicate::alignedload:
  case TargetOpcode::Predicate::alignedload256:
  case TargetOpcode::Predicate::alignedload512:
  // FIXME: Take advantage of the implied alignment.
  case TargetOpcode::Predicate::load: {
    Type *ResPtrTy = ResEVT.getTypeForEVT(getContext())->getPointerTo();
    Value *Ptr = getNextOperand();
    if (!Ptr->getType()->isPointerTy())
      Ptr = Builder->CreateIntToPtr(Ptr, ResPtrTy);
    else if (Ptr->getType() != ResPtrTy)
      Ptr = Builder->CreateBitCast(Ptr, ResPtrTy);
    registerResult(Builder->CreateAlignedLoad(Ptr, 1));
    return true;
  }
  case TargetOpcode::Predicate::alignednontemporalstore:
  case TargetOpcode::Predicate::nontemporalstore:
  case TargetOpcode::Predicate::alignedstore:
  case TargetOpcode::Predicate::alignedstore256:
  case TargetOpcode::Predicate::alignedstore512:
  // FIXME: Take advantage of NT/alignment.
  case TargetOpcode::Predicate::store: {
    Value *Val = getNextOperand();
    Value *Ptr = getNextOperand();
    Type *ValPtrTy = Val->getType()->getPointerTo();
    Type *PtrTy = Ptr->getType();
    if (!PtrTy->isPointerTy())
      Ptr = Builder->CreateIntToPtr(Ptr, ValPtrTy);
    else if (PtrTy != ValPtrTy)
      Ptr = Builder->CreateBitCast(Ptr, ValPtrTy);
    Builder->CreateAlignedStore(Val, Ptr, 1);
    return true;
  }
  case TargetOpcode::Predicate::zextloadi8:
    return translateExtLoad(Builder->getInt8Ty());
  case TargetOpcode::Predicate::zextloadi16:
    return translateExtLoad(Builder->getInt16Ty());
  case TargetOpcode::Predicate::sextloadi8:
    return translateExtLoad(Builder->getInt8Ty(), /*isSExt=*/true);
  case TargetOpcode::Predicate::sextloadi16:
    return translateExtLoad(Builder->getInt16Ty(), /*isSExt=*/true);
  case TargetOpcode::Predicate::sextloadi32:
    return translateExtLoad(Builder->getInt32Ty(), /*isSExt=*/true);

  case TargetOpcode::Predicate::and_su: {
    translateBinOp(Instruction::And);
    return true;
  }
  }
  return false;
}
