//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ObservationUtils.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

using namespace mlir;
using namespace circt;
using namespace llhd;

void ObservationTracker::addUsers(Value value) {
  for (auto *user : value.getUsers()) {
    if (user->hasTrait<OpTrait::IsTerminator>() || !isMemoryEffectFree(user))
      continue;
    auto processOp = user->getParentOfType<ProcessOp>();
    if (!processOp)
      continue;
    auto &region = processOp.getBody();
    if (auto *block = region.findAncestorBlockInRegion(*user->getBlock()))
      userBlocks[&region].insert(block);
  }
}

/// Collect the blocks at which execution resumes and from which `block` is
/// reached without suspending. These are the entry block and the destinations
/// of `llhd.wait` ops.
static void collectResumePoints(Block *block,
                                SmallPtrSetImpl<Block *> &resumePoints) {
  SmallPtrSet<Block *, 8> visited;
  SmallVector<Block *> worklist{block};
  while (!worklist.empty()) {
    auto *current = worklist.pop_back_val();
    if (!visited.insert(current).second)
      continue;
    if (current->isEntryBlock())
      resumePoints.insert(current);
    for (auto *predecessor : current->getPredecessors()) {
      if (isa<WaitOp>(predecessor->getTerminator()))
        resumePoints.insert(current);
      else
        worklist.push_back(predecessor);
    }
  }
}

bool ObservationTracker::observedAtMultipleTimes() {
  for (auto &[region, blocks] : userBlocks) {
    if (blocks.size() < 2)
      continue;
    SmallPtrSet<Block *, 4> resumePoints;
    for (auto *block : blocks) {
      collectResumePoints(block, resumePoints);
      if (resumePoints.size() > 1)
        return true;
    }
  }
  return false;
}
