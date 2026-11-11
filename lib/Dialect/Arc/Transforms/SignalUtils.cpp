//===- SignalUtils.cpp - Helpers for lowering LLHD signals ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SignalUtils.h"
#include "circt/Dialect/Arc/ArcOps.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace circt;
using namespace arc;
using namespace signals;

bool signals::isProjection(Operation *op) {
  return isa<llhd::SigExtractOp, llhd::SigArrayGetOp, llhd::SigArraySliceOp,
             llhd::SigStructExtractOp>(op);
}

Value signals::getRootRef(Value ref, SmallVectorImpl<Projection> &projections) {
  SmallVector<Projection> reversed;
  while (auto *op = ref.getDefiningOp()) {
    if (!isProjection(op))
      break;
    Value index =
        TypeSwitch<Operation *, Value>(op)
            .Case<llhd::SigExtractOp>([](auto op) { return op.getLowBit(); })
            .Case<llhd::SigArrayGetOp>([](auto op) { return op.getIndex(); })
            .Case<llhd::SigArraySliceOp>(
                [](auto op) { return op.getLowIndex(); })
            .Default([](auto) { return Value{}; });
    reversed.push_back({op, index});
    ref = op->getOperand(0);
  }
  projections.append(reversed.rbegin(), reversed.rend());
  return ref;
}

/// Adjust the width of an unsigned integer `value` to `width` bits, either by
/// zero-extending or by truncating it.
static Value zextOrTrunc(OpBuilder &builder, Location loc, Value value,
                         unsigned width) {
  auto valueWidth = cast<IntegerType>(value.getType()).getWidth();
  if (valueWidth == width)
    return value;
  if (valueWidth > width)
    return comb::ExtractOp::create(builder, loc, value, 0, width);
  auto zeros = hw::ConstantOp::create(
      builder, loc, builder.getIntegerType(width - valueWidth), 0);
  return comb::ConcatOp::create(builder, loc, zeros, value);
}

/// Compute the bit offset `index * stride` as an integer of `width` bits, which
/// must be wide enough to hold any in-bounds offset. Indices that are out of
/// bounds produce an offset of at least `width`, such that shifts by the offset
/// produce all zeros.
static Value computeBitOffset(OpBuilder &builder, Location loc, Value index,
                              unsigned stride, unsigned width) {
  // Compute the offset with enough bits to never overflow, then saturate it
  // to the target width.
  auto indexWidth = cast<IntegerType>(index.getType()).getWidth();
  auto strideWidth = llvm::Log2_64_Ceil(stride + 1);
  auto wideWidth = std::max(width, indexWidth + strideWidth) + 1;
  Value offset = zextOrTrunc(builder, loc, index, wideWidth);
  if (stride != 1) {
    auto strideValue = hw::ConstantOp::create(
        builder, loc, builder.getIntegerType(wideWidth), stride);
    offset = comb::MulOp::create(builder, loc, offset, strideValue);
  }
  if (wideWidth == width)
    return offset;
  auto limit = hw::ConstantOp::create(builder, loc, APInt(wideWidth, width));
  auto inBounds = comb::ICmpOp::create(builder, loc, comb::ICmpPredicate::ult,
                                       offset, limit);
  auto truncated = comb::ExtractOp::create(builder, loc, offset, 0, width);
  // The callers pick a width that can represent offsets beyond the last bit,
  // such that an all-ones offset shifts all bits out.
  auto saturated =
      hw::ConstantOp::create(builder, loc, APInt::getAllOnes(width));
  return comb::MuxOp::create(builder, loc, inBounds, truncated, saturated);
}

/// Extract `width` bits from an integer `value` starting at a dynamic bit
/// `offset`. Bits beyond the end of the value read as zero.
static Value extractBits(OpBuilder &builder, Location loc, Value value,
                         Value offset, unsigned width) {
  auto valueWidth = cast<IntegerType>(value.getType()).getWidth();
  // Widen the value such that the shift amount can address all bits.
  auto offsetWidth = cast<IntegerType>(offset.getType()).getWidth();
  auto wideWidth = std::max(valueWidth, offsetWidth);
  Value wide = zextOrTrunc(builder, loc, value, wideWidth);
  Value amount = zextOrTrunc(builder, loc, offset, wideWidth);
  Value shifted = comb::ShrUOp::create(builder, loc, wide, amount);
  return comb::ExtractOp::create(builder, loc, shifted, 0, width);
}

/// Replace `width` bits of an integer `value` starting at a dynamic bit
/// `offset` with the bits of `newBits`. Bits beyond the end of the value are
/// discarded.
static Value insertBits(OpBuilder &builder, Location loc, Value value,
                        Value offset, Value newBits) {
  auto valueWidth = cast<IntegerType>(value.getType()).getWidth();
  auto width = cast<IntegerType>(newBits.getType()).getWidth();
  auto offsetWidth = cast<IntegerType>(offset.getType()).getWidth();
  auto wideWidth = std::max(valueWidth, offsetWidth);
  Value wide = zextOrTrunc(builder, loc, value, wideWidth);
  Value amount = zextOrTrunc(builder, loc, offset, wideWidth);
  auto ones = hw::ConstantOp::create(builder, loc,
                                     APInt::getLowBitsSet(wideWidth, width));
  Value mask = comb::ShlOp::create(builder, loc, ones, amount);
  auto allOnes =
      hw::ConstantOp::create(builder, loc, APInt::getAllOnes(wideWidth));
  Value invMask = comb::XorOp::create(builder, loc, mask, allOnes);
  Value cleared = comb::AndOp::create(builder, loc, wide, invMask);
  Value inserted = comb::ShlOp::create(
      builder, loc, zextOrTrunc(builder, loc, newBits, wideWidth), amount);
  Value result = comb::OrOp::create(builder, loc, cleared, inserted);
  return zextOrTrunc(builder, loc, result, valueWidth);
}

/// Apply a single projection to a value.
static Value projectOne(OpBuilder &builder, Location loc, Value value,
                        const Projection &proj) {
  auto resultType =
      cast<llhd::RefType>(proj.op->getResult(0).getType()).getNestedType();
  return TypeSwitch<Operation *, Value>(proj.op)
      .Case<llhd::SigExtractOp>([&](auto op) -> Value {
        auto width = cast<IntegerType>(resultType).getWidth();
        return extractBits(builder, loc, value, proj.index, width);
      })
      .Case<llhd::SigArrayGetOp>([&](auto op) -> Value {
        return hw::ArrayGetOp::create(builder, loc, value, proj.index);
      })
      .Case<llhd::SigArraySliceOp>([&](auto op) -> Value {
        return hw::ArraySliceOp::create(builder, loc, resultType, value,
                                        proj.index);
      })
      .Case<llhd::SigStructExtractOp>([&](auto op) -> Value {
        if (isa<hw::UnionType>(value.getType()))
          return hw::UnionExtractOp::create(builder, loc, value,
                                            op.getFieldAttr());
        return hw::StructExtractOp::create(builder, loc, value,
                                           op.getFieldAttr());
      });
}

/// Replace the part of a value designated by a single projection.
static Value injectOne(OpBuilder &builder, Location loc, Value value,
                       const Projection &proj, Value newValue) {
  return TypeSwitch<Operation *, Value>(proj.op)
      .Case<llhd::SigExtractOp>([&](auto op) -> Value {
        return insertBits(builder, loc, value, proj.index, newValue);
      })
      .Case<llhd::SigArrayGetOp>([&](auto op) -> Value {
        return hw::ArrayInjectOp::create(builder, loc, value, proj.index,
                                         newValue);
      })
      .Case<llhd::SigArraySliceOp>([&](auto op) -> Value {
        // There is no array slice injection op. Insert the slice into the bits
        // of the array instead. Element 0 is located at the least significant
        // bits of the array.
        auto arrayType = cast<hw::ArrayType>(value.getType());
        auto elementWidth = hw::getBitWidth(arrayType.getElementType());
        auto bits = toBits(builder, loc, value);
        auto bitsWidth = cast<IntegerType>(bits.getType()).getWidth();
        auto offset = computeBitOffset(builder, loc, proj.index, elementWidth,
                                       llvm::Log2_64_Ceil(bitsWidth + 1));
        auto result = insertBits(builder, loc, bits, offset,
                                 toBits(builder, loc, newValue));
        return fromBits(builder, loc, result, arrayType);
      })
      .Case<llhd::SigStructExtractOp>([&](auto op) -> Value {
        if (auto unionType = dyn_cast<hw::UnionType>(value.getType()))
          return hw::UnionCreateOp::create(builder, loc, unionType,
                                           op.getFieldAttr(), newValue);
        return hw::StructInjectOp::create(builder, loc, value,
                                          op.getFieldAttr(), newValue);
      });
}

Value signals::project(OpBuilder &builder, Location loc, Value root,
                       ArrayRef<Projection> projections) {
  for (auto &proj : projections)
    root = projectOne(builder, loc, root, proj);
  return root;
}

Value signals::inject(OpBuilder &builder, Location loc, Value root,
                      ArrayRef<Projection> projections, Value value) {
  if (projections.empty())
    return value;

  // Compute the intermediate values along the projection chain.
  SmallVector<Value> parts;
  parts.push_back(root);
  for (auto &proj : projections.drop_back())
    parts.push_back(projectOne(builder, loc, parts.back(), proj));

  // Inject the new value into each intermediate value, from the innermost
  // projection outward.
  for (auto [part, proj] : llvm::reverse(llvm::zip_equal(parts, projections)))
    value = injectOne(builder, loc, part, proj, value);
  return value;
}

bool signals::hasBitRepresentation(Type type) {
  if (isa<FloatType>(type))
    return true;
  return hw::getBitWidth(type) >= 0;
}

bool signals::isSupportedType(Type type) {
  return hasBitRepresentation(type) || isa<sim::DynamicStringType>(type);
}

bool signals::isOpaque(Type type) {
  return isa<FloatType, sim::DynamicStringType>(type);
}

unsigned signals::getMaskWidth(Type type) {
  return isOpaque(type) ? 1 : hw::getBitWidth(type);
}

Value signals::emitChanged(OpBuilder &builder, Location loc, Value lhs,
                           Value rhs) {
  if (isa<sim::DynamicStringType>(lhs.getType()))
    return sim::StringCmpOp::create(builder, loc, sim::StringCmpPredicate::ne,
                                    lhs, rhs);
  return comb::ICmpOp::create(builder, loc, comb::ICmpPredicate::ne,
                              toBits(builder, loc, lhs),
                              toBits(builder, loc, rhs));
}

Value signals::toBits(OpBuilder &builder, Location loc, Value value) {
  auto type = value.getType();
  if (isa<IntegerType>(type))
    return value;
  if (auto floatType = dyn_cast<FloatType>(type))
    return arith::BitcastOp::create(
        builder, loc, builder.getIntegerType(floatType.getWidth()), value);
  auto width = hw::getBitWidth(type);
  assert(width >= 0 && "type has no bit representation");
  return hw::BitcastOp::create(builder, loc, builder.getIntegerType(width),
                               value);
}

Value signals::fromBits(OpBuilder &builder, Location loc, Value bits,
                        Type type) {
  if (bits.getType() == type)
    return bits;
  if (isa<FloatType>(type))
    return arith::BitcastOp::create(builder, loc, type, bits);
  return hw::BitcastOp::create(builder, loc, type, bits);
}

Value signals::emitProbe(OpBuilder &builder, Location loc,
                         const SignalStorage &storage,
                         ArrayRef<Projection> projections) {
  Value value = StateReadOp::create(builder, loc, storage.value);
  return project(builder, loc, value, projections);
}

void signals::emitScheduleWakeup(OpBuilder &builder, Location loc,
                                 Value arcContext, Value time) {
  auto nextWakeup = GetNextWakeupOp::create(builder, loc, arcContext);
  auto minWakeup = arith::MinUIOp::create(builder, loc, nextWakeup, time);
  SetNextWakeupOp::create(builder, loc, arcContext, minWakeup);
}

void signals::emitDrive(OpBuilder &builder, Location loc, Value arcContext,
                        const SignalStorage &storage,
                        ArrayRef<Projection> projections, Value value,
                        Value delay, Value changeFlag) {
  auto type = cast<StateType>(storage.value.getType()).getType();

  // Immediately update the signal value if the drive has no delay, and request
  // a re-evaluation of the model if the value changed.
  if (!delay) {
    auto oldValue = StateReadOp::create(builder, loc, storage.value);
    auto oldPart = project(builder, loc, oldValue, projections);
    auto newValue = inject(builder, loc, oldValue, projections, value);
    StateWriteOp::create(builder, loc, storage.value, newValue);
    auto changed = emitChanged(builder, loc, oldPart, value);
    auto ifChangedOp = scf::IfOp::create(builder, loc, changed, false);
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(ifChangedOp.thenYield());
    auto now = CurrentTimeOp::create(builder, loc, arcContext);
    emitScheduleWakeup(builder, loc, arcContext, now);
    if (changeFlag)
      StateWriteOp::create(
          builder, loc, changeFlag,
          hw::ConstantOp::create(builder, loc, builder.getI1Type(), 1));
    return;
  }

  // Otherwise update the signal's shadow storage, which will be applied once
  // the delay has elapsed.
  auto oldNext = StateReadOp::create(builder, loc, storage.next);
  auto newNext = inject(builder, loc, oldNext, projections, value);
  StateWriteOp::create(builder, loc, storage.next, newNext);

  // Mark the driven bits as pending.
  Value partMask;
  auto maskType = cast<StateType>(storage.mask.getType()).getType();
  if (isOpaque(type)) {
    partMask = hw::ConstantOp::create(builder, loc, maskType, 1);
  } else {
    auto zeros = fromBits(
        builder, loc, hw::ConstantOp::create(builder, loc, maskType, 0), type);
    auto partWidth = hw::getBitWidth(value.getType());
    auto ones = fromBits(
        builder, loc,
        hw::ConstantOp::create(builder, loc, APInt::getAllOnes(partWidth)),
        value.getType());
    partMask =
        toBits(builder, loc, inject(builder, loc, zeros, projections, ones));
  }
  auto oldMask = StateReadOp::create(builder, loc, storage.mask);
  auto newMask = comb::OrOp::create(builder, loc, oldMask, partMask);
  StateWriteOp::create(builder, loc, storage.mask, newMask);

  // Record the time at which the pending value takes effect.
  auto now = CurrentTimeOp::create(builder, loc, arcContext);
  auto time = comb::AddOp::create(builder, loc, now, delay);
  StateWriteOp::create(builder, loc, storage.time, time);
}
