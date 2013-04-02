//===- EphemeralValues.h - Ephemeral value analysis -------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Calculate ephemeral values - those used only (indirectly) by invariants.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_EPHEMERAL_VALUES_H
#define LLVM_ANALYSIS_EPHEMERAL_VALUES_H

#include "llvm/ADT/DenseSet.h"
#include "llvm/Pass.h"

namespace llvm {

class Value;
class raw_ostream;

//===----------------------------------------------------------------------===//
/// @brief Analysis that finds ephemeral values. 
class EphemeralValues : public ModulePass {
  DenseSet<Value *> EphValues;

  EphemeralValues(const EphemeralValues &) LLVM_DELETED_FUNCTION;
  const EphemeralValues &operator=(const EphemeralValues &) LLVM_DELETED_FUNCTION;

public:
  // Returns true if the provided value is ephemeral.
  bool isEphemeralValue(Value *V) const {
    return EphValues.count(V);
  }
  bool isEphemeralValue(const Value *V) const {
    return isEphemeralValue(const_cast<Value *>(V));
  }

public:
  static char ID;
  explicit EphemeralValues();

  /// @name ModulePass interface
  //@{
  virtual bool runOnModule(Module &M);
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;
  virtual void print(raw_ostream &OS, const Module *M) const;
  //@}
};

} // End llvm namespace
#endif

