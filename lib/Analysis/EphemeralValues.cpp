//===------------------------ EphemeralValues.cpp ------------------------===//
//              Code to perform Alignment Invariant Propagation
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements ephemeral value determination.
//
//===----------------------------------------------------------------------===//

#define EV_NAME "eph-values"
#define DEBUG_TYPE EV_NAME
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/EphemeralValues.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

char EphemeralValues::ID = 0;
static const char ev_name[] = "Ephemeral value analysis";
INITIALIZE_PASS_BEGIN(EphemeralValues, EV_NAME,
                ev_name, false, false)
INITIALIZE_PASS_END(EphemeralValues, EV_NAME,
                ev_name, false, false)

ModulePass *llvm::createEphemeralValuesPass() {
  return new EphemeralValues();
}

EphemeralValues::EphemeralValues() : ModulePass(ID) {
  initializeEphemeralValuesPass(*PassRegistry::getPassRegistry());
}

void EphemeralValues::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
}

void EphemeralValues::print(raw_ostream &OS, const Module *M) const {
  if (!M)
    return;

  OS << "Ephemeral values...\n";
  for (Module::const_iterator L = M->begin(), LE = M->end(); L != LE; ++L)
    for (Function::const_iterator I = L->begin(), IE = L->end(); I != IE; ++I)
      for (BasicBlock::const_iterator J = I->getFirstInsertionPt(),
           JE = I->end(); J != JE; ++J)
        if (isEphemeralValue(J)) {
          OS << "\tephemeral: " << L->getName() << ": " << I->getName() <<
                ": " << *J << "\n";
        }
}

bool EphemeralValues::runOnModule(Module &M) {
  DenseSet<Value *> Visited;
  SmallVector<Value *, 16> WorkSet;

  EphValues.clear();

  for (Module::iterator L = M.begin(), LE = M.end(); L != LE; ++L)
    for (Function::iterator I = L->begin(), IE = L->end(); I != IE; ++I)
      for (BasicBlock::iterator J = I->getFirstInsertionPt(), JE = I->end();
           J != JE; ++J)
        if (CallInst *CI = dyn_cast<CallInst>(J))
          if (Function *F2 = CI->getCalledFunction())
            if (F2->getIntrinsicID() == Intrinsic::invariant) {
              WorkSet.push_back(CI);
              EphValues.insert(CI);
            }

  while (!WorkSet.empty()) {
    Value *V = WorkSet.pop_back_val();
    if (!Visited.insert(V).second)
      continue;

    // If all uses of this value are ephemeral, then so is this value.
    bool FoundNEUse = false;
    for (Value::use_iterator I = V->use_begin(), IE = V->use_end();
         I != IE; ++I)
      if (!EphValues.count(*I)) {
        FoundNEUse = true;
        break;
      }

    if (!FoundNEUse) {
      EphValues.insert(V);

      if (User *U = dyn_cast<User>(V))
        for (User::op_iterator J = U->op_begin(), JE = U->op_end();
             J != JE; ++J)
          WorkSet.push_back(*J);
    }
  }

  return false;
}

