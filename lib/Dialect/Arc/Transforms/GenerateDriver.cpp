//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass generates a self-contained simulation driver for every `arc.model`
// in the module, so that the model can be run directly through the arcilator
// JIT without a hand-written driver loop.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Arc/ArcOps.h"
#include "circt/Dialect/Arc/ArcPasses.h"
#include "circt/Dialect/Arc/ArcTypes.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"

namespace circt {
namespace arc {
#define GEN_PASS_DEF_GENERATEDRIVER
#include "circt/Dialect/Arc/ArcPasses.h.inc"
} // namespace arc
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace arc;

//===----------------------------------------------------------------------===//
// Generate Simulation Drivers
//===----------------------------------------------------------------------===//

namespace {
struct GenerateDriverPass
    : public arc::impl::GenerateDriverBase<GenerateDriverPass> {
  using GenerateDriverBase::GenerateDriverBase;
  void runOnOperation() override;
  void generateDriver(ModelOp modelOp, SymbolTable &symbolTable);
};
} // namespace

void GenerateDriverPass::runOnOperation() {
  auto &symbolTable = getAnalysis<SymbolTable>();
  for (auto modelOp : getOperation().getOps<ModelOp>())
    generateDriver(modelOp, symbolTable);
}

/// Generate a `func.func @<model>_main` that runs the given model to
/// completion.
void GenerateDriverPass::generateDriver(ModelOp modelOp,
                                        SymbolTable &symbolTable) {
  auto *context = &getContext();
  auto loc = modelOp.getLoc();
  auto i64Type = IntegerType::get(context, 64);
  auto instanceType = SimModelInstanceType::get(
      context, FlatSymbolRefAttr::get(modelOp.getSymNameAttr()));

  // Create the driver function as a sibling placed before the model. The symbol
  // table uniquifies the name if it happens to collide.
  OpBuilder builder(modelOp);
  auto funcName = builder.getStringAttr(modelOp.getSymName() + "_main");
  auto funcType = builder.getFunctionType({}, {});
  auto funcOp = func::FuncOp::create(builder, loc, funcName, funcType);
  symbolTable.insert(funcOp);
  builder.setInsertionPointToStart(funcOp.addEntryBlock());

  // Instantiate the model. Its lifetime spans the body region, in which the
  // model storage is available as the instance block argument.
  auto instanceOp = SimInstantiateOp::create(builder, loc);
  func::ReturnOp::create(builder, loc);
  auto *instanceBlock = builder.createBlock(&instanceOp.getBody());
  auto instance = instanceBlock->addArgument(instanceType, loc);
  auto arcContext = AsContextOp::create(builder, loc, instance);

  // Loop until no further wakeup is scheduled or termination has been
  // requested. The next wakeup slot uses `UINT64_MAX` to signal that the model
  // has gone quiescent.
  auto never = arith::ConstantOp::create(builder, loc,
                                         builder.getIntegerAttr(i64Type, -1));
  auto firstWakeup = GetNextWakeupOp::create(builder, loc, arcContext);
  auto zero = arith::ConstantOp::create(builder, loc,
                                        builder.getIntegerAttr(i64Type, 0));

  // The loop carries the next wakeup time and the number of consecutive
  // evaluations at the current time.
  auto whileOp = scf::WhileOp::create(builder, loc, {i64Type, i64Type},
                                      ValueRange{firstWakeup, zero});

  // The "before" region tests whether a wakeup is still pending and the
  // simulation has not requested termination, and forwards the wakeup time
  // into the loop body.
  auto *beforeBlock = builder.createBlock(&whileOp.getBefore());
  auto wakeup = beforeBlock->addArgument(i64Type, loc);
  auto deltaCount = beforeBlock->addArgument(i64Type, loc);
  auto pending = arith::CmpIOp::create(builder, loc, arith::CmpIPredicate::ne,
                                       wakeup, never);
  auto terminated = IsTerminatedOp::create(builder, loc, arcContext);
  auto running = arith::CmpIOp::create(
      builder, loc, arith::CmpIPredicate::eq, terminated,
      arith::ConstantOp::create(builder, loc, builder.getBoolAttr(false)));
  auto proceed = arith::AndIOp::create(builder, loc, pending, running);
  scf::ConditionOp::create(builder, loc, proceed,
                           ValueRange{wakeup, deltaCount});

  // The "after" region advances time to the scheduled wakeup, evaluates the
  // model, and reads the next wakeup time to feed back into the condition.
  auto *afterBlock = builder.createBlock(&whileOp.getAfter());
  auto resumeTime = afterBlock->addArgument(i64Type, loc);
  auto prevDeltaCount = afterBlock->addArgument(i64Type, loc);

  // Count the consecutive evaluations at the same time, and abort the
  // simulation if the count exceeds the limit.
  auto currentTime = CurrentTimeOp::create(builder, loc, arcContext);
  auto sameTime = arith::CmpIOp::create(builder, loc, arith::CmpIPredicate::eq,
                                        resumeTime, currentTime);
  auto one = arith::ConstantOp::create(builder, loc,
                                       builder.getIntegerAttr(i64Type, 1));
  auto incremented = arith::AddIOp::create(builder, loc, prevDeltaCount, one);
  Value newDeltaCount =
      arith::SelectOp::create(builder, loc, sameTime, incremented, zero);
  OpBuilder::InsertionGuard guard(builder);
  if (deltaLimit != 0) {
    auto limit = arith::ConstantOp::create(
        builder, loc, builder.getIntegerAttr(i64Type, deltaLimit));
    auto exceeded = arith::CmpIOp::create(
        builder, loc, arith::CmpIPredicate::uge, newDeltaCount, limit);
    auto ifOp = scf::IfOp::create(builder, loc, exceeded, true);
    builder.setInsertionPoint(ifOp.thenYield());
    auto prefix = sim::FormatLiteralOp::create(
        builder, loc,
        "Error: simulation does not settle after " +
            std::to_string(deltaLimit) + " evaluations at time ");
    auto time = sim::FormatDecOp::create(builder, loc, resumeTime);
    time.setSpecifierWidthAttr(builder.getI32IntegerAttr(0));
    auto suffix = sim::FormatLiteralOp::create(
        builder, loc, " fs; possible combinational loop\n");
    auto message = sim::FormatStringConcatOp::create(
        builder, loc, ValueRange{prefix, time, suffix});
    auto stream = sim::StderrStreamOp::create(builder, loc);
    sim::PrintFormattedProcOp::create(builder, loc, message, stream);
    TerminateOp::create(builder, loc, arcContext, false);

    // Otherwise evaluate the model.
    builder.setInsertionPoint(ifOp.elseYield());
  }
  SimSetTimeOp::create(builder, loc, instance, resumeTime);
  SimStepOp::create(builder, loc, instance);
  builder.setInsertionPointToEnd(afterBlock);
  auto nextWakeup = GetNextWakeupOp::create(builder, loc, arcContext);
  scf::YieldOp::create(builder, loc, ValueRange{nextWakeup, newDeltaCount});
}
