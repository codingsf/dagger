; RUN: llc -enable-ipra -print-regusage -o /dev/null 2>&1 < %s | FileCheck %s

; XXX: Disabled because of CtlSysEFLAGS.
; XFAIL: *

target triple = "x86_64-unknown-unknown"
declare void @bar1()
define preserve_allcc void @foo()#0 {
; CHECK: foo Clobbered Registers: CS DS EFLAGS EIP EIZ ES FPSW FS GS IP RIP RIZ SS BND0 BND1 BND2 BND3 CR0 CR1 CR2 CR3 CR4 CR5 CR6 CR7 CR8 CR9 CR10 CR11 CR12 CR13 CR14 CR15 DR0 DR1 DR2 DR3 DR4 DR5 DR6 DR7 DR8 DR9 DR10 DR11 DR12 DR13 DR14 DR15 FP0 FP1 FP2 FP3 FP4 FP5 FP6 FP7 K0 K1 K2 K3 K4 K5 K6 K7 MM0 MM1 MM2 MM3 MM4 MM5 MM6 MM7 R11 ST0 ST1 ST2 ST3 ST4 ST5 ST6 ST7 XMM16 XMM17 XMM18 XMM19 XMM20 XMM21 XMM22 XMM23 XMM24 XMM25 XMM26 XMM27 XMM28 XMM29 XMM30 XMM31 YMM0 YMM1 YMM2 YMM3 YMM4 YMM5 YMM6 YMM7 YMM8 YMM9 YMM10 YMM11 YMM12 YMM13 YMM14 YMM15 YMM16 YMM17 YMM18 YMM19 YMM20 YMM21 YMM22 YMM23 YMM24 YMM25 YMM26 YMM27 YMM28 YMM29 YMM30 YMM31 ZMM0 ZMM1 ZMM2 ZMM3 ZMM4 ZMM5 ZMM6 ZMM7 ZMM8 ZMM9 ZMM10 ZMM11 ZMM12 ZMM13 ZMM14 ZMM15 ZMM16 ZMM17 ZMM18 ZMM19 ZMM20 ZMM21 ZMM22 ZMM23 ZMM24 ZMM25 ZMM26 ZMM27 ZMM28 ZMM29 ZMM30 ZMM31 R11B R11D R11W
  call void @bar1()
  call void @bar2()
  ret void
}
declare void @bar2()
attributes #0 = {nounwind}
