//===------------------------ AlignmentInvProp.cpp ------------------------===//
//              Code to perform Alignment Invariant Propagation
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements alignment invariant propagation.
//
//===----------------------------------------------------------------------===//

#define AA_NAME "alignment-inv-prop"
#define DEBUG_TYPE AA_NAME
#include "llvm/Transforms/Scalar.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Pass.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/PostOrderIterator.h"
using namespace llvm;

STATISTIC(NumLoadAlignChanged,
  "Number of loads changed by alignment assumptions");
STATISTIC(NumStoreAlignChanged,
  "Number of stores changed by alignment assumptions");
STATISTIC(NumMemIntAlignChanged,
  "Number of memory intrinsics changed by alignment assumptions");

namespace {
  struct AlignmentInvProp : public FunctionPass {
    static char ID; // Pass identification, replacement for typeid
    AlignmentInvProp() : FunctionPass(ID) {
      initializeAlignmentInvPropPass(*PassRegistry::getPassRegistry());
    }

    bool runOnFunction(Function &F);

    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.setPreservesCFG();
      AU.addRequired<ScalarEvolution>();
    }
  };
}

char AlignmentInvProp::ID = 0;
static const char aip_name[] = "Alignment invariant propagation";
INITIALIZE_PASS_BEGIN(AlignmentInvProp, AA_NAME,
                aip_name, false, false)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
INITIALIZE_PASS_END(AlignmentInvProp, AA_NAME,
                aip_name, false, false)

FunctionPass *llvm::createAlignmentInvPropPass() {
  return new AlignmentInvProp();
}

static unsigned getNewAlignmentDiff(const SCEV *DiffSCEV,
                                    const SCEV *AlignSCEV,
                                    ScalarEvolution *SE) {
  // DiffUnits = Diff % int64_t(Alignment)
  const SCEV *DiffAlignDiv = SE->getUDivExpr(DiffSCEV, AlignSCEV);
  const SCEV *DiffAlign = SE->getMulExpr(DiffAlignDiv, AlignSCEV);
  const SCEV *DiffUnitsSCEV = SE->getMinusSCEV(DiffAlign, DiffSCEV);

  DEBUG(dbgs() << "\talignment relative to " << *AlignSCEV << " is " <<
                  *DiffUnitsSCEV << " (diff: " << *DiffSCEV << ")\n");

  if (const SCEVConstant *ConstDUSCEV =
      dyn_cast<SCEVConstant>(DiffUnitsSCEV)) {
    int64_t DiffUnits = ConstDUSCEV->getValue()->getSExtValue();

    if (!DiffUnits)
      return (unsigned)
        cast<SCEVConstant>(AlignSCEV)->getValue()->getSExtValue();

    uint64_t DiffUnitsAbs = abs64(DiffUnits);
    if (isPowerOf2_64(DiffUnitsAbs))
      return (unsigned) DiffUnitsAbs;
  }

  return 0;
}

static unsigned getNewAlignment(const SCEV *AASCEV, const SCEV *AlignSCEV,
                                const SCEV *OffSCEV, Value *Ptr,
                                ScalarEvolution *SE) {
  const SCEV *PtrSCEV = SE->getSCEV(Ptr);
  const SCEV *DiffSCEV = SE->getMinusSCEV(PtrSCEV, AASCEV);

  // What we really want to know if the overall offset to the aligned
  // address. This address is displaced by the provided offset.
  DiffSCEV = SE->getMinusSCEV(DiffSCEV, OffSCEV);

  DEBUG(dbgs() << AA_NAME ": alignment of " << *Ptr << " relative to " <<
                  *AlignSCEV << " and offset " << *OffSCEV <<
                  " using diff " << *DiffSCEV << "\n");

  unsigned NewAlignment = getNewAlignmentDiff(DiffSCEV, AlignSCEV, SE);
  DEBUG(dbgs() << "\tnew alignment: " << NewAlignment << "\n");

  if (NewAlignment) {
    return NewAlignment;
  } else if (const SCEVAddRecExpr *DiffARSCEV =
             dyn_cast<SCEVAddRecExpr>(DiffSCEV)) {
    // The relative offset to the alignment assumption did not yield a constant,
    // but we should try harder: if we assume that a is 32-byte aligned, then in
    // for (i = 0; i < 1024; i += 4) r += a[i]; not all of the loads from a are
    // 32-byte aligned, but instead alternate between 32 and 16-byte alignment.
    // As a result, the new alignment will not be a constant, but can still
    // be improved over the default (of 4) to 16.

    const SCEV *DiffStartSCEV = DiffARSCEV->getStart();
    const SCEV *DiffIncSCEV = DiffARSCEV->getStepRecurrence(*SE);

    DEBUG(dbgs() << "\ttrying start/inc alignment using start " <<
                    *DiffStartSCEV << " and inc " << *DiffIncSCEV << "\n");

    NewAlignment = getNewAlignmentDiff(DiffStartSCEV, AlignSCEV, SE);
    unsigned NewIncAlignment = getNewAlignmentDiff(DiffIncSCEV, AlignSCEV, SE);

    DEBUG(dbgs() << "\tnew start alignment: " << NewAlignment << "\n");
    DEBUG(dbgs() << "\tnew inc alignment: " << NewIncAlignment << "\n");

	///HACK: what's going on here? investigate.
	if(!NewAlignment)
		return 0;

    if ( NewAlignment > NewIncAlignment) {
      if (NewAlignment % NewIncAlignment == 0) {
        DEBUG(dbgs() << "\tnew start/inc alignment: " <<
                        NewIncAlignment << "\n");
        return NewIncAlignment;
      }
    } else if (NewIncAlignment > NewAlignment) {
      if (NewIncAlignment % NewAlignment == 0) {
        DEBUG(dbgs() << "\tnew start/inc alignment: " <<
                        NewAlignment << "\n");
        return NewAlignment;
      }
    } else if (NewIncAlignment == NewAlignment && NewIncAlignment) {
      DEBUG(dbgs() << "\tnew start/inc alignment: " <<
                      NewAlignment << "\n");
      return NewAlignment;
    }
  }

  return 0;
}

bool AlignmentInvProp::runOnFunction(Function &F) {
  SmallVector<Value *, 4> InvConds;
  BasicBlock *EntryBB = F.begin();
  for (df_iterator<BasicBlock *> I = df_begin(EntryBB), IE = df_end(EntryBB);
       I != IE; ++I)
    for (BasicBlock::iterator J = I->getFirstInsertionPt(), JE = I->end();
         J != JE; ++J)
      if (CallInst *CI = dyn_cast<CallInst>(J))
        if (Function *F2 = CI->getCalledFunction())
          if (F2->getIntrinsicID() == Intrinsic::invariant)
            InvConds.push_back(CI->getArgOperand(0));

  // Visit all invariant conditions, and split those that are ands of
  // other conditions.
  DenseSet<Value *> VisitedInvCond;
  while (VisitedInvCond.size() != InvConds.size()) {
    SmallVector<Value *, 4> NewInvConds;
    for (SmallVector<Value*, 4>::iterator I = InvConds.begin(),
         IE = InvConds.end(); I != IE; ++I) {
      if (BinaryOperator *BO = dyn_cast<BinaryOperator>(*I)) {
        if (BO->getOpcode() == Instruction::And) {
          NewInvConds.push_back(BO->getOperand(0));
          NewInvConds.push_back(BO->getOperand(1));
        }
      }

      VisitedInvCond.insert(*I);
    }

    InvConds.insert(InvConds.end(), NewInvConds.begin(), NewInvConds.end());
    NewInvConds.clear();
  }

  bool Changed = false;
  ScalarEvolution *SE = &getAnalysis<ScalarEvolution>();

  DenseMap<MemTransferInst *, unsigned> NewDestAlignments, NewSrcAlignments;

  for (SmallVector<Value *, 4>::iterator I = InvConds.begin(),
       IE = InvConds.end(); I != IE; ++I) {
    // An alignment invariant must be a statement about the least-significant
    // bits of the pointer being zero, possibly with some offset.
    ICmpInst *ICI = dyn_cast<ICmpInst>(*I);
    if (!ICI)
      continue;

    // This must be an expression of the form: x & m == 0.
    if (ICI->getPredicate() != ICmpInst::ICMP_EQ)
      continue;

    Value *CmpLHS = ICI->getOperand(0);
    Value *CmpRHS = ICI->getOperand(1);
    const SCEV *CmpLHSSCEV = SE->getSCEV(CmpLHS);
    const SCEV *CmpRHSSCEV = SE->getSCEV(CmpRHS);
    if (CmpLHSSCEV->isZero())
      std::swap(CmpLHS, CmpRHS);
    else if (!CmpRHSSCEV->isZero())
      continue;

    BinaryOperator *CmpBO = dyn_cast<BinaryOperator>(CmpLHS);
    if (!CmpBO || CmpBO->getOpcode() != Instruction::And)
      continue;

    Value *AndLHS = CmpBO->getOperand(0);
    Value *AndRHS = CmpBO->getOperand(1);
    const SCEV *AndLHSSCEV = SE->getSCEV(AndLHS);
    const SCEV *AndRHSSCEV = SE->getSCEV(AndRHS);
    if (isa<SCEVConstant>(AndLHSSCEV)) {
      std::swap(AndLHS, AndRHS);
      std::swap(AndLHSSCEV, AndRHSSCEV);
    }

    const SCEVConstant *MaskSCEV = dyn_cast<SCEVConstant>(AndRHSSCEV);
    if (!MaskSCEV)
      continue;

    unsigned TrailingOnes =
      MaskSCEV->getValue()->getValue().countTrailingOnes();
    if (!TrailingOnes)
      continue;

    uint64_t Alignment;
    TrailingOnes = std::min(TrailingOnes,
      unsigned(sizeof(unsigned) * CHAR_BIT - 1));
    Alignment = std::min(1u << TrailingOnes, +Value::MaximumAlignment);

    Type *Int64Ty = Type::getInt64Ty(F.getContext());
    const SCEV *AlignSCEV = SE->getConstant(Int64Ty, Alignment);

    // The LHS might be a ptrtoint instruction, or it might be the pointer
    // with an offset.
    Value *AAPtr = 0;
    const SCEV *OffSCEV = 0;
    if (PtrToIntInst *PToI = dyn_cast<PtrToIntInst>(AndLHS)) {
      AAPtr = PToI->getPointerOperand();
      OffSCEV = SE->getConstant(Int64Ty, 0);
    } else if (const SCEVAddExpr* AndLHSAddSCEV =
               dyn_cast<SCEVAddExpr>(AndLHSSCEV)) {
      // Try to find the ptrtoint; subtract it and the rest is the offset.
      for (SCEVAddExpr::op_iterator J = AndLHSAddSCEV->op_begin(),
           JE = AndLHSAddSCEV->op_end(); J != JE; ++J)
        if (const SCEVUnknown *OpUnk = dyn_cast<SCEVUnknown>(*J))
          if (PtrToIntInst *PToI = dyn_cast<PtrToIntInst>(OpUnk->getValue())) {
            AAPtr = PToI->getPointerOperand();
            OffSCEV = SE->getMinusSCEV(AndLHSAddSCEV, *J);
            break;
          }
    }

    if (!AAPtr)
      continue;
    
    unsigned OffSCEVBits = OffSCEV->getType()->getPrimitiveSizeInBits();
    if (OffSCEVBits < 64)
      OffSCEV = SE->getSignExtendExpr(OffSCEV, Int64Ty);
    else if (OffSCEVBits > 64)
      continue;

    AAPtr = AAPtr->stripPointerCasts();
    const SCEV *AASCEV = SE->getSCEV(AAPtr);

    // Apply the assumption to all other users of the specified pointer.
    DenseSet<Instruction *> Visited;
    SmallVector<Instruction*, 16> WorkList;
    for (Value::use_iterator J = AAPtr->use_begin(),
         JE = AAPtr->use_end(); J != JE; ++J) {
      if (*J == *I)
        continue;

      if (Instruction *K = dyn_cast<Instruction>(*J))
        WorkList.push_back(K);
    }

    while (!WorkList.empty()) {
      Instruction *J = WorkList.pop_back_val();

      if (LoadInst *LI = dyn_cast<LoadInst>(J)) {
        unsigned NewAlignment = getNewAlignment(AASCEV, AlignSCEV, OffSCEV,
          LI->getPointerOperand(), SE);

        if (NewAlignment > LI->getAlignment()) {
          LI->setAlignment(NewAlignment);
          ++NumLoadAlignChanged;
          Changed = true;
        }
      } else if (StoreInst *SI = dyn_cast<StoreInst>(J)) {
        unsigned NewAlignment = getNewAlignment(AASCEV, AlignSCEV, OffSCEV,
          SI->getPointerOperand(), SE);

        if (NewAlignment > SI->getAlignment()) {
          SI->setAlignment(NewAlignment);
          ++NumStoreAlignChanged;
          Changed = true;
        }
      } else if (MemIntrinsic *MI = dyn_cast<MemIntrinsic>(J)) {
        unsigned NewDestAlignment = getNewAlignment(AASCEV, AlignSCEV, OffSCEV,
          MI->getDest(), SE);

        // For memory transfers, we need a common alignment for both the
        // source and destination. If we have a new alignment for this
        // instruction, but only for one operand, save it. If we reach the
        // other operand through another assumption later, then we may
        // change the alignment at that point.
        if (MemTransferInst *MTI = dyn_cast<MemTransferInst>(MI)) {
          unsigned NewSrcAlignment = getNewAlignment(AASCEV, AlignSCEV, OffSCEV,
            MTI->getSource(), SE);

          DenseMap<MemTransferInst *, unsigned>::iterator DI =
            NewDestAlignments.find(MTI);
          unsigned AltDestAlignment = (DI == NewDestAlignments.end()) ?
                                      0 : DI->second;

          DenseMap<MemTransferInst *, unsigned>::iterator SI =
            NewSrcAlignments.find(MTI);
          unsigned AltSrcAlignment = (SI == NewSrcAlignments.end()) ?
                                     0 : SI->second;

          DEBUG(dbgs() << "\tmem trans: " << NewDestAlignment << " " <<
                          AltDestAlignment << " " << NewSrcAlignment <<
                          " " << AltSrcAlignment << "\n");

          // If these four alignments, pick the largest possible...
          unsigned NewAlignment = 0;
          if (NewDestAlignment <= NewSrcAlignment ||
              NewDestAlignment <= AltSrcAlignment)
            NewAlignment = std::max(NewAlignment, NewDestAlignment);
          if (AltDestAlignment <= NewSrcAlignment ||
              AltDestAlignment <= AltSrcAlignment)
            NewAlignment = std::max(NewAlignment, AltDestAlignment);
          if (NewSrcAlignment <= NewDestAlignment ||
              NewSrcAlignment <= AltDestAlignment)
            NewAlignment = std::max(NewAlignment, NewSrcAlignment);
          if (AltSrcAlignment <= NewDestAlignment ||
              AltSrcAlignment <= AltDestAlignment)
            NewAlignment = std::max(NewAlignment, AltSrcAlignment);

          if (NewAlignment > MI->getAlignment()) {
            MI->setAlignment(ConstantInt::get(Type::getInt32Ty(
              MI->getParent()->getContext()), NewAlignment));
            ++NumMemIntAlignChanged;
            Changed = true;
          }

          NewDestAlignments.insert(std::make_pair(MTI, NewDestAlignment));
          NewSrcAlignments.insert(std::make_pair(MTI, NewSrcAlignment));
        } else if (NewDestAlignment > MI->getAlignment()) {
          MI->setAlignment(ConstantInt::get(Type::getInt32Ty(
            MI->getParent()->getContext()), NewDestAlignment));
          ++NumMemIntAlignChanged;
          Changed = true;
        }
      }

      Visited.insert(J);
      for (Value::use_iterator UJ = J->use_begin(), UE = J->use_end();
           UJ != UE; ++UJ) {
        Instruction *K = cast<Instruction>(*UJ);
        if (!Visited.count(K))
          WorkList.push_back(cast<Instruction>(*UJ));
      }
    }

    Changed = true;
  }

  return Changed;
}

