//===- SignalUtils.h - Helpers for lowering LLHD signals --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Utilities to lower accesses to LLHD signals into reads and writes of the
// signal's entire value. References to parts of a signal are expressed as a
// chain of projections (`llhd.sig.extract`, `llhd.sig.array_get`,
// `llhd.sig.array_slice`, and `llhd.sig.struct_extract`) rooted at the signal.
// Probing such a reference reads the root value and applies the projections to
// the value, while driving it computes a new root value with the projected part
// replaced.
//
//===----------------------------------------------------------------------===//

#ifndef LIB_DIALECT_ARC_TRANSFORMS_SIGNALUTILS_H
#define LIB_DIALECT_ARC_TRANSFORMS_SIGNALUTILS_H

#include "circt/Support/LLVM.h"
#include "mlir/IR/Builders.h"

namespace circt {
namespace arc {
namespace signals {

/// A single projection step from a reference into a part of it.
struct Projection {
  /// The `llhd.sig.*` projection op.
  Operation *op;
  /// The dynamic index of the projection, already remapped to be usable at the
  /// insertion point. Null for struct field projections.
  Value index;
};

/// The storage allocated for an LLHD signal. Signals that are driven with a
/// delay additionally carry shadow storage that holds the pending value, a bit
/// mask indicating which bits of the pending value are valid, and the time at
/// which the pending value takes effect.
struct SignalStorage {
  /// The current value of the signal.
  Value value;
  /// The pending value of the signal. Null if not allocated.
  Value next;
  /// A bit mask of the pending bits in `next`. A non-zero mask indicates that a
  /// pending value is scheduled. Null if not allocated.
  Value mask;
  /// The time at which the pending value takes effect. Null if not allocated.
  Value time;
};

/// Check whether an op is a projection from a reference into a part of it.
bool isProjection(Operation *op);

/// Strip projections from a reference until the root reference is reached.
/// The projection ops are added to `projections` in order from the root
/// outward. The dynamic index operands of the projections are not remapped.
Value getRootRef(Value ref, SmallVectorImpl<Projection> &projections);

/// Compute the part of the `root` value designated by the `projections`.
Value project(OpBuilder &builder, Location loc, Value root,
              ArrayRef<Projection> projections);

/// Compute a new version of the `root` value where the part designated by the
/// `projections` is replaced with `value`.
Value inject(OpBuilder &builder, Location loc, Value root,
             ArrayRef<Projection> projections, Value value);

/// Read the part of a signal designated by the `projections`.
Value emitProbe(OpBuilder &builder, Location loc, const SignalStorage &storage,
                ArrayRef<Projection> projections);

/// Request a wakeup of the model at the given time, unless an earlier wakeup
/// has already been requested.
void emitScheduleWakeup(OpBuilder &builder, Location loc, Value arcContext,
                        Value time);

/// Drive the part of a signal designated by the `projections` with `value`. If
/// `delay` is null, the signal's value is updated immediately. Otherwise
/// `delay` is an `i64` number of femtoseconds after which the value takes
/// effect, and the drive updates the signal's shadow storage. A change of the
/// signal's value requests a wakeup of the model at the current time, and sets
/// the optional `changeFlag` state.
void emitDrive(OpBuilder &builder, Location loc, Value arcContext,
               const SignalStorage &storage, ArrayRef<Projection> projections,
               Value value, Value delay, Value changeFlag = {});

/// Check whether values of the given type can be converted to and from a flat
/// integer with `toBits` and `fromBits`.
bool hasBitRepresentation(Type type);

/// Check whether values of the given type can be held in signals and compared
/// with `emitChanged`. This includes all types with a bit representation, as
/// well as dynamic strings.
bool isSupportedType(Type type);

/// Check whether values of the given type are always updated as a whole, such
/// that the pending bits of a delayed drive are tracked with a single bit.
bool isOpaque(Type type);

/// Get the width of the mask tracking the pending bits of a signal.
unsigned getMaskWidth(Type type);

/// Emit a check whether two values of a supported type differ.
Value emitChanged(OpBuilder &builder, Location loc, Value lhs, Value rhs);

/// Convert a value to a flat integer of the same bit width.
Value toBits(OpBuilder &builder, Location loc, Value value);

/// Convert a flat integer back to a value of the given type.
Value fromBits(OpBuilder &builder, Location loc, Value bits, Type type);

} // namespace signals
} // namespace arc
} // namespace circt

#endif // LIB_DIALECT_ARC_TRANSFORMS_SIGNALUTILS_H
