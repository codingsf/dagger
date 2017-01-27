//===-- AArch64DCInstruction.h - AArch64 DCInstruction ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AARCH64_AARCH64DCINSTRUCTION_H
#define LLVM_LIB_TARGET_AARCH64_AARCH64DCINSTRUCTION_H

#include "AArch64DCBasicBlock.h"
#include "AArch64RegisterSema.h"
#include "llvm/DC/DCInstruction.h"

namespace llvm {

class AArch64DCInstruction final : public DCInstruction {
public:
  AArch64DCInstruction(DCBasicBlock &DCB, const MCDecodedInst &MCI);

  bool translateTargetInst(unsigned &InstOpcode) override;
  bool translateTargetOpcode(unsigned Opcode) override;
  Value *translateComplexPattern(unsigned CP) override;
  Value *translateCustomOperand(unsigned OperandType,
                                unsigned MIOperandNo) override;
  bool translateImplicit(unsigned RegNo) override;

  AArch64DCBasicBlock &getParent() {
    return static_cast<AArch64DCBasicBlock &>(DCInstruction::getParent());
  }

private:
  AArch64RegisterSema &getDRS() {
    return static_cast<AArch64RegisterSema &>(DCInstruction::getDRS());
  }
};

} // end llvm namespace

#endif
