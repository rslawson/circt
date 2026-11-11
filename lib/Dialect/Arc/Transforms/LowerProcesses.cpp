//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lower `llhd.process` ops into `arc.coroutine.define` and
// `arc.coroutine.instance` pairs so the rest of the arcilator pipeline can
// schedule them like any other coroutine.
//
// For each process, the lowering:
//
// - Captures values defined outside the body that are referenced inside.
//   Constant-like ops are cloned into the body; all other values are threaded
//   as coroutine arguments.
//
// - Lifts the body into a new top-level `arc.coroutine.define` and appends an
//   observe bitmask and an `i64` wakeup time result after the process results.
//   The entry block and every block targeted by an `llhd.wait` receive the
//   coroutine's argument types as leading block arguments, and a per-value SSA
//   renaming pass threads each coroutine argument through the CFG so uses see
//   the value bound on the most recent re-entry.
//
// - Rewrites each `llhd.wait` into an `arc.coroutine.yield` (or, for a wait
//   that can never resume, an `arc.coroutine.halt`) whose trailing yield
//   operands are an observe bitmask and a wakeup time. The bitmask has one bit
//   per coroutine argument, set for each observed value; the instance re-enters
//   the coroutine when a set argument changes. The wakeup is `now + delay`,
//   where `now` is the current simulation time the coroutine reads from the
//   model context via `arc.inferred_context` (with the delay converted to `i64`
//   via `llhd.time_to_int`, leaving the time unit opaque to this pass), or the
//   unit-independent sentinel `-1` when the wait has no delay. Each `llhd.halt`
//   becomes an `arc.coroutine.halt` with an empty mask and the `-1` wakeup.
//
// - Lowers simulation control ops in the body that need access to the model
//   context: `sim.terminate` and `sim.pause` become `arc.terminate`,
//   `sim.dump_file` and `sim.dump_vars` become their Arc equivalents, and
//   `llhd.current_time` reads the time via `arc.current_time`. A
//   `ub.unreachable` terminator, which typically follows a `sim.terminate`,
//   becomes a halt that yields zero values.
//
// - Replaces the original process with an `arc.coroutine.instance` that
//   forwards the captured values.
//
//===----------------------------------------------------------------------===//

#include "SignalUtils.h"
#include "circt/Dialect/Arc/ArcOps.h"
#include "circt/Dialect/Arc/ArcPasses.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "circt/Dialect/Verif/VerifOps.h"
#include "mlir/Analysis/Liveness.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/GenericIteratedDominanceFrontier.h"

namespace circt {
namespace arc {
#define GEN_PASS_DEF_LOWERPROCESSESPASS
#include "circt/Dialect/Arc/ArcPasses.h.inc"
} // namespace arc
} // namespace circt

using namespace circt;
using namespace arc;
using namespace mlir;
using llvm::SmallDenseMap;
using llvm::SmallDenseSet;

namespace {

/// Worker that lowers a single `llhd.process` or `llhd.coroutine` op.
struct ProcessLowering {
  ProcessLowering(Operation *op, SymbolTable &symbolTable)
      : op(op), processOp(dyn_cast<llhd::ProcessOp>(op)),
        llhdCoroutineOp(dyn_cast<llhd::CoroutineOp>(op)),
        isCoroutine(llhdCoroutineOp), symbolTable(symbolTable) {
    if (processOp)
      llvm::append_range(resultTypes, processOp.getResultTypes());
    else
      llvm::append_range(resultTypes,
                         llhdCoroutineOp.getFunctionType().getResults());
  }

  LogicalResult run();

private:
  Region &getBody() { return op->getRegion(0); }
  SmallVector<Value> createZeroResults(OpBuilder &builder, Location loc);
  void replaceUnreachables();
  LogicalResult expandCoroutineCalls();
  void collectCaptures();
  void createCoroutine();
  void lowerSimulationOps();
  void addEntryAndResumeBlockArguments();
  LogicalResult rewriteTerminators();
  void rewriteCallSuspensions();
  void renameCoroutineArgument(unsigned argIdx, Liveness &liveness,
                               DominanceInfo &dominance);
  void buildInstance();

  /// The op being lowered, and the process or coroutine it is. The coroutine
  /// op is erased once its body has been moved into `coroOp`.
  Operation *op;
  llhd::ProcessOp processOp;
  llhd::CoroutineOp llhdCoroutineOp;
  bool isCoroutine;
  SymbolTable &symbolTable;

  /// The results of the process or coroutine.
  SmallVector<Type> resultTypes;

  /// A suspension of the coroutine while a called coroutine is suspended. The
  /// observe bitmask of the suspension is derived from the callee's bitmask.
  struct CallSuspension {
    /// The block to be terminated by the suspension.
    Block *block;
    /// The block at which execution resumes.
    Block *resumeBlock;
    /// The call of the suspended callee.
    CoroutineCallOp callOp;
  };
  SmallVector<CallSuspension> callSuspensions;

  /// The coroutine created for this process. Populated by `createCoroutine`.
  CoroutineDefineOp coroOp;

  /// External SSA values referenced inside the body that need to be threaded
  /// as coroutine arguments.
  SmallVector<Value> captures;

  /// Block arguments of the coroutine entry block, mirroring `captures`.
  SmallVector<Value> entryArgs;

  /// Blocks targeted by an `llhd.wait`. Each receives a copy of the
  /// coroutine's argument-type prefix as its leading block arguments. (The
  /// entry block cannot appear here: MLIR entry blocks have no predecessors.)
  SetVector<Block *> resumeBlocks;

  /// Maps each block argument that stands in for a coroutine argument to its
  /// argument index `0..N-1`. This covers the entry block's leading arguments
  /// and every resume block's leading prefix arguments, so multiple block
  /// arguments map to the same index. Populated by
  /// `addEntryAndResumeBlockArguments` and used by `rewriteTerminators` to
  /// build the observe bitmask.
  SmallDenseMap<Value, uint32_t> argIndices;

  /// Indicates which arguments are relevant for the sensitivity check. This
  /// matches the bit-wise OR of all dynamically returned sensitivity masks.
  SmallVector<bool> staticSensitivityMask;

  /// The inferred `!arc.context` value.
  Value inferredContext;
};

} // namespace

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// Materialize a zero value of the given type. Falls back to `ub.poison` for
/// types that have no obvious zero value.
static Value createZeroValue(OpBuilder &builder, Location loc, Type type) {
  if (isa<FloatType>(type))
    return arith::ConstantOp::create(builder, loc, builder.getZeroAttr(type));
  if (isa<llhd::TimeType>(type))
    return llhd::ConstantTimeOp::create(builder, loc, 0, "fs", 0);
  auto bitWidth = hw::getBitWidth(type);
  if (bitWidth < 0)
    return ub::PoisonOp::create(builder, loc, type);
  auto flatType = builder.getIntegerType(bitWidth);
  Value value = hw::ConstantOp::create(builder, loc, flatType, 0);
  if (type != flatType)
    value = hw::BitcastOp::create(builder, loc, type, value);
  return value;
}

/// Materialize zero values for all results of the process or coroutine.
SmallVector<Value> ProcessLowering::createZeroResults(OpBuilder &builder,
                                                      Location loc) {
  SmallVector<Value> values;
  for (auto type : resultTypes)
    values.push_back(createZeroValue(builder, loc, type));
  return values;
}

/// Replace `ub.unreachable` terminators in the body with `llhd.halt` ops that
/// yield zero values. These terminators typically follow a `sim.terminate` op.
/// Since `arc.terminate` only requests termination after the current
/// evaluation, the coroutine has to suspend cleanly.
void ProcessLowering::replaceUnreachables() {
  SmallVector<ub::UnreachableOp> unreachables;
  getBody().walk([&](ub::UnreachableOp op) { unreachables.push_back(op); });
  for (auto op : unreachables) {
    OpBuilder builder(op);
    llhd::HaltOp::create(builder, op.getLoc(),
                         createZeroResults(builder, op.getLoc()));
    op.erase();
  }
}

/// Compute the type of the `arc.coroutine.define` an `llhd.coroutine` is
/// lowered to. The results are extended with an observe bitmask with one bit
/// per argument, and the next wakeup time.
static FunctionType getArcCoroutineType(FunctionType type) {
  auto *context = type.getContext();
  SmallVector<Type> resultTypes(type.getResults());
  resultTypes.push_back(IntegerType::get(context, type.getNumInputs()));
  resultTypes.push_back(IntegerType::get(context, 64));
  return FunctionType::get(context, type.getInputs(), resultTypes);
}

/// Expand each `llhd.call_coroutine` op into a loop that repeatedly calls the
/// coroutine until it returns. Whenever the callee suspends, the caller
/// suspends as well and resumes the callee upon re-entry. If the callee halts,
/// the caller halts as well.
///
/// The suspensions are only created once the body has been moved into the
/// coroutine and its arguments are known, by `rewriteCallSuspensions`.
LogicalResult ProcessLowering::expandCoroutineCalls() {
  SmallVector<llhd::CallCoroutineOp> callOps;
  getBody().walk([&](llhd::CallCoroutineOp op) { callOps.push_back(op); });

  for (auto callOp : callOps) {
    auto loc = callOp.getLoc();
    auto *context = callOp.getContext();
    auto calleeAttr = callOp.getCalleeAttr();

    // Determine the type of the callee once it is lowered to an Arc coroutine.
    FunctionType calleeType;
    auto *calleeOp = symbolTable.lookup(calleeAttr.getAttr());
    if (auto coroOp = dyn_cast_or_null<llhd::CoroutineOp>(calleeOp))
      calleeType = getArcCoroutineType(coroOp.getFunctionType());
    else if (auto defineOp = dyn_cast_or_null<CoroutineDefineOp>(calleeOp))
      calleeType = defineOp.getFunctionType();
    else
      return callOp.emitOpError() << "calls unknown coroutine " << calleeAttr;
    auto stateType = CoroutineStateType::get(context, calleeAttr);
    auto pcType = CoroutinePCType::get(context, calleeAttr);

    // Split the block at the call. The call's results become arguments of the
    // continuation block.
    auto *beforeBlock = callOp->getBlock();
    auto *contBlock = beforeBlock->splitBlock(std::next(callOp->getIterator()));
    for (auto result : callOp.getResults())
      result.replaceAllUsesWith(contBlock->addArgument(result.getType(), loc));

    // Create the blocks of the call loop.
    auto *region = beforeBlock->getParent();
    auto *callBlock = new Block();
    auto *suspendBlock = new Block();
    auto *haltBlock = new Block();
    auto *waitBlock = new Block();
    auto *resumeBlock = new Block();
    region->getBlocks().insertAfter(beforeBlock->getIterator(), callBlock);
    region->getBlocks().insertAfter(callBlock->getIterator(), suspendBlock);
    region->getBlocks().insertAfter(suspendBlock->getIterator(), haltBlock);
    region->getBlocks().insertAfter(haltBlock->getIterator(), waitBlock);
    region->getBlocks().insertAfter(waitBlock->getIterator(), resumeBlock);
    auto state = callBlock->addArgument(stateType, loc);
    auto pc = callBlock->addArgument(pcType, loc);
    auto resumeState = resumeBlock->addArgument(stateType, loc);
    auto resumePC = resumeBlock->addArgument(pcType, loc);

    // Start the callee with an undefined state.
    auto builder = OpBuilder::atBlockEnd(beforeBlock);
    auto initState = CoroutineUndefinedStateOp::create(builder, loc, stateType);
    auto initPC = CoroutineStartPCOp::create(builder, loc, pcType);
    cf::BranchOp::create(builder, loc, callBlock,
                         ValueRange{initState, initPC});

    // Call the coroutine, and continue if it returned.
    builder.setInsertionPointToEnd(callBlock);
    SmallVector<Type> callResultTypes{stateType, pcType};
    llvm::append_range(callResultTypes, calleeType.getResults());
    auto newCallOp =
        CoroutineCallOp::create(builder, loc, callResultTypes, calleeAttr,
                                state, pc, callOp.getOperands());
    auto newPC = newCallOp.getResult(1);
    auto isReturn = CoroutinePCIsReturnOp::create(builder, loc, newPC);
    // Note that `getResults()` only covers the values returned by the callee,
    // excluding the state and program counter.
    auto results = newCallOp.getResults().take_front(callOp.getNumResults());
    cf::CondBranchOp::create(builder, loc, isReturn, contBlock, results,
                             suspendBlock, ValueRange{});

    // Halt if the callee halted, and suspend otherwise.
    builder.setInsertionPointToEnd(suspendBlock);
    auto isHalt = CoroutinePCIsHaltOp::create(builder, loc, newPC);
    cf::CondBranchOp::create(builder, loc, isHalt, haltBlock, ValueRange{},
                             waitBlock, ValueRange{});
    builder.setInsertionPointToEnd(haltBlock);
    llhd::HaltOp::create(builder, loc, createZeroResults(builder, loc));

    // The suspension in the wait block is created later. Terminate the block
    // with a placeholder until then. Upon resumption, call the coroutine again
    // with its new state.
    builder.setInsertionPointToEnd(waitBlock);
    ub::UnreachableOp::create(builder, loc);
    builder.setInsertionPointToEnd(resumeBlock);
    cf::BranchOp::create(builder, loc, callBlock,
                         ValueRange{resumeState, resumePC});
    callSuspensions.push_back({waitBlock, resumeBlock, newCallOp});
    callOp.erase();
  }
  return success();
}

void ProcessLowering::collectCaptures() {
  auto &body = processOp.getBody();
  auto entryBuilder = OpBuilder::atBlockBegin(&body.front());
  SmallDenseSet<Value> captureSet;
  DenseMap<Operation *, Operation *> clonedConstants;

  // Map an outside-defined value to a value usable inside the body by the
  // given user. Constants are cloned into the entry block. Projections into
  // signals and format strings are cloned right before the user, such that
  // their operands are re-evaluated with the coroutine argument values on every
  // entry. Only the projected signal itself and the operands have to be
  // captured.
  std::function<Value(Value, Operation *)> capture =
      [&](Value value, Operation *user) -> Value {
    auto result = dyn_cast<OpResult>(value);
    if (result && result.getOwner()->hasTrait<OpTrait::ConstantLike>()) {
      auto *&cloned = clonedConstants[result.getOwner()];
      if (!cloned)
        cloned = entryBuilder.clone(*result.getOwner());
      return cloned->getResult(result.getResultNumber());
    }
    // Format strings cannot be passed to coroutines. Clone the pure ops
    // producing them before the user instead, which may happen if CSE merged
    // a format string with an identical one outside the process.
    bool isFormatString = result &&
                          isa<sim::FormatStringType>(result.getType()) &&
                          isPure(result.getOwner());
    if (result &&
        (isFormatString || signals::isProjection(result.getOwner()))) {
      auto *cloned = result.getOwner()->clone();
      OpBuilder(user).insert(cloned);
      for (auto &operand : cloned->getOpOperands())
        operand.set(capture(operand.get(), cloned));
      return cloned->getResult(result.getResultNumber());
    }

    // Otherwise this is an outside-defined SSA value we need to capture as an
    // argument of the coroutine.
    if (captureSet.insert(value).second)
      captures.push_back(value);
    return value;
  };

  SmallVector<Operation *> ops;
  body.walk([&](Operation *op) { ops.push_back(op); });
  for (auto *op : ops) {
    for (auto &operand : op->getOpOperands()) {
      // Skip values defined inside the body.
      if (body.isAncestor(operand.get().getParentRegion()))
        continue;
      operand.set(capture(operand.get(), op));
    }
  }
}

void ProcessLowering::createCoroutine() {
  // An `llhd.coroutine` is replaced with an `arc.coroutine.define` of the same
  // name.
  if (llhdCoroutineOp) {
    OpBuilder builder(llhdCoroutineOp);
    auto name = llhdCoroutineOp.getSymNameAttr();
    auto type = getArcCoroutineType(llhdCoroutineOp.getFunctionType());
    symbolTable.remove(llhdCoroutineOp);
    coroOp = CoroutineDefineOp::create(builder, llhdCoroutineOp.getLoc(), name,
                                       TypeAttr::get(type));
    coroOp.setSymVisibilityAttr(llhdCoroutineOp.getSymVisibilityAttr());
    symbolTable.insert(coroOp);
    coroOp.getBody().takeBody(llhdCoroutineOp.getBody());
    llhdCoroutineOp.erase();
    llhdCoroutineOp = {};
    op = coroOp;
    builder.setInsertionPointToStart(&coroOp.getBody().front());
    inferredContext = InferredContextOp::create(builder, coroOp.getLoc());
    return;
  }

  // Find the parent module and use it as the insertion anchor.
  auto hwModule = processOp->getParentOfType<hw::HWModuleOp>();
  assert(hwModule);
  OpBuilder builder(hwModule);

  // Build the function type:
  // `(captureTypes...) -> (procResults..., iN mask, i64 wakeup)`, where
  // the observe bitmask `iN` carries one bit per coroutine argument and the
  // trailing `i64` is the next wakeup time.
  auto i64Type = builder.getIntegerType(64);
  SmallVector<Type> argTypes;
  for (auto cap : captures)
    argTypes.push_back(cap.getType());
  auto maskType = builder.getIntegerType(argTypes.size());
  SmallVector<Type> funcResultTypes(resultTypes);
  funcResultTypes.push_back(maskType);
  funcResultTypes.push_back(i64Type);
  auto funcType = builder.getFunctionType(argTypes, funcResultTypes);

  // Pick a symbol name based on the parent hw.module's name. Insertion into
  // the symbol table uniquifies the name.
  auto funcName =
      builder.getStringAttr(hwModule.getSymName() + ".llhd.process");
  coroOp = CoroutineDefineOp::create(builder, processOp.getLoc(), funcName,
                                     funcType);
  symbolTable.insert(coroOp);

  // Move the process body wholesale into the coroutine. Block arguments are
  // added in a separate step.
  coroOp.getBody().takeBody(processOp.getBody());

  // Retrieve the context value in the entry block.
  builder.setInsertionPointToStart(&coroOp.getBody().front());
  inferredContext = InferredContextOp::create(builder, coroOp.getLoc());
}

/// Lower an immediate `verif.assert` or `verif.assume` op with an `i1`
/// property into a print of an error message if the property does not hold.
static void lowerImmediateAssertion(OpBuilder &builder, Operation *op) {
  auto loc = op->getLoc();
  Value property = op->getOperand(0);
  Value enable = op->getNumOperands() > 1 ? op->getOperand(1) : Value{};
  auto label = op->getAttrOfType<StringAttr>("label");

  // Compute whether the assertion failed.
  auto trueValue = hw::ConstantOp::create(builder, loc, builder.getI1Type(), 1);
  Value failed = comb::XorOp::create(builder, loc, property, trueValue);
  if (enable)
    failed = comb::AndOp::create(builder, loc, failed, enable);

  // Assemble the error message.
  std::string message = "Error: Assertion failed";
  if (label && !label.getValue().empty())
    message += " (" + label.getValue().str() + ")";
  if (auto fileLoc = dyn_cast<FileLineColLoc>(loc))
    message += " at " + fileLoc.getFilename().getValue().str() + ":" +
               std::to_string(fileLoc.getLine());
  message += "\n";

  // Print the message if the assertion failed.
  auto *block = op->getBlock();
  auto *contBlock = block->splitBlock(op->getIterator());
  auto *failBlock = new Block();
  block->getParent()->getBlocks().insert(contBlock->getIterator(), failBlock);
  builder.setInsertionPointToEnd(block);
  cf::CondBranchOp::create(builder, loc, failed, failBlock, contBlock);
  builder.setInsertionPointToEnd(failBlock);
  auto literal = sim::FormatLiteralOp::create(builder, loc, message);
  sim::PrintFormattedProcOp::create(builder, loc, literal);
  cf::BranchOp::create(builder, loc, contBlock);
}

/// Lower ops in the coroutine body that interact with the simulation and need
/// access to the model context.
void ProcessLowering::lowerSimulationOps() {
  SmallVector<Operation *> ops;
  coroOp.walk([&](Operation *op) {
    if (isa<sim::TerminateOp, sim::PauseOp, sim::DumpFileOp, sim::DumpVarsOp,
            llhd::CurrentTimeOp>(op))
      ops.push_back(op);
    else if (isa<verif::AssertOp, verif::AssumeOp>(op) &&
             op->getOperand(0).getType().isInteger(1))
      ops.push_back(op);
  });
  for (auto *op : ops) {
    OpBuilder builder(op);
    auto loc = op->getLoc();
    if (auto termOp = dyn_cast<sim::TerminateOp>(op)) {
      TerminateOp::create(builder, loc, inferredContext, termOp.getSuccess());
    } else if (isa<sim::PauseOp>(op)) {
      // Non-interactive simulations terminate on `$stop`. Treat this as a
      // failure, similar to other simulators.
      TerminateOp::create(builder, loc, inferredContext, false);
    } else if (auto dumpFileOp = dyn_cast<sim::DumpFileOp>(op)) {
      DumpFileOp::create(builder, loc, inferredContext,
                         dumpFileOp.getFilenameAttr());
    } else if (isa<sim::DumpVarsOp>(op)) {
      DumpVarsOp::create(builder, loc, inferredContext);
    } else if (isa<verif::AssertOp, verif::AssumeOp>(op)) {
      // An immediate assertion that fails reports an error, like `$error`, and
      // the simulation continues. See IEEE 1800-2017 § 16.3 "Immediate
      // assertions".
      lowerImmediateAssertion(builder, op);
    } else if (auto timeOp = dyn_cast<llhd::CurrentTimeOp>(op)) {
      auto now = CurrentTimeOp::create(builder, loc, inferredContext);
      auto time = llhd::IntToTimeOp::create(builder, loc, now);
      timeOp.replaceAllUsesWith(time.getResult());
    }
    op->erase();
  }
}

/// Prepend the coroutine function-type prefix as leading block arguments to
/// the entry block and to every block targeted by an `llhd.wait`. After this
/// step the resume blocks satisfy `arc.coroutine.yield`'s contract (its
/// destination's first `N` arguments must match the coroutine's function
/// type) and the entry block's first `N` arguments become the SSA values
/// that the per-argument SSA renaming step will thread through the CFG.
///
/// Predecessor branches of resume blocks that are not `llhd.wait`s (e.g.,
/// `cf.br` from inside the body looping back to a wait target) have their
/// successor operands extended with placeholder values — the entry block
/// arguments themselves — at positions 0..N-1, so that the operand count
/// keeps matching the resume block's new argument count. The SSA renaming
/// step run afterwards walks the body and rewrites these references to the
/// correct reaching definition for the source block.
///
/// Uses of the original outside captures inside the body are also rewritten
/// to the entry block's new capture arguments here, so by the time renaming
/// runs every reference is to an internal SSA value defined at the entry.
void ProcessLowering::addEntryAndResumeBlockArguments() {
  auto &body = coroOp.getBody();
  auto loc = op->getLoc();

  // Assemble the initial resume block argument types that will be provided by
  // callers entering or re-entering the coroutine.
  auto prefixTypes = coroOp.getArgumentTypes();

  // Add arguments to the entry block, recording each one's coroutine argument
  // index so `rewriteTerminators` can map observed values to bitmask bits. The
  // entry block of a lowered `llhd.coroutine` already has these arguments.
  auto &entry = body.front();
  for (auto [index, type] : llvm::enumerate(prefixTypes)) {
    if (isCoroutine)
      entryArgs.push_back(entry.getArgument(index));
    else
      entryArgs.push_back(entry.insertArgument(index, type, loc));
    argIndices[entryArgs.back()] = index;
  }

  // Collect the resume blocks, which are targets of `llhd.wait` ops and of the
  // suspensions in coroutine calls.
  for (auto &block : body)
    if (auto wait = dyn_cast<llhd::WaitOp>(block.getTerminator()))
      resumeBlocks.insert(wait.getDest());
  for (auto &suspension : callSuspensions)
    resumeBlocks.insert(suspension.resumeBlock);

  // Pre-add prefix args to each resume block, then extend non-wait predecessor
  // branches with placeholder operands so the destination operand counts keep
  // matching.
  auto prefixSize = prefixTypes.size();
  for (auto *resumeBlock : resumeBlocks) {
    for (auto [index, type] : llvm::enumerate(prefixTypes))
      argIndices[resumeBlock->insertArgument(index, type, loc)] = index;

    // Fix non-wait predecessors. The wait predecessors will become
    // `arc.coroutine.yield`s in the next step; the yield's
    // `getSuccessorOperands` treats the prefix as produced (not forwarded) so
    // its operand count already matches.
    for (auto *pred : resumeBlock->getPredecessors()) {
      auto *term = pred->getTerminator();
      if (isa<llhd::WaitOp, CoroutineYieldOp>(term))
        continue;
      auto branchOp = cast<BranchOpInterface>(term);
      for (unsigned succIdx = 0, succNum = term->getNumSuccessors();
           succIdx < succNum; ++succIdx) {
        if (term->getSuccessor(succIdx) != resumeBlock)
          continue;
        auto succOperands = branchOp.getSuccessorOperands(succIdx);
        SmallVector<Value> newOperands(entryArgs.begin(),
                                       entryArgs.begin() + prefixSize);
        for (auto v : succOperands.getForwardedOperands())
          newOperands.push_back(v);
        succOperands.getMutableForwardedOperands().assign(newOperands);
      }
    }
  }

  // Replace in-body uses of the original outside captures with the entry
  // block's new arguments. The per-argument SSA renaming step then refines
  // these uses to the right reaching definitions, since each resume block
  // provides its own set of resume arguments, and multiple resume blocks may
  // converge on a single block.
  for (auto [capture, entryArg] : llvm::zip(captures, entryArgs))
    capture.replaceUsesWithIf(entryArg, [&](OpOperand &use) {
      return body.isAncestor(use.getOwner()->getParentRegion());
    });
}

/// Convert `llhd.wait`/`llhd.halt` terminators to their `arc.coroutine`
/// equivalents. The wakeup operand is the inferred context's current time plus
/// the wait's delay. A `llhd.wait` with an `observed` clause yields an observe
/// bitmask with the bit of each observed coroutine argument set; ops without a
/// delay and without observed values can never resume and are treated as halts.
LogicalResult ProcessLowering::rewriteTerminators() {
  // The observe bitmask has one bit per coroutine argument. `argIndices` maps
  // the block arguments that stand in for coroutine arguments to their bit
  // index; block arguments beyond that prefix (a wait's forwarded destination
  // operands) and values defined inside the body are absent from the map and
  // therefore cannot be observed for changes.
  auto maskWidth = entryArgs.size();
  staticSensitivityMask.resize(maskWidth, false);
  for (auto &block : coroOp.getBody()) {
    auto *term = block.getTerminator();
    auto loc = term->getLoc();
    OpBuilder builder(term);

    // Handle `llhd.wait` ops.
    if (auto wait = dyn_cast<llhd::WaitOp>(term)) {
      // Build the observe bitmask from the observed operands. Only operands
      // that map to a coroutine argument set a bit; internally-defined values
      // (which can never change) and forwarded block arguments contribute none.
      APInt maskBits(maskWidth, 0);
      for (auto observed : wait.getObserved()) {
        // Observing the probed value of a signal argument observes the
        // signal itself.
        if (auto probeOp = observed.getDefiningOp<llhd::ProbeOp>()) {
          SmallVector<signals::Projection> projections;
          observed = signals::getRootRef(probeOp.getSignal(), projections);
        }
        if (auto it = argIndices.find(observed); it != argIndices.end()) {
          maskBits.setBit(it->second);
          staticSensitivityMask[it->second] = true;
        }
      }
      auto mask = hw::ConstantOp::create(builder, loc, maskBits);

      // Waits in a coroutine do not yield any values. Yield zeros for the
      // coroutine's results instead.
      SmallVector<Value> waitYieldOperands(wait.getYieldOperands());
      if (isCoroutine)
        waitYieldOperands = createZeroResults(builder, loc);

      // A wait without a delay never resumes on time; use the sentinel `-1` as
      // its wakeup. It is only a real suspension if it observes something --
      // otherwise it can never resume and is treated as a halt.
      if (!wait.getDelay()) {
        auto never =
            hw::ConstantOp::create(builder, loc, APInt::getAllOnes(64));
        SmallVector<Value> yieldOperands(waitYieldOperands);
        yieldOperands.push_back(mask);
        yieldOperands.push_back(never);
        if (wait.getObserved().empty())
          CoroutineHaltOp::create(builder, loc, yieldOperands);
        else
          CoroutineYieldOp::create(builder, loc, yieldOperands,
                                   wait.getDestOperands(), wait.getDest());
        wait.erase();
        continue;
      }

      auto now = CurrentTimeOp::create(builder, loc, inferredContext);
      auto delay = llhd::TimeToIntOp::create(builder, loc, wait.getDelay());
      auto wakeup = comb::AddOp::create(builder, loc, now, delay);
      SmallVector<Value> yieldOperands(waitYieldOperands);
      yieldOperands.push_back(mask);
      yieldOperands.push_back(wakeup);
      CoroutineYieldOp::create(builder, loc, yieldOperands,
                               wait.getDestOperands(), wait.getDest());
      wait.erase();
      continue;
    }

    // Handle `llhd.halt` ops. Halts in a coroutine do not yield any values.
    // Yield zeros for the coroutine's results instead.
    if (auto halt = dyn_cast<llhd::HaltOp>(term)) {
      auto mask = hw::ConstantOp::create(builder, loc, APInt(maskWidth, 0));
      auto never = hw::ConstantOp::create(builder, loc, APInt::getAllOnes(64));
      SmallVector<Value> yieldOperands(halt.getYieldOperands());
      if (yieldOperands.size() != resultTypes.size())
        yieldOperands = createZeroResults(builder, loc);
      yieldOperands.push_back(mask);
      yieldOperands.push_back(never);
      CoroutineHaltOp::create(builder, loc, yieldOperands);
      halt.erase();
      continue;
    }

    // Handle `llhd.return` ops in coroutines.
    if (auto ret = dyn_cast<llhd::ReturnOp>(term)) {
      auto mask = hw::ConstantOp::create(builder, loc, APInt(maskWidth, 0));
      auto never = hw::ConstantOp::create(builder, loc, APInt::getAllOnes(64));
      SmallVector<Value> yieldOperands(ret.getOperands());
      yieldOperands.push_back(mask);
      yieldOperands.push_back(never);
      CoroutineReturnOp::create(builder, loc, yieldOperands);
      ret.erase();
      continue;
    }
  }

  rewriteCallSuspensions();
  return success();
}

/// Create the suspensions of the coroutine while a called coroutine is
/// suspended. The coroutine resumes at the same time as the callee. The callee
/// observes changes of its arguments, which are translated to the coroutine's
/// own arguments: if a signal passed to the callee is derived from a signal
/// argument of the coroutine, a change of that argument resumes the coroutine.
/// Since the callee re-checks the conditions it waits for upon resumption,
/// resuming due to a change of a different part of the same signal is
/// harmless.
void ProcessLowering::rewriteCallSuspensions() {
  auto maskWidth = entryArgs.size();
  for (auto &suspension : callSuspensions) {
    auto callOp = suspension.callOp;
    auto loc = callOp.getLoc();
    suspension.block->getTerminator()->erase();
    auto builder = OpBuilder::atBlockEnd(suspension.block);

    // Map the bits of the callee's observe bitmask to the coroutine's bits. The
    // callee's last two results are the observe bitmask and the wakeup time.
    auto calleeMask = callOp.getResults().drop_back().back();
    SmallVector<Value> bits(maskWidth);
    for (auto [calleeIndex, arg] : llvm::enumerate(callOp.getArgs())) {
      if (!isa<llhd::RefType>(arg.getType()))
        continue;
      SmallVector<signals::Projection> projections;
      auto it = argIndices.find(signals::getRootRef(arg, projections));
      if (it == argIndices.end())
        continue;
      Value bit =
          comb::ExtractOp::create(builder, loc, calleeMask, calleeIndex, 1);
      auto &slot = bits[it->second];
      slot = slot ? comb::OrOp::create(builder, loc, slot, bit) : bit;
      staticSensitivityMask[it->second] = true;
    }
    Value mask;
    if (maskWidth == 0) {
      mask = hw::ConstantOp::create(builder, loc, builder.getIntegerType(0), 0);
    } else {
      SmallVector<Value> concatBits;
      for (auto bit : llvm::reverse(bits))
        concatBits.push_back(
            bit ? bit
                : hw::ConstantOp::create(builder, loc, builder.getI1Type(), 0));
      mask = comb::ConcatOp::create(builder, loc, concatBits);
    }

    auto yieldOperands = createZeroResults(builder, loc);
    yieldOperands.push_back(mask);
    yieldOperands.push_back(callOp.getResults().back());
    CoroutineYieldOp::create(
        builder, loc, yieldOperands,
        ValueRange{callOp.getResult(0), callOp.getResult(1)},
        suspension.resumeBlock);
  }
}

/// SSA-rename uses of the coroutine's `argIdx`-th entry-block argument so
/// that every use observes the reaching definition for its block.
///
/// The entry block's argument and the corresponding pre-added argument on
/// each resume block are the per-block "redefinitions" of this value. Any
/// block where two such reaching definitions converge — the iterated
/// dominance frontier of the redefining blocks, restricted to blocks where
/// the value is live-in — receives a fresh merge block argument and any
/// branches reaching it gain the appropriate successor operand. Inside each
/// block we then rewrite uses to that block's reaching definition.
///
/// This mirrors the capture rewriting we do in `LowerCoroutines` as part of the
/// `captureValue` function.
void ProcessLowering::renameCoroutineArgument(unsigned argIdx,
                                              Liveness &liveness,
                                              DominanceInfo &dominance) {
  auto &body = coroOp.getBody();
  auto entryArg = entryArgs[argIdx];
  auto *entryBlock = &body.front();

  // Defining blocks of the value: the entry block plus every resume block.
  SmallPtrSet<Block *, 8> definingBlocks;
  definingBlocks.insert(entryBlock);
  for (auto *resumeBlock : resumeBlocks)
    definingBlocks.insert(resumeBlock);

  // Restrict the IDF to blocks where the value is live-in.
  SmallPtrSet<Block *, 16> liveInBlocks;
  for (auto &block : body)
    if (liveness.getLiveness(&block)->isLiveIn(entryArg))
      liveInBlocks.insert(&block);

  auto &domTree = dominance.getDomTree(&body);
  // Second template arg `false` means forward (not post-) dominance frontier.
  llvm::IDFCalculatorBase<Block, false> idfCalculator(domTree);
  idfCalculator.setDefiningBlocks(definingBlocks);
  idfCalculator.setLiveInBlocks(liveInBlocks);
  SmallVector<Block *> mergeBlocks;
  idfCalculator.calculate(mergeBlocks);

  SmallPtrSet<Block *, 16> mergeBlockSet(mergeBlocks.begin(),
                                         mergeBlocks.end());

  // Walk the dominator tree from the entry block, threading the reaching
  // definition through each block.
  struct WorklistItem {
    DominanceInfoNode *domNode;
    Value reachingDef;
  };
  SmallVector<WorklistItem> worklist;
  worklist.push_back({domTree.getNode(entryBlock), entryArg});

  while (!worklist.empty()) {
    auto item = worklist.pop_back_val();
    auto *block = item.domNode->getBlock();

    if (resumeBlocks.contains(block)) {
      item.reachingDef = block->getArgument(argIdx);
    } else if (mergeBlockSet.contains(block)) {
      item.reachingDef =
          block->addArgument(entryArg.getType(), entryArg.getLoc());
    }

    // Rewrite uses in this block (including nested regions and successor
    // operands of the terminator) to the reaching definition.
    if (item.reachingDef != entryArg)
      block->walk([&](Operation *nested) {
        nested->replaceUsesOfWith(entryArg, item.reachingDef);
      });

    // For each edge leaving this block into a merge block, forward the
    // reaching definition by appending it to the branch op's successor
    // operands. Resume blocks are skipped even when the IDF also flags them
    // as merge blocks: their prefix slots are filled by the runtime on the
    // yield path and by the placeholder operands fixed up by the RAUW above
    // on the non-yield paths, so an append would push an extra operand past
    // the block's argument count.
    auto *terminator = block->getTerminator();
    if (auto branchOp = dyn_cast<BranchOpInterface>(terminator)) {
      for (auto &blockOperand : terminator->getBlockOperands()) {
        auto *succ = blockOperand.get();
        if (!mergeBlockSet.contains(succ) || resumeBlocks.contains(succ))
          continue;
        branchOp.getSuccessorOperands(blockOperand.getOperandNumber())
            .append(item.reachingDef);
      }
    }

    for (auto *child : item.domNode->children())
      worklist.push_back({child, item.reachingDef});
  }
}

/// Create an `arc.coroutine.instance` that instantiates the outlined coroutine
/// in place of the old `llhd.process`.
void ProcessLowering::buildInstance() {
  OpBuilder builder(processOp);
  auto loc = processOp.getLoc();

  assert(captures.size() == staticSensitivityMask.size());
  auto instanceOp = CoroutineInstanceOp::create(
      builder, loc, processOp.getResultTypes(),
      FlatSymbolRefAttr::get(coroOp.getSymNameAttr()), captures,
      builder.getDenseBoolArrayAttr(staticSensitivityMask));

  processOp.replaceAllUsesWith(instanceOp.getResults());
  processOp.erase();
}

LogicalResult ProcessLowering::run() {
  replaceUnreachables();
  if (failed(expandCoroutineCalls()))
    return failure();
  if (processOp)
    collectCaptures();
  createCoroutine();
  lowerSimulationOps();
  addEntryAndResumeBlockArguments();
  if (failed(rewriteTerminators()))
    return failure();

  // SSA-rename each coroutine entry argument through the CFG. With a single
  // block, every use is already in the entry block and refers to the entry
  // arg directly, so there is nothing to thread; `DominanceInfo` also asserts
  // on single-block regions.
  if (!coroOp.getBody().hasOneBlock()) {
    Liveness liveness(coroOp);
    DominanceInfo dominance(coroOp);
    for (unsigned argIdx = 0, argNum = entryArgs.size(); argIdx != argNum;
         ++argIdx) {
      // References to signals passed into the coroutine always refer to the
      // same signal on every entry. Uses can keep referring to the entry
      // block's argument, which is persisted across suspensions like any other
      // value.
      if (isa<llhd::RefType>(entryArgs[argIdx].getType()))
        continue;
      renameCoroutineArgument(argIdx, liveness, dominance);
    }
  }

  if (processOp)
    buildInstance();
  return success();
}

//===----------------------------------------------------------------------===//
// Pass Implementation
//===----------------------------------------------------------------------===//

namespace {
struct LowerProcessesPass
    : public arc::impl::LowerProcessesPassBase<LowerProcessesPass> {
  void runOnOperation() override;
};
} // namespace

/// Convert an `llhd.combinational` op with side effects into an equivalent
/// `llhd.process`. The combinational op re-executes its body whenever any of
/// the values it uses changes. Without side effects, the body can simply be
/// evaluated as part of every model evaluation. With side effects, such as
/// prints or termination of the simulation, the body must only be executed
/// once initially and whenever the used values change, which is exactly what a
/// process waiting for changes of these values does.
static void convertCombinationalWithSideEffects(llhd::CombinationalOp op) {
  bool hasSideEffects = false;
  op.getBody().walk([&](Operation *nestedOp) {
    if (isa<llhd::YieldOp>(nestedOp) ||
        nestedOp->hasTrait<OpTrait::IsTerminator>() ||
        mlir::isMemoryEffectFree(nestedOp))
      return WalkResult::advance();
    hasSideEffects = true;
    return WalkResult::interrupt();
  });
  if (!hasSideEffects)
    return;

  // Collect the values used by the body that are defined outside of it. The
  // process observes changes of these values.
  SetVector<Value> observed;
  op.getBody().walk([&](Operation *nestedOp) {
    for (auto operand : nestedOp->getOperands())
      if (!op.getBody().isAncestor(operand.getParentRegion()) &&
          !isa_and_nonnull<hw::ConstantOp>(operand.getDefiningOp()))
        observed.insert(operand);
  });

  // Create the process and move the body into it. Add a new entry block that
  // branches to the old entry block, which becomes the resume block of every
  // wait. Signals used by the body are observed through a probe.
  OpBuilder builder(op);
  auto loc = op.getLoc();
  auto processOp =
      llhd::ProcessOp::create(builder, loc, op.getResultTypes(), ValueRange{});
  processOp.getBody().takeBody(op.getBody());
  auto *bodyBlock = &processOp.getBody().front();
  builder.createBlock(&processOp.getBody(), processOp.getBody().begin());
  SmallVector<Value> observedValues;
  for (auto value : observed) {
    if (isa<llhd::RefType>(value.getType()))
      value = llhd::ProbeOp::create(builder, loc, value);
    observedValues.push_back(value);
  }
  cf::BranchOp::create(builder, loc, bodyBlock);

  // Replace each `llhd.yield` with a wait that yields the same values and
  // resumes at the beginning of the body when an observed value changes.
  SmallVector<llhd::YieldOp> yieldOps;
  processOp.getBody().walk([&](llhd::YieldOp op) { yieldOps.push_back(op); });
  for (auto yieldOp : yieldOps) {
    builder.setInsertionPoint(yieldOp);
    llhd::WaitOp::create(builder, yieldOp.getLoc(), yieldOp.getOperands(),
                         /*delay=*/Value{}, observedValues, ValueRange{},
                         bodyBlock);
    yieldOp.erase();
  }
  op.replaceAllUsesWith(processOp.getResults());
  op.erase();
}

void LowerProcessesPass::runOnOperation() {
  auto module = getOperation();
  auto &symbolTable = getAnalysis<SymbolTable>();

  SmallVector<llhd::CombinationalOp> combOps;
  module.walk([&](llhd::CombinationalOp op) { combOps.push_back(op); });
  for (auto op : combOps)
    convertCombinationalWithSideEffects(op);

  SmallVector<Operation *> ops;
  module.walk([&](Operation *op) {
    if (isa<llhd::ProcessOp, llhd::CoroutineOp>(op))
      ops.push_back(op);
  });
  bool anyFailed = false;
  for (auto *op : ops)
    if (failed(ProcessLowering(op, symbolTable).run()))
      anyFailed = true;
  if (anyFailed)
    signalPassFailure();
}
