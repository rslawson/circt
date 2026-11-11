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

/// Arrays wider than this many bits are compared and stored elementwise,
/// rather than as a single flat integer.
static constexpr int64_t kMaxFlatArrayWidth = 4096;

/// Check whether an array is too large to be handled as a flat integer.
static bool isLargeArray(Type type) {
  return isa<hw::ArrayType>(type) && hw::getBitWidth(type) > kMaxFlatArrayWidth;
}

bool signals::isElementwise(Type type) {
  if (!isLargeArray(type))
    return false;
  return hasBitRepresentation(cast<hw::ArrayType>(type).getElementType());
}

Type signals::getMaskType(Type type) {
  auto *context = type.getContext();
  if (isOpaque(type))
    return IntegerType::get(context, 1);
  if (isElementwise(type)) {
    auto arrayType = cast<hw::ArrayType>(type);
    return hw::ArrayType::get(
        IntegerType::get(context, hw::getBitWidth(arrayType.getElementType())),
        arrayType.getNumElements());
  }
  return IntegerType::get(context, hw::getBitWidth(type));
}

SmallVector<Type, 6> signals::getStorageTypes(Type type) {
  auto i64Type = IntegerType::get(type.getContext(), 64);
  SmallVector<Type, 6> types{StateType::get(type), StateType::get(type),
                             StateType::get(getMaskType(type)),
                             StateType::get(i64Type)};
  if (isElementwise(type))
    types.append({StateType::get(i64Type), StateType::get(i64Type)});
  return types;
}

SmallVector<Value, 6> SignalStorage::getStates() const {
  SmallVector<Value, 6> states;
  for (auto state : {value, next, mask, time, low, high})
    if (state)
      states.push_back(state);
  return states;
}

SignalStorage SignalStorage::fromStates(ValueRange states) {
  SignalStorage storage;
  Value *fields[] = {&storage.value, &storage.next, &storage.mask,
                     &storage.time,  &storage.low,  &storage.high};
  for (auto [field, state] : llvm::zip(fields, states))
    *field = state;
  return storage;
}

/// Create a loop over the elements of an array with `numElements` elements,
/// accumulating an `i1` with a logical OR. The `body` is called with the
/// builder, the element index as `i64`, and the element index as an integer
/// suitable for indexing into the array, and returns the `i1` for the element.
static Value
emitAnyElement(OpBuilder &builder, Location loc, uint64_t numElements,
               Value lowBound, Value highBound,
               llvm::function_ref<Value(OpBuilder &, Value, Value)> body) {
  auto i64Type = builder.getI64Type();
  auto indexWidth = llvm::Log2_64_Ceil(numElements);
  if (!lowBound)
    lowBound = hw::ConstantOp::create(builder, loc, i64Type, 0);
  if (!highBound)
    highBound = hw::ConstantOp::create(builder, loc, i64Type, numElements);
  auto step = hw::ConstantOp::create(builder, loc, i64Type, 1);
  auto falseValue =
      hw::ConstantOp::create(builder, loc, builder.getI1Type(), 0);
  auto forOp = scf::ForOp::create(builder, loc, lowBound, highBound, step,
                                  ValueRange{falseValue});
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(forOp.getBody());
  Value i64Index = forOp.getInductionVar();
  Value index = i64Index;
  if (indexWidth < 64)
    index = comb::ExtractOp::create(builder, loc, i64Index, 0,
                                    std::max(indexWidth, 1u));
  auto element = body(builder, i64Index, index);
  auto any =
      comb::OrOp::create(builder, loc, forOp.getRegionIterArg(0), element);
  scf::YieldOp::create(builder, loc, ValueRange{any});
  return forOp.getResult(0);
}

Value signals::emitChanged(OpBuilder &builder, Location loc, Value lhs,
                           Value rhs) {
  if (isa<sim::DynamicStringType>(lhs.getType()))
    return sim::StringCmpOp::create(builder, loc, sim::StringCmpPredicate::ne,
                                    lhs, rhs);
  // Compare large arrays elementwise in a loop, rather than as huge integers.
  if (isLargeArray(lhs.getType())) {
    auto arrayType = cast<hw::ArrayType>(lhs.getType());
    return emitAnyElement(
        builder, loc, arrayType.getNumElements(), {}, {},
        [&](OpBuilder &builder, Value, Value index) {
          return emitChanged(builder, loc,
                             hw::ArrayGetOp::create(builder, loc, lhs, index),
                             hw::ArrayGetOp::create(builder, loc, rhs, index));
        });
  }
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

/// Check whether the first projection selects an element of an elementwise
/// signal, which can be accessed without copying the entire signal.
static bool isElementAccess(const SignalStorage &storage,
                            ArrayRef<Projection> projections) {
  return !projections.empty() &&
         isa<llhd::SigArrayGetOp>(projections.front().op) &&
         isElementwise(cast<StateType>(storage.value.getType()).getType());
}

Value signals::emitProbe(OpBuilder &builder, Location loc,
                         const SignalStorage &storage,
                         ArrayRef<Projection> projections) {
  if (isElementAccess(storage, projections)) {
    auto elementType =
        cast<hw::ArrayType>(cast<StateType>(storage.value.getType()).getType())
            .getElementType();
    Value element = StateReadElementOp::create(
        builder, loc, elementType, storage.value, projections.front().index);
    return project(builder, loc, element, projections.drop_front());
  }
  Value value = StateReadOp::create(builder, loc, storage.value);
  return project(builder, loc, value, projections);
}

void signals::emitScheduleWakeup(OpBuilder &builder, Location loc,
                                 Value arcContext, Value time) {
  auto nextWakeup = GetNextWakeupOp::create(builder, loc, arcContext);
  auto minWakeup = arith::MinUIOp::create(builder, loc, nextWakeup, time);
  SetNextWakeupOp::create(builder, loc, arcContext, minWakeup);
}

/// Compute the bit mask of the bits of a value of type `type` designated by the
/// `projections`.
static Value computePartMask(OpBuilder &builder, Location loc, Type type,
                             ArrayRef<Projection> projections, Type partType) {
  auto maskType = cast<IntegerType>(getMaskType(type));
  if (isOpaque(type))
    return hw::ConstantOp::create(builder, loc, maskType, 1);
  auto zeros = fromBits(
      builder, loc, hw::ConstantOp::create(builder, loc, maskType, 0), type);
  auto partWidth = hw::getBitWidth(partType);
  auto ones = fromBits(
      builder, loc,
      hw::ConstantOp::create(builder, loc, APInt::getAllOnes(partWidth)),
      partType);
  return toBits(builder, loc, inject(builder, loc, zeros, projections, ones));
}

/// Request a re-evaluation of the model in the current time step if `changed`
/// is set, and set the optional `changeFlag` state.
static void emitIfChanged(OpBuilder &builder, Location loc, Value arcContext,
                          Value changed, Value changeFlag) {
  auto ifChangedOp = scf::IfOp::create(builder, loc, changed, false);
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPoint(ifChangedOp.thenYield());
  auto now = CurrentTimeOp::create(builder, loc, arcContext);
  emitScheduleWakeup(builder, loc, arcContext, now);
  if (changeFlag)
    StateWriteOp::create(
        builder, loc, changeFlag,
        hw::ConstantOp::create(builder, loc, builder.getI1Type(), 1));
}

/// Drive an element of an elementwise signal, without copying the signal.
static void emitElementDrive(OpBuilder &builder, Location loc, Value arcContext,
                             const SignalStorage &storage,
                             ArrayRef<Projection> projections, Value value,
                             Value delay, Value changeFlag) {
  auto arrayType =
      cast<hw::ArrayType>(cast<StateType>(storage.value.getType()).getType());
  auto elementType = arrayType.getElementType();
  auto index = projections.front().index;
  auto rest = projections.drop_front();

  if (!delay) {
    auto oldElement = StateReadElementOp::create(builder, loc, elementType,
                                                 storage.value, index);
    auto oldPart = project(builder, loc, oldElement, rest);
    auto newElement = inject(builder, loc, oldElement, rest, value);
    StateWriteElementOp::create(builder, loc, storage.value, index, newElement);
    emitIfChanged(builder, loc, arcContext,
                  emitChanged(builder, loc, oldPart, value), changeFlag);
    return;
  }

  // Ignore delayed drives beyond the end of the array, such that they don't
  // extend the range of pending elements.
  auto indexType = cast<IntegerType>(index.getType());
  auto i64Type = builder.getI64Type();
  Value i64Index = index;
  if (indexType.getWidth() < 64)
    i64Index = comb::ConcatOp::create(
        builder, loc,
        hw::ConstantOp::create(
            builder, loc, builder.getIntegerType(64 - indexType.getWidth()), 0),
        index);
  else if (indexType.getWidth() > 64)
    return; // Indices this wide are always out of bounds for the state.
  auto numElements =
      hw::ConstantOp::create(builder, loc, i64Type, arrayType.getNumElements());
  auto inBounds = comb::ICmpOp::create(builder, loc, comb::ICmpPredicate::ult,
                                       i64Index, numElements);
  auto ifOp = scf::IfOp::create(builder, loc, inBounds, false);
  OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPoint(ifOp.thenYield());

  // Update the pending value and mask of the element.
  auto oldNext = StateReadElementOp::create(builder, loc, elementType,
                                            storage.next, index);
  StateWriteElementOp::create(builder, loc, storage.next, index,
                              inject(builder, loc, oldNext, rest, value));
  auto maskElementType =
      cast<hw::ArrayType>(cast<StateType>(storage.mask.getType()).getType())
          .getElementType();
  auto oldMask = StateReadElementOp::create(builder, loc, maskElementType,
                                            storage.mask, index);
  auto partMask =
      computePartMask(builder, loc, elementType, rest, value.getType());
  StateWriteElementOp::create(
      builder, loc, storage.mask, index,
      comb::OrOp::create(builder, loc, oldMask, partMask));

  // Extend the range of pending elements.
  auto oldLow = StateReadOp::create(builder, loc, storage.low);
  auto oldHigh = StateReadOp::create(builder, loc, storage.high);
  auto zero = hw::ConstantOp::create(builder, loc, i64Type, 0);
  auto one = hw::ConstantOp::create(builder, loc, i64Type, 1);
  auto isEmpty = comb::ICmpOp::create(builder, loc, comb::ICmpPredicate::eq,
                                      oldHigh, zero);
  auto newLow = comb::MuxOp::create(
      builder, loc, isEmpty, i64Index,
      arith::MinUIOp::create(builder, loc, oldLow, i64Index));
  auto newHigh = arith::MaxUIOp::create(
      builder, loc, oldHigh, comb::AddOp::create(builder, loc, i64Index, one));
  StateWriteOp::create(builder, loc, storage.low, newLow);
  StateWriteOp::create(builder, loc, storage.high, newHigh);

  // Record the time at which the pending value takes effect.
  auto now = CurrentTimeOp::create(builder, loc, arcContext);
  auto time = comb::AddOp::create(builder, loc, now, delay);
  StateWriteOp::create(builder, loc, storage.time, time);
}

void signals::emitDrive(OpBuilder &builder, Location loc, Value arcContext,
                        const SignalStorage &storage,
                        ArrayRef<Projection> projections, Value value,
                        Value delay, Value changeFlag) {
  auto type = cast<StateType>(storage.value.getType()).getType();
  if (isElementAccess(storage, projections))
    return emitElementDrive(builder, loc, arcContext, storage, projections,
                            value, delay, changeFlag);

  // Immediately update the signal value if the drive has no delay, and request
  // a re-evaluation of the model if the value changed.
  if (!delay) {
    auto oldValue = StateReadOp::create(builder, loc, storage.value);
    auto oldPart = project(builder, loc, oldValue, projections);
    auto newValue = inject(builder, loc, oldValue, projections, value);
    StateWriteOp::create(builder, loc, storage.value, newValue);
    emitIfChanged(builder, loc, arcContext,
                  emitChanged(builder, loc, oldPart, value), changeFlag);
    return;
  }

  // Otherwise update the signal's shadow storage, which will be applied once
  // the delay has elapsed.
  auto oldNext = StateReadOp::create(builder, loc, storage.next);
  auto newNext = inject(builder, loc, oldNext, projections, value);
  StateWriteOp::create(builder, loc, storage.next, newNext);

  // Mark the driven bits as pending. For elementwise signals, this marks all
  // elements, since only element accesses are tracked individually.
  if (isElementwise(type)) {
    auto arrayType = cast<hw::ArrayType>(type);
    auto maskElementType =
        cast<hw::ArrayType>(getMaskType(type)).getElementType();
    auto ones = hw::ConstantOp::create(
        builder, loc,
        APInt::getAllOnes(cast<IntegerType>(maskElementType).getWidth()));
    emitAnyElement(
        builder, loc, arrayType.getNumElements(), {}, {},
        [&](OpBuilder &builder, Value, Value index) -> Value {
          StateWriteElementOp::create(builder, loc, storage.mask, index, ones);
          return hw::ConstantOp::create(builder, loc, builder.getI1Type(), 0);
        });
    auto i64Type = builder.getI64Type();
    StateWriteOp::create(builder, loc, storage.low,
                         hw::ConstantOp::create(builder, loc, i64Type, 0));
    StateWriteOp::create(builder, loc, storage.high,
                         hw::ConstantOp::create(builder, loc, i64Type,
                                                arrayType.getNumElements()));
  } else {
    auto partMask =
        computePartMask(builder, loc, type, projections, value.getType());
    auto oldMask = StateReadOp::create(builder, loc, storage.mask);
    auto newMask = comb::OrOp::create(builder, loc, oldMask, partMask);
    StateWriteOp::create(builder, loc, storage.mask, newMask);
  }

  // Record the time at which the pending value takes effect.
  auto now = CurrentTimeOp::create(builder, loc, arcContext);
  auto time = comb::AddOp::create(builder, loc, now, delay);
  StateWriteOp::create(builder, loc, storage.time, time);
}

Value signals::emitApplyPendingElements(OpBuilder &builder, Location loc,
                                        const SignalStorage &storage) {
  auto arrayType =
      cast<hw::ArrayType>(cast<StateType>(storage.value.getType()).getType());
  auto elementType = arrayType.getElementType();
  auto maskElementType =
      cast<hw::ArrayType>(cast<StateType>(storage.mask.getType()).getType())
          .getElementType();
  auto low = StateReadOp::create(builder, loc, storage.low);
  auto high = StateReadOp::create(builder, loc, storage.high);
  auto changed = emitAnyElement(
      builder, loc, arrayType.getNumElements(), low, high,
      [&](OpBuilder &builder, Value, Value index) -> Value {
        auto mask = StateReadElementOp::create(builder, loc, maskElementType,
                                               storage.mask, index);
        auto zeroMask =
            hw::ConstantOp::create(builder, loc, maskElementType, 0);
        auto pending = comb::ICmpOp::create(
            builder, loc, comb::ICmpPredicate::ne, mask, zeroMask);
        auto ifOp =
            scf::IfOp::create(builder, loc, builder.getI1Type(), pending, true);
        {
          OpBuilder::InsertionGuard guard(builder);
          builder.setInsertionPointToStart(ifOp.thenBlock());
          auto oldElement = StateReadElementOp::create(
              builder, loc, elementType, storage.value, index);
          auto nextElement = StateReadElementOp::create(
              builder, loc, elementType, storage.next, index);
          auto oldBits = toBits(builder, loc, oldElement);
          auto allOnes = hw::ConstantOp::create(
              builder, loc,
              APInt::getAllOnes(cast<IntegerType>(maskElementType).getWidth()));
          auto keptBits = comb::AndOp::create(
              builder, loc, oldBits,
              comb::XorOp::create(builder, loc, mask, allOnes));
          auto newBits = comb::AndOp::create(
              builder, loc, toBits(builder, loc, nextElement), mask);
          auto bits = comb::OrOp::create(builder, loc, keptBits, newBits);
          StateWriteElementOp::create(
              builder, loc, storage.value, index,
              fromBits(builder, loc, bits, elementType));
          StateWriteElementOp::create(builder, loc, storage.mask, index,
                                      zeroMask);
          auto elementChanged = comb::ICmpOp::create(
              builder, loc, comb::ICmpPredicate::ne, oldBits, bits);
          scf::YieldOp::create(builder, loc, ValueRange{elementChanged});
        }
        {
          OpBuilder::InsertionGuard guard(builder);
          builder.setInsertionPointToStart(ifOp.elseBlock());
          auto falseValue =
              hw::ConstantOp::create(builder, loc, builder.getI1Type(), 0);
          scf::YieldOp::create(builder, loc, ValueRange{falseValue});
        }
        return ifOp.getResult(0);
      });

  // Clear the range of pending elements.
  auto zero = hw::ConstantOp::create(builder, loc, builder.getI64Type(), 0);
  StateWriteOp::create(builder, loc, storage.low, zero);
  StateWriteOp::create(builder, loc, storage.high, zero);
  return changed;
}
