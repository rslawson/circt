//===- LowerGlobalSignals.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Replace LLHD global signals, such as variables declared in packages, with
// signals declared in the root modules of the instance hierarchy. References
// to the signals are passed down to the modules using them through additional
// input ports. Since arcilator flattens the hierarchy, all modules then access
// the same signal.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Arc/ArcPasses.h"
#include "circt/Dialect/HW/HWInstanceGraph.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SetVector.h"

namespace circt {
namespace arc {
#define GEN_PASS_DEF_LOWERGLOBALSIGNALS
#include "circt/Dialect/Arc/ArcPasses.h.inc"
} // namespace arc
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace arc;

namespace {
struct LowerGlobalSignalsPass
    : public arc::impl::LowerGlobalSignalsBase<LowerGlobalSignalsPass> {
  void runOnOperation() override;
};
} // namespace

void LowerGlobalSignalsPass::runOnOperation() {
  auto module = getOperation();
  DenseMap<StringAttr, llhd::GlobalSignalOp> globals;
  SmallVector<llhd::GlobalSignalOp> globalOrder;
  for (auto globalOp : module.getOps<llhd::GlobalSignalOp>()) {
    globals.insert({globalOp.getSymNameAttr(), globalOp});
    globalOrder.push_back(globalOp);
  }
  if (globals.empty())
    return;

  // Determine the global signals used by each module, either directly or
  // through its instances. Visit children before their parents.
  auto &instanceGraph = getAnalysis<hw::InstanceGraph>();
  DenseMap<Operation *, SmallSetVector<StringAttr, 4>> neededGlobals;
  module.walk([&](llhd::GetGlobalSignalOp op) {
    if (auto parent = op->getParentOfType<hw::HWModuleOp>())
      neededGlobals[parent].insert(op.getGlobalNameAttr().getAttr());
  });
  SmallVector<hw::HWModuleOp> modules;
  for (auto *node : llvm::post_order(&instanceGraph)) {
    auto moduleOp = dyn_cast_or_null<hw::HWModuleOp>(
        node->getModule().getOperation());
    if (!moduleOp)
      continue;
    modules.push_back(moduleOp);
    for (auto *record : *node) {
      auto *child = record->getTarget()->getModule().getOperation();
      auto it = neededGlobals.find(child);
      if (it == neededGlobals.end())
        continue;
      auto childGlobals = it->second;
      neededGlobals[moduleOp].insert(childGlobals.begin(), childGlobals.end());
    }
  }

  // Make the global signals available in each module that needs them. Root
  // modules, which are not instantiated anywhere, declare the signals. All
  // other modules receive references to them through additional ports, in the
  // order of the global signal declarations.
  DenseMap<Operation *, DenseMap<StringAttr, Value>> moduleValues;
  DenseMap<Operation *, SmallVector<StringAttr>> modulePorts;
  for (auto moduleOp : modules) {
    auto it = neededGlobals.find(moduleOp);
    if (it == neededGlobals.end())
      continue;
    auto &values = moduleValues[moduleOp];
    auto *node = instanceGraph.lookup(moduleOp);
    bool isRoot = llvm::none_of(
        node->uses(), [](auto *use) { return use->getInstance(); });
    OpBuilder builder = OpBuilder::atBlockBegin(moduleOp.getBodyBlock());
    for (auto globalOp : globalOrder) {
      auto name = globalOp.getSymNameAttr();
      if (!it->second.contains(name))
        continue;
      auto refType = llhd::RefType::get(globalOp.getType());
      if (!isRoot) {
        values[name] = moduleOp.appendInput(name.getValue(), refType).second;
        modulePorts[moduleOp].push_back(name);
        continue;
      }
      // Compute the initial value of the signal from the global's initializer.
      Value init;
      if (auto *initBlock = globalOp.getInitBlock()) {
        IRMapping mapping;
        for (auto &op : initBlock->without_terminator())
          builder.clone(op, mapping);
        init = mapping.lookupOrDefault(
            initBlock->getTerminator()->getOperand(0));
      }
      values[name] = llhd::SignalOp::create(builder, globalOp.getLoc(),
                                            refType, name, init);
    }
  }

  // Pass the signals to the instances of modules that received new ports.
  for (auto moduleOp : modules) {
    SmallVector<hw::InstanceOp> instances(
        moduleOp.getBodyBlock()->getOps<hw::InstanceOp>());
    moduleOp.walk([&](hw::InstanceOp instOp) {
      if (instOp->getParentOp() != moduleOp)
        instances.push_back(instOp);
    });
    for (auto instOp : instances) {
      auto *child = instanceGraph.lookup(instOp.getModuleNameAttr().getAttr())
                        ->getModule()
                        .getOperation();
      auto portsIt = modulePorts.find(child);
      if (portsIt == modulePorts.end())
        continue;
      SmallVector<Value> inputs(instOp.getInputs());
      for (auto name : portsIt->second)
        inputs.push_back(moduleValues[moduleOp][name]);
      OpBuilder builder(instOp);
      auto newInstOp = hw::InstanceOp::create(
          builder, instOp.getLoc(), child, instOp.getInstanceNameAttr(),
          inputs, instOp.getParameters(), instOp.getInnerSymAttr());
      instOp.replaceAllUsesWith(newInstOp.getResults());
      instOp.erase();
    }
  }

  // Replace the accesses of the global signals and remove the globals.
  SmallVector<llhd::GetGlobalSignalOp> getOps;
  module.walk([&](llhd::GetGlobalSignalOp op) { getOps.push_back(op); });
  for (auto op : getOps) {
    auto parent = op->getParentOfType<hw::HWModuleOp>();
    if (!parent)
      continue;
    op.replaceAllUsesWith(
        moduleValues[parent][op.getGlobalNameAttr().getAttr()]);
    op.erase();
  }
  for (auto globalOp : globalOrder)
    if (globalOp.symbolKnownUseEmpty(module))
      globalOp.erase();
}
