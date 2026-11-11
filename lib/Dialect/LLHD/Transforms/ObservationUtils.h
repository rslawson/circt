//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Helpers to reason about the points in time at which processes observe values
// defined outside of them.
//
// A value defined outside of a process, such as the probe of a signal, is
// observed at the time of its use in the process. If side-effect free ops in a
// process use such a value at different points in time, identical ops in
// different blocks may be merged by CSE, which changes the point in time at
// which the value is observed. Transformations that replace probes inside
// processes with values defined outside must therefore avoid introducing such
// uses.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_LLHD_TRANSFORMS_OBSERVATIONUTILS_H
#define CIRCT_DIALECT_LLHD_TRANSFORMS_OBSERVATIONUTILS_H

#include "circt/Support/LLVM.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/Region.h"
#include "mlir/IR/Value.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"

namespace circt {
namespace llhd {

/// Tracks the ops in processes that would observe a value defined outside of
/// them. Only side-effect free ops that are not terminators are tracked, since
/// only those can be merged by CSE.
class ObservationTracker {
public:
  /// Record the users of `value`.
  void addUsers(Value value);

  /// Check whether the recorded users in any process observe the value at
  /// more than one point in time. This is the case if they are in blocks that
  /// are reached from different points at which the process resumes
  /// execution, without suspending in between.
  bool observedAtMultipleTimes();

private:
  /// The blocks containing the users, grouped by the region of the process.
  SmallMapVector<Region *, SmallSetVector<Block *, 2>, 2> userBlocks;
};

} // namespace llhd
} // namespace circt

#endif // CIRCT_DIALECT_LLHD_TRANSFORMS_OBSERVATIONUTILS_H
