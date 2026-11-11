// RUN: circt-opt --arc-lower-processes %s | FileCheck %s

// A process that reads an input port, suspends twice, and halts.

// The coroutine results are the process result, an `i1` observe bitmask (one
// bit per coroutine argument: just `%a`), and the `i64` wakeup time. These
// waits observe nothing, so the mask is `0`. The current simulation time is
// read from the model context via `arc.inferred_context` at each suspension.

// CHECK-LABEL: arc.coroutine.define @Foo.llhd.process
// CHECK-SAME: ([[ENTRY_A:%.+]]: i32) -> (i32, i1, i64)
// CHECK:   [[CTX:%.+]] = arc.inferred_context
// CHECK:   [[C1:%.+]] = hw.constant 1 : i32
// CHECK:   [[T1:%.+]] = llhd.constant_time <42000000fs
// CHECK:   [[T2:%.+]] = llhd.constant_time <11000000fs
// CHECK:   [[C2:%.+]] = hw.constant 2 : i32
// CHECK:   [[NOW1:%.+]] = arc.current_time [[CTX]]
// CHECK:   [[D1:%.+]] = llhd.time_to_int [[T1]]
// CHECK:   [[W1:%.+]] = comb.add [[NOW1]], [[D1]] : i64
// CHECK:   arc.coroutine.yield ([[C1]], {{%.+}}, [[W1]] : i32, i1, i64), ^[[BB1:.+]]
// CHECK: ^[[BB1]]([[A1:%.+]]: i32):
// CHECK:   [[NOW2:%.+]] = arc.current_time [[CTX]]
// CHECK:   [[D2:%.+]] = llhd.time_to_int [[T2]]
// CHECK:   [[W2:%.+]] = comb.add [[NOW2]], [[D2]] : i64
// CHECK:   arc.coroutine.yield ([[A1]], {{%.+}}, [[W2]] : i32, i1, i64), ^[[BB2:.+]]
// CHECK: ^[[BB2]]({{%.+}}: i32):
// CHECK:   [[NEVER:%.+]] = hw.constant -1 : i64
// CHECK:   arc.coroutine.halt [[C2]], {{%.+}}, [[NEVER]] : i32, i1, i64

// CHECK-LABEL: hw.module @Foo
// CHECK-SAME:    (in %a : i32, out x : i32)
hw.module @Foo(in %a: i32, out x: i32) {
  // CHECK: [[OUT:%.+]] = arc.coroutine.instance @Foo.llhd.process(%a) sensitive [false] : (i32) -> i32
  // CHECK: hw.output [[OUT]]
  %c1_i32 = hw.constant 1 : i32
  %c2_i32 = hw.constant 2 : i32
  %0 = llhd.constant_time <42000000fs, 0d>
  %1 = llhd.constant_time <11000000fs, 0d>
  %2 = llhd.process -> i32 {
    llhd.wait yield (%c1_i32 : i32), delay %0, ^bb1
  ^bb1:
    llhd.wait yield (%a : i32), delay %1, ^bb2
  ^bb2:
    llhd.halt %c2_i32 : i32
  }
  hw.output %2 : i32
}

//===----------------------------------------------------------------------===//
// Process with no results: function type is `() -> (i0, i64)`. With no captures
// the coroutine takes no arguments and the observe bitmask is the zero-width
// `i0`.

// CHECK-LABEL: arc.coroutine.define @NoResults.llhd.process
// CHECK-SAME: () -> (i0, i64)
// CHECK:   arc.coroutine.yield ({{%.+}}, {{%.+}} : i0, i64), ^[[BB:.+]]
// CHECK: ^[[BB]]:
// CHECK:   arc.coroutine.halt {{%.+}}, {{%.+}} : i0, i64

// CHECK-LABEL: hw.module @NoResults
hw.module @NoResults() {
  // CHECK: arc.coroutine.instance @NoResults.llhd.process() sensitive [] : () -> ()
  %t = llhd.constant_time <1ns, 0d>
  llhd.process {
    llhd.wait delay %t, ^bb1
  ^bb1:
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// All values the body references are constants defined outside; they get
// cloned in and the coroutine signature has no captures.

// CHECK-LABEL: arc.coroutine.define @NoCaptures.llhd.process
// CHECK-SAME: () -> (i32, i0, i64)

// CHECK-LABEL: hw.module @NoCaptures
hw.module @NoCaptures(out o: i32) {
  // CHECK: arc.coroutine.instance @NoCaptures.llhd.process() sensitive [] : () -> i32
  %c7 = hw.constant 7 : i32
  %t = llhd.constant_time <5ns, 0d>
  %r = llhd.process -> i32 {
    llhd.wait yield (%c7 : i32), delay %t, ^bb1
  ^bb1:
    llhd.halt %c7 : i32
  }
  hw.output %r : i32
}

//===----------------------------------------------------------------------===//
// Process with multiple results.

// CHECK-LABEL: arc.coroutine.define @MultiResult.llhd.process
// CHECK-SAME: () -> (i32, i8, i0, i64)
// CHECK: arc.coroutine.yield ({{%.+}}, {{%.+}}, {{%.+}}, {{%.+}} : i32, i8, i0, i64), ^{{.+}}
// CHECK: arc.coroutine.halt {{%.+}}, {{%.+}}, {{%.+}}, {{%.+}} : i32, i8, i0, i64

// CHECK-LABEL: hw.module @MultiResult
hw.module @MultiResult(out a: i32, out b: i8) {
  // CHECK: arc.coroutine.instance @MultiResult.llhd.process() sensitive [] : () -> (i32, i8)
  %c32 = hw.constant 9 : i32
  %c8 = hw.constant 3 : i8
  %t = llhd.constant_time <1ns, 0d>
  %x, %y = llhd.process -> i32, i8 {
    llhd.wait yield (%c32, %c8 : i32, i8), delay %t, ^bb1
  ^bb1:
    llhd.halt %c32, %c8 : i32, i8
  }
  hw.output %x, %y : i32, i8
}

//===----------------------------------------------------------------------===//
// Multiple non-constant captures (both module input ports) plus an outside
// constant. Constants are cloned in; both ports are threaded as args.

// CHECK-LABEL: arc.coroutine.define @MultiCapture.llhd.process
// CHECK-SAME: ({{%.+}}: i32, {{%.+}}: i8) -> (i32, i2, i64)

// CHECK-LABEL: hw.module @MultiCapture
hw.module @MultiCapture(in %a: i32, in %b: i8, out o: i32) {
  // CHECK: arc.coroutine.instance @MultiCapture.llhd.process(%a, %b) sensitive [false, false] : (i32, i8) -> i32
  %c1 = hw.constant 1 : i32
  %t = llhd.constant_time <1ns, 0d>
  %res = llhd.process -> i32 {
    %sum = comb.concat %a, %b : i32, i8
    llhd.wait yield (%c1 : i32), delay %t, ^bb1
  ^bb1:
    llhd.halt %c1 : i32
  }
  hw.output %res : i32
}

//===----------------------------------------------------------------------===//
// `llhd.wait` with destOperands: the wait passes a value to the resume block
// as a regular block argument, which lands AFTER the coroutine's function-type
// prefix.

// CHECK-LABEL: arc.coroutine.define @WaitDestOperands.llhd.process
// CHECK-SAME: ({{%.+}}: i32) -> (i32, i1, i64)
// CHECK: arc.coroutine.yield ({{%.+}}, {{%.+}}, {{%.+}} : i32, i1, i64), ^[[RB:.+]]({{%.+}} : i32)
// CHECK: ^[[RB]]({{%.+}}: i32, [[X:%.+]]: i32):
// CHECK:   arc.coroutine.halt [[X]], {{%.+}}, {{%.+}} : i32, i1, i64

// CHECK-LABEL: hw.module @WaitDestOperands
hw.module @WaitDestOperands(in %a: i32, out o: i32) {
  %c0 = hw.constant 0 : i32
  %t = llhd.constant_time <1ns, 0d>
  %r = llhd.process -> i32 {
    %carry = comb.add %a, %c0 : i32
    llhd.wait yield (%c0 : i32), delay %t, ^bb1(%carry : i32)
  ^bb1(%x: i32):
    llhd.halt %x : i32
  }
  hw.output %r : i32
}

//===----------------------------------------------------------------------===//
// A `llhd.wait` with no delay and no observed values can never resume and is
// equivalent to an `llhd.halt`. The wait's yield operands become the halt's
// final yield.

// CHECK-LABEL: arc.coroutine.define @WaitWithoutDelay.llhd.process
// CHECK-SAME: () -> (i32, i0, i64)
// CHECK:   [[C:%.+]] = hw.constant 7 : i32
// CHECK:   [[N:%.+]] = hw.constant -1 : i64
// CHECK:   arc.coroutine.halt [[C]], {{%.+}}, [[N]] : i32, i0, i64

// CHECK-LABEL: hw.module @WaitWithoutDelay
hw.module @WaitWithoutDelay(out o: i32) {
  %c7 = hw.constant 7 : i32
  %r = llhd.process -> i32 {
    llhd.wait yield (%c7 : i32), ^bb1
  ^bb1:
    llhd.halt %c7 : i32
  }
  hw.output %r : i32
}

//===----------------------------------------------------------------------===//
// Same `llhd.constant_time` SSA value used by both waits is cloned once into
// the body; both waits share the cloned constant.

// CHECK-LABEL: arc.coroutine.define @SameDelayReused.llhd.process
// CHECK:       [[CT:%.+]] = llhd.constant_time
// CHECK-NOT:   llhd.constant_time
// CHECK:       llhd.time_to_int [[CT]]
// CHECK:       llhd.time_to_int [[CT]]

// CHECK-LABEL: hw.module @SameDelayReused
hw.module @SameDelayReused() {
  %t = llhd.constant_time <2ns, 0d>
  llhd.process {
    llhd.wait delay %t, ^bb1
  ^bb1:
    llhd.wait delay %t, ^bb2
  ^bb2:
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// Diamond CFG where one arm yields and the other doesn't. The join block is
// reached from two paths with different reaching defs of the capture, so the
// SSA renamer adds a fresh merge block argument and patches both branches.

// CHECK-LABEL: arc.coroutine.define @DiamondMerge.llhd.process
// CHECK-SAME: ([[A:%.+]]: i32) -> (i1, i64)
// CHECK:       cf.cond_br {{.+}}, ^[[LEFT:.+]], ^[[RIGHT:.+]]
// CHECK: ^[[LEFT]]:
// CHECK:       arc.coroutine.yield ({{.+}} : i1, i64), ^[[LRESUME:.+]]
// CHECK: ^[[LRESUME]]([[L_A:%.+]]: i32):
// CHECK:       cf.br ^[[JOIN:.+]]([[L_A]] : i32)
// CHECK: ^[[RIGHT]]:
// CHECK:       cf.br ^[[JOIN]]([[A]] : i32)
// CHECK: ^[[JOIN]]([[M_A:%.+]]: i32):
// CHECK:       comb.add [[M_A]], {{.+}} : i32
// CHECK:       arc.coroutine.halt {{.+}} : i1, i64

// CHECK-LABEL: hw.module @DiamondMerge
hw.module @DiamondMerge(in %a: i32) {
  %t = llhd.constant_time <1ns, 0d>
  %c0 = hw.constant 0 : i32
  llhd.process {
    %cond = comb.icmp eq %a, %c0 : i32
    cf.cond_br %cond, ^left, ^right
  ^left:
    llhd.wait delay %t, ^left_resume
  ^left_resume:
    cf.br ^join
  ^right:
    cf.br ^join
  ^join:
    %x = comb.add %a, %c0 : i32
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// Two parallel arms both yield, then join. Both incoming branches into the
// merge block must forward the resume-block reaching defs.

// CHECK-LABEL: arc.coroutine.define @TwoYieldsJoin.llhd.process
// CHECK-SAME: (%{{.+}}: i32) -> (i1, i64)
// CHECK:       cf.cond_br {{.+}}, ^[[LEFT:.+]], ^[[RIGHT:.+]]
// CHECK: ^[[LEFT]]:
// CHECK:       arc.coroutine.yield ({{.+}} : i1, i64), ^[[LR:.+]]
// CHECK: ^[[LR]]([[L_A:%.+]]: i32):
// CHECK:       cf.br ^[[JOIN:.+]]([[L_A]] : i32)
// CHECK: ^[[RIGHT]]:
// CHECK:       arc.coroutine.yield ({{.+}} : i1, i64), ^[[RR:.+]]
// CHECK: ^[[RR]]([[R_A:%.+]]: i32):
// CHECK:       cf.br ^[[JOIN]]([[R_A]] : i32)
// CHECK: ^[[JOIN]]([[M_A:%.+]]: i32):
// CHECK:       comb.add [[M_A]], {{.+}} : i32
// CHECK:       arc.coroutine.halt {{.+}} : i1, i64

// CHECK-LABEL: hw.module @TwoYieldsJoin
hw.module @TwoYieldsJoin(in %a: i32) {
  %t = llhd.constant_time <1ns, 0d>
  %c0 = hw.constant 0 : i32
  llhd.process {
    %cond = comb.icmp eq %a, %c0 : i32
    cf.cond_br %cond, ^left, ^right
  ^left:
    llhd.wait delay %t, ^left_resume
  ^left_resume:
    cf.br ^join
  ^right:
    llhd.wait delay %t, ^right_resume
  ^right_resume:
    cf.br ^join
  ^join:
    %x = comb.add %a, %c0 : i32
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// A long chain of single-predecessor blocks between a yield and the use of a
// capture. The reaching definition must propagate through all of the
// intermediate blocks via dominator inheritance, and none of them should
// gain a merge argument.

// CHECK-LABEL: arc.coroutine.define @LongChain.llhd.process
// CHECK-SAME: (%{{.+}}: i32) -> (i1, i64)
// CHECK:       arc.coroutine.yield ({{.+}} : i1, i64), ^[[BB1:.+]]
// CHECK: ^[[BB1]]([[R_A:%.+]]: i32):
// CHECK:       cf.br ^[[BB2:.+]]
// CHECK: ^[[BB2]]:
// CHECK:       cf.br ^[[BB3:.+]]
// CHECK: ^[[BB3]]:
// CHECK:       cf.br ^[[BB4:.+]]
// CHECK: ^[[BB4]]:
// CHECK:       comb.add [[R_A]], [[R_A]] : i32
// CHECK:       arc.coroutine.halt {{.+}} : i1, i64

// CHECK-LABEL: hw.module @LongChain
hw.module @LongChain(in %a: i32) {
  %t = llhd.constant_time <1ns, 0d>
  llhd.process {
    llhd.wait delay %t, ^bb1
  ^bb1:
    cf.br ^bb2
  ^bb2:
    cf.br ^bb3
  ^bb3:
    cf.br ^bb4
  ^bb4:
    %x = comb.add %a, %a : i32
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// A loop header that is BOTH targeted by an `llhd.wait` (making it a resume
// block) AND reached by a `cf.br` from outside the loop. The cf.br must be
// patched with the prefix operand positions filled in by the renamer, and
// the yield-to-self edge must not gain an extra successor operand.

// CHECK-LABEL: arc.coroutine.define @LoopHeaderResume.llhd.process
// CHECK-SAME: ([[A:%.+]]: i32) -> (i1, i64)
// CHECK:       cf.br ^[[HEADER:.+]]([[A]] : i32)
// CHECK: ^[[HEADER]]([[R_A:%.+]]: i32):
// CHECK:       comb.add [[R_A]], [[R_A]] : i32
// CHECK:       [[NOW:%.+]] = arc.current_time
// CHECK:       [[D:%.+]] = llhd.time_to_int
// CHECK:       [[WAKE:%.+]] = comb.add [[NOW]], [[D]] : i64
// CHECK:       arc.coroutine.yield ({{%.+}}, [[WAKE]] : i1, i64), ^[[HEADER]]

// CHECK-LABEL: hw.module @LoopHeaderResume
hw.module @LoopHeaderResume(in %a: i32) {
  %t = llhd.constant_time <1ns, 0d>
  llhd.process {
    cf.br ^header
  ^header:
    %x = comb.add %a, %a : i32
    llhd.wait delay %t, ^header
  }
}

//===----------------------------------------------------------------------===//
// Two `llhd.process` ops in the same hw.module. The symbol table uniquifies
// the second coroutine's name.

// CHECK-DAG: arc.coroutine.define @MultiProcess.llhd.process(
// CHECK-DAG: arc.coroutine.define @MultiProcess.llhd.process_0(

// CHECK-LABEL: hw.module @MultiProcess
hw.module @MultiProcess() {
  // CHECK: arc.coroutine.instance @MultiProcess.llhd.process(
  // CHECK: arc.coroutine.instance @MultiProcess.llhd.process_0(
  llhd.process {
    llhd.halt
  }
  llhd.process {
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// Two distinct `llhd.wait` ops targeting the same resume block. The prefix
// args are inserted exactly once on the shared destination.

// CHECK-LABEL: arc.coroutine.define @SharedResumeBlock.llhd.process
// CHECK-SAME: ([[A:%.+]]: i32) -> (i1, i64)
// CHECK:       cf.cond_br {{.+}}, ^[[LEFT:.+]], ^[[RIGHT:.+]]
// CHECK: ^[[LEFT]]:
// CHECK:       arc.coroutine.yield ({{.+}} : i1, i64), ^[[SHARED:.+]]
// CHECK: ^[[RIGHT]]:
// CHECK:       arc.coroutine.yield ({{.+}} : i1, i64), ^[[SHARED]]
// CHECK: ^[[SHARED]]([[S_A:%.+]]: i32):
// CHECK:       comb.add [[S_A]], {{.+}} : i32
// CHECK:       arc.coroutine.halt

// CHECK-LABEL: hw.module @SharedResumeBlock
hw.module @SharedResumeBlock(in %a: i32) {
  %t = llhd.constant_time <1ns, 0d>
  %c0 = hw.constant 0 : i32
  llhd.process {
    %cond = comb.icmp eq %a, %c0 : i32
    cf.cond_br %cond, ^left, ^right
  ^left:
    llhd.wait delay %t, ^shared
  ^right:
    llhd.wait delay %t, ^shared
  ^shared:
    %x = comb.add %a, %c0 : i32
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// A block that is both a resume block (targeted by `llhd.wait`) and a CFG
// merge point reached by a non-wait predecessor. The yield edge contributes
// no successor operands (the runtime supplies the prefix on resume); the
// `cf.br` edge gets the placeholder operands fixed up to the in-block
// reaching definition.

// CHECK-LABEL: arc.coroutine.define @ResumeAlsoMerges.llhd.process
// CHECK-SAME: ([[A:%.+]]: i32) -> (i1, i64)
// CHECK:       cf.cond_br {{.+}}, ^[[YIELDS:.+]], ^[[DIRECT:.+]]
// CHECK: ^[[YIELDS]]:
// CHECK:       arc.coroutine.yield ({{.+}} : i1, i64), ^[[SHARED:.+]]
// CHECK: ^[[DIRECT]]:
// CHECK:       cf.br ^[[SHARED]]([[A]] : i32)
// CHECK: ^[[SHARED]]([[S_A:%.+]]: i32):
// CHECK:       comb.add [[S_A]], [[S_A]] : i32

// CHECK-LABEL: hw.module @ResumeAlsoMerges
hw.module @ResumeAlsoMerges(in %a: i32) {
  %t = llhd.constant_time <1ns, 0d>
  %c0 = hw.constant 0 : i32
  llhd.process {
    %cond = comb.icmp eq %a, %c0 : i32
    cf.cond_br %cond, ^yields, ^direct
  ^yields:
    llhd.wait delay %t, ^shared
  ^direct:
    cf.br ^shared
  ^shared:
    %x = comb.add %a, %a : i32
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// A constant defined outside the body is referenced from inside a nested
// region (an `scf.if` body) within the process. The walk recurses into the
// nested region, clones the constant once at the entry, and rewrites the
// nested use to the clone.

// CHECK-LABEL: arc.coroutine.define @ConstInNestedRegion.llhd.process
// CHECK-SAME: ([[COND:%.+]]: i1) -> (i1, i64)
// CHECK:   [[C5:%.+]] = hw.constant 5 : i32
// CHECK-NOT: hw.constant 5
// CHECK:   scf.if [[COND]]
// CHECK:     comb.add [[C5]], [[C5]] : i32

// CHECK-LABEL: hw.module @ConstInNestedRegion
hw.module @ConstInNestedRegion(in %cond: i1) {
  %c5 = hw.constant 5 : i32
  %t = llhd.constant_time <1ns, 0d>
  llhd.process {
    scf.if %cond {
      %sum = comb.add %c5, %c5 : i32
      scf.yield
    }
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// A `llhd.wait` with a zero-fs delay is not treated as a halt: it still emits
// a real yield with `wake = now + 0`.

// CHECK-LABEL: arc.coroutine.define @ZeroDelay.llhd.process
// CHECK-SAME: () -> (i0, i64)
// CHECK:   [[T:%.+]] = llhd.constant_time <0fs
// CHECK:   [[NOW:%.+]] = arc.current_time
// CHECK:   [[D:%.+]] = llhd.time_to_int [[T]]
// CHECK:   [[W:%.+]] = comb.add [[NOW]], [[D]] : i64
// CHECK:   arc.coroutine.yield ({{%.+}}, [[W]] : i0, i64), ^[[BB:.+]]
// CHECK: ^[[BB]]:
// CHECK:   arc.coroutine.halt

// CHECK-LABEL: hw.module @ZeroDelay
hw.module @ZeroDelay() {
  %t = llhd.constant_time <0fs, 0d>
  llhd.process {
    llhd.wait delay %t, ^bb1
  ^bb1:
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// A `llhd.wait` that observes a single value and has no delay. It is a real
// suspension (unlike a bare no-delay wait) whose wakeup is the `-1` sentinel
// (it never resumes on time) and whose observe bitmask has the bit of the
// observed argument set. Here the observed value `%clk` is the only capture,
// so the mask is the `i1` value `1` (`true`).

// CHECK-LABEL: arc.coroutine.define @ObserveOnly.llhd.process
// CHECK-SAME: ({{%.+}}: i1) -> (i1, i64)
// CHECK:   [[MASK:%.+]] = hw.constant true
// CHECK:   [[NEVER:%.+]] = hw.constant -1 : i64
// CHECK:   arc.coroutine.yield ([[MASK]], [[NEVER]] : i1, i64), ^[[BB:.+]]
// CHECK: ^[[BB]]({{.+}}):
// CHECK:   arc.coroutine.halt

// CHECK-LABEL: hw.module @ObserveOnly
hw.module @ObserveOnly(in %clk: i1) {
  // CHECK: arc.coroutine.instance @ObserveOnly.llhd.process(%clk) sensitive [true]
  llhd.process {
    llhd.wait (%clk : i1), ^bb1
  ^bb1:
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// A `llhd.wait` that observes a value AND has a delay resumes on either a value
// change or the scheduled time. The mask has the observed argument's bit set
// and the wakeup is `now + delay`.

// CHECK-LABEL: arc.coroutine.define @ObserveAndDelay.llhd.process
// CHECK-SAME: ({{%.+}}: i1) -> (i1, i64)
// CHECK:   [[MASK:%.+]] = hw.constant true
// CHECK:   [[NOW:%.+]] = arc.current_time
// CHECK:   [[D:%.+]] = llhd.time_to_int
// CHECK:   [[W:%.+]] = comb.add [[NOW]], [[D]] : i64
// CHECK:   arc.coroutine.yield ([[MASK]], [[W]] : i1, i64), ^[[BB:.+]]

// CHECK-LABEL: hw.module @ObserveAndDelay
hw.module @ObserveAndDelay(in %clk: i1) {
  // CHECK: arc.coroutine.instance @ObserveAndDelay.llhd.process(%clk) sensitive [true] : (i1)
  %t = llhd.constant_time <1ns, 0d>
  llhd.process {
    llhd.wait delay %t, (%clk : i1), ^bb1
  ^bb1:
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// Observing multiple values sets multiple mask bits. The captures are ordered
// `%a` (arg 0), `%b` (arg 1), so observing both yields the `i2` value `3` (bits
// 0 and 1), which prints as `-1`.

// CHECK-LABEL: arc.coroutine.define @ObserveMulti.llhd.process
// CHECK-SAME: ({{%.+}}: i1, {{%.+}}: i1) -> (i2, i64)
// CHECK:   [[MASK:%.+]] = hw.constant -1 : i2
// CHECK:   arc.coroutine.yield ([[MASK]], {{%.+}} : i2, i64), ^{{.+}}

// CHECK-LABEL: hw.module @ObserveMulti
hw.module @ObserveMulti(in %a: i1, in %b: i1) {
  // CHECK: arc.coroutine.instance @ObserveMulti.llhd.process(%a, %b) sensitive [true, true]
  %t = llhd.constant_time <1ns, 0d>
  llhd.process {
    llhd.wait delay %t, (%a, %b : i1, i1), ^bb1
  ^bb1:
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// Observe only one of two arguments. Observing `%b` (arg 1) yields the `i2`
// value `2` (bit 1), which prints as `-2`.

// CHECK-LABEL: arc.coroutine.define @ObserveSome.llhd.process
// CHECK-SAME: ({{%.+}}: i1, {{%.+}}: i1) -> (i1, i2, i64)
// CHECK:   [[MASK:%.+]] = hw.constant -2 : i2
// CHECK:   arc.coroutine.yield ({{%.+}}, [[MASK]], {{%.+}} : i1, i2, i64), ^{{.+}}

// CHECK-LABEL: hw.module @ObserveSome
hw.module @ObserveSome(in %a: i1, in %b: i1) {
  // CHECK: arc.coroutine.instance @ObserveSome.llhd.process(%a, %b) sensitive [false, true] : (i1, i1)
  %t = llhd.constant_time <1ns, 0d>
  llhd.process -> i1 {
    llhd.wait yield (%a : i1), delay %t, (%b : i1), ^bb1
  ^bb1:
    llhd.halt %a : i1
  }
}


//===----------------------------------------------------------------------===//
// An observed value that is defined *inside* the process body can never change
// and is not a coroutine argument, so it contributes no mask bit. Only the
// observed input port `%a` (arg 0) sets a bit; the internally-derived `%inv` is
// ignored, leaving the `i1` mask value `1` (`true`). The `comb.xor` anchor
// below skips the unrelated `true` constant feeding it.

// CHECK-LABEL: arc.coroutine.define @ObserveInternal.llhd.process
// CHECK-SAME: ({{%.+}}: i1) -> (i1, i64)
// CHECK:   comb.xor
// CHECK:   [[MASK:%.+]] = hw.constant true
// CHECK:   arc.coroutine.yield ([[MASK]], {{%.+}} : i1, i64), ^{{.+}}

// CHECK-LABEL: hw.module @ObserveInternal
hw.module @ObserveInternal(in %a: i1) {
  %t = llhd.constant_time <1ns, 0d>
  %true = hw.constant true
  llhd.process {
    %inv = comb.xor %a, %true : i1
    llhd.wait delay %t, (%a, %inv : i1, i1), ^bb1
  ^bb1:
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// An observed value that is a resume block argument -- a value forwarded to the
// block as a wait destination operand -- is NOT a coroutine argument, so it
// must contribute no mask bit. Only the observed input port `%a` (arg 0) sets a
// bit. This guards against blindly using a block argument's number, which for a
// forwarded operand lands past the argument prefix and would be out of range.

// CHECK-LABEL: arc.coroutine.define @ObserveResumeArg.llhd.process
// CHECK-SAME: ({{%.+}}: i8) -> (i1, i64)
// CHECK: ^{{.+}}({{%.+}}: i8, [[X:%.+]]: i8):
// CHECK:   [[MASK:%.+]] = hw.constant true
// CHECK:   arc.coroutine.yield ([[MASK]], {{%.+}} : i1, i64), ^{{.+}}

// CHECK-LABEL: hw.module @ObserveResumeArg
hw.module @ObserveResumeArg(in %a: i8) {
  %t = llhd.constant_time <1ns, 0d>
  llhd.process {
    llhd.wait delay %t, ^bb1(%a : i8)
  ^bb1(%x: i8):
    // `%x` is a forwarded block argument, `%a` is a coroutine argument.
    llhd.wait delay %t, (%x, %a : i8, i8), ^bb2
  ^bb2:
    llhd.halt
  }
}

//===----------------------------------------------------------------------===//
// Simulation control ops that need access to the model context are lowered to
// their Arc equivalents. `ub.unreachable` becomes a halt yielding zeros.

// CHECK-LABEL: arc.coroutine.define @SimControl.llhd.process
// CHECK-SAME: ([[A:%.+]]: i1) -> (i32, i1, i64)
// CHECK:   [[CTX:%.+]] = arc.inferred_context
// CHECK:   [[NOW:%.+]] = arc.current_time [[CTX]]
// CHECK:   [[TIME:%.+]] = llhd.int_to_time [[NOW]]
// CHECK:   [[TINT:%.+]] = llhd.time_to_int [[TIME]]
// CHECK:   [[TRUNC:%.+]] = comb.extract [[TINT]] from 0 : (i64) -> i32
// CHECK:   cf.cond_br [[A]], ^[[BB1:.+]], ^[[BB2:.+]]
// CHECK: ^[[BB1]]:
// CHECK:   arc.terminate [[CTX]], true
// CHECK:   [[ZERO:%.+]] = hw.constant 0 : i32
// CHECK:   arc.coroutine.halt [[ZERO]], {{%.+}}, {{%.+}} : i32, i1, i64
// CHECK: ^[[BB2]]:
// CHECK:   arc.terminate [[CTX]], false
// CHECK:   arc.coroutine.halt [[TRUNC]], {{%.+}}, {{%.+}} : i32, i1, i64
// CHECK-NOT: sim.terminate
// CHECK-NOT: sim.pause
// CHECK-NOT: llhd.current_time

// CHECK-LABEL: hw.module @SimControl
hw.module @SimControl(in %a: i1, out x: i32) {
  %0 = llhd.process -> i32 {
    %now = llhd.current_time
    %nowInt = llhd.time_to_int %now
    %trunc = comb.extract %nowInt from 0 : (i64) -> i32
    cf.cond_br %a, ^bb1, ^bb2
  ^bb1:
    sim.terminate success, quiet
    ub.unreachable
  ^bb2:
    sim.pause quiet
    llhd.halt %trunc : i32
  }
  hw.output %0 : i32
}

//===----------------------------------------------------------------------===//
// Signals referenced inside the process are captured as coroutine arguments.
// Projections into signals defined outside the process are cloned right before
// their uses, such that their dynamic indices use the argument values of the
// current entry. Uses of the signal itself refer to the entry block argument
// on every entry. Observing a probed signal sets the signal argument's bit in
// the observe bitmask.

// CHECK-LABEL: arc.coroutine.define @SignalCaptures.llhd.process
// CHECK-SAME: ([[S:%.+]]: !llhd.ref<!hw.array<2xi8>>, [[IDX:%.+]]: i1) -> (i2, i64)
// CHECK:   cf.br ^[[BB1:.+]]([[IDX]] : i1)
// CHECK: ^[[BB1]]([[IDX1:%.+]]: i1):
// CHECK:   [[E1:%.+]] = llhd.sig.array_get [[S]][[[IDX1]]]
// CHECK:   [[P:%.+]] = llhd.prb [[E1]]
// CHECK:   [[MASK:%.+]] = hw.constant 1 : i2
// CHECK:   arc.coroutine.yield ([[MASK]], {{%.+}} : i2, i64), ^[[BB2:.+]]
// CHECK: ^[[BB2]]({{%.+}}: !llhd.ref<!hw.array<2xi8>>, [[IDX2:%.+]]: i1):
// CHECK:   [[N:%.+]] = comb.add [[P]]
// CHECK:   [[E2:%.+]] = llhd.sig.array_get [[S]][[[IDX2]]]
// CHECK:   llhd.drv [[E2]], [[N]] after
// CHECK:   cf.br ^[[BB1]]([[IDX2]] : i1)

// CHECK-LABEL: hw.module @SignalCaptures
// CHECK: [[SIG:%.+]] = llhd.sig
// CHECK: arc.coroutine.instance @SignalCaptures.llhd.process([[SIG]], %idx) sensitive [true, false]
hw.module @SignalCaptures(in %idx: i1) {
  %c0_i16 = hw.constant 0 : i16
  %init = hw.bitcast %c0_i16 : (i16) -> !hw.array<2xi8>
  %s = llhd.sig %init : <!hw.array<2xi8>>
  %e = llhd.sig.array_get %s[%idx] : <!hw.array<2xi8>>
  %delta = llhd.constant_time <0ns, 1d>
  llhd.process {
    cf.br ^bb1
  ^bb1:
    %p = llhd.prb %e : i8
    llhd.wait (%p : i8), ^bb2
  ^bb2:
    %c1_i8 = hw.constant 1 : i8
    %n = comb.add %p, %c1_i8 : i8
    llhd.drv %e, %n after %delta : i8
    cf.br ^bb1
  }
  hw.output
}

//===----------------------------------------------------------------------===//
// Coroutines are lowered to `arc.coroutine.define` ops of the same name. Their
// waits yield zero values for the coroutine's results, and observe the
// coroutine's arguments. Calls to coroutines are expanded into a loop that
// suspends the caller whenever the callee suspends, mapping the callee's
// observe bitmask onto the caller's arguments.

// CHECK-LABEL: arc.coroutine.define private @waitEdges
// CHECK-SAME: ([[CLK:%.+]]: !llhd.ref<i1>, [[N:%.+]]: i8) -> (i8, i2, i64)
// CHECK:   llhd.prb [[CLK]]
// CHECK:   [[ZERO:%.+]] = hw.constant 0 : i8
// CHECK:   arc.coroutine.yield ([[ZERO]], {{%.+}}, {{%.+}} : i8, i2, i64), ^[[BB:.+]]
// CHECK: ^[[BB]]({{%.+}}: !llhd.ref<i1>, [[N2:%.+]]: i8):
// CHECK:   arc.coroutine.return [[N2]], {{%.+}}, {{%.+}} : i8, i2, i64
llhd.coroutine private @waitEdges(%clk: !llhd.ref<i1>, %n: i8) -> i8 {
  %p = llhd.prb %clk : i1
  llhd.wait (%p : i1), ^bb1
^bb1:
  llhd.return %n : i8
}

// CHECK-LABEL: arc.coroutine.define @CoroutineCalls.llhd.process
// CHECK-SAME: ([[CLK:%.+]]: !llhd.ref<i1>) -> (i8, i1, i64)
// CHECK:   [[STATE0:%.+]] = arc.coroutine.undefined_state : <@waitEdges>
// CHECK:   [[PC0:%.+]] = arc.coroutine.start_pc : <@waitEdges>
// CHECK:   cf.br ^[[CALL:.+]]([[STATE0]], [[PC0]] :
// CHECK: ^[[CALL]]([[STATE:%.+]]: !arc.coroutine_state<@waitEdges>, [[PC:%.+]]: !arc.coroutine_pc<@waitEdges>):
// CHECK:   [[NEWSTATE:%.+]], [[NEWPC:%.+]], [[RES:%.+]]:3 = arc.coroutine.call @waitEdges([[STATE]], [[PC]], [[CLK]], {{%.+}})
// CHECK:   [[ISRET:%.+]] = arc.coroutine.pc_is_return [[NEWPC]]
// CHECK:   cf.cond_br [[ISRET]], ^[[CONT:.+]]([[RES]]#0 : i8), ^[[SUSP:.+]]
// CHECK: ^[[SUSP]]:
// CHECK:   [[ISHALT:%.+]] = arc.coroutine.pc_is_halt [[NEWPC]]
// CHECK:   cf.cond_br [[ISHALT]], ^[[HALT:.+]], ^[[WAIT:.+]]
// CHECK: ^[[HALT]]:
// CHECK:   arc.coroutine.halt
// CHECK: ^[[WAIT]]:
// CHECK:   [[BIT:%.+]] = comb.extract [[RES]]#1 from 0 : (i2) -> i1
// CHECK:   [[MASK:%.+]] = comb.concat [[BIT]] : i1
// CHECK:   [[ZERO:%.+]] = hw.constant 0 : i8
// CHECK:   arc.coroutine.yield ([[ZERO]], [[MASK]], [[RES]]#2 : i8, i1, i64), ^[[RESUME:.+]]([[NEWSTATE]], [[NEWPC]] :
// CHECK: ^[[RESUME]]({{%.+}}: !llhd.ref<i1>, [[RSTATE:%.+]]: !arc.coroutine_state<@waitEdges>, [[RPC:%.+]]: !arc.coroutine_pc<@waitEdges>):
// CHECK:   cf.br ^[[CALL]]([[RSTATE]], [[RPC]] :
// CHECK: ^[[CONT]]([[R:%.+]]: i8):
// CHECK:   arc.coroutine.halt [[R]], {{%.+}}, {{%.+}} : i8, i1, i64

// CHECK-LABEL: hw.module @CoroutineCalls
// CHECK: arc.coroutine.instance @CoroutineCalls.llhd.process({{%.+}}) sensitive [true]
hw.module @CoroutineCalls(out x: i8) {
  %false = hw.constant false
  %c3 = hw.constant 3 : i8
  %clk = llhd.sig %false : <i1>
  %0 = llhd.process -> i8 {
    %r = llhd.call_coroutine @waitEdges(%clk, %c3) : (!llhd.ref<i1>, i8) -> i8
    llhd.halt %r : i8
  }
  hw.output %0 : i8
}

//===----------------------------------------------------------------------===//
// Value change dump ops are lowered to their Arc equivalents, which need access
// to the model context.

// CHECK-LABEL: arc.coroutine.define @DumpVars.llhd.process
// CHECK:   [[CTX:%.+]] = arc.inferred_context
// CHECK:   arc.dump_file [[CTX]], "waves.vcd"
// CHECK:   arc.dump_vars [[CTX]]
// CHECK-NOT: sim.dump
hw.module @DumpVars() {
  llhd.process {
    sim.dump_file "waves.vcd"
    sim.dump_vars
    llhd.halt
  }
  hw.output
}

//===----------------------------------------------------------------------===//
// Immediate assertions print an error message if they fail.

// CHECK-LABEL: arc.coroutine.define @ImmediateAssert.llhd.process
// CHECK:   [[TRUE:%.+]] = hw.constant true
// CHECK:   [[NOT:%.+]] = comb.xor %arg0, [[TRUE]] : i1
// CHECK:   [[FAILED:%.+]] = comb.and [[NOT]], %arg1 : i1
// CHECK:   cf.cond_br [[FAILED]], ^[[FAIL:.+]], ^[[CONT:.+]]
// CHECK: ^[[FAIL]]:
// CHECK:   [[MSG:%.+]] = sim.fmt.literal "Error: Assertion failed (myAssert)
// CHECK:   sim.proc.print [[MSG]]
// CHECK:   cf.br ^[[CONT]]
// CHECK: ^[[CONT]]:
// CHECK-NOT: verif.assert
hw.module @ImmediateAssert(in %a: i1, in %en: i1) {
  llhd.process {
    verif.assert %a if %en label "myAssert" : i1
    llhd.halt
  }
  hw.output
}

//===----------------------------------------------------------------------===//
// Combinational processes with side effects are converted into processes that
// wait for changes of the values they use.

// CHECK-LABEL: arc.coroutine.define @CombinationalSideEffects.llhd.process
// CHECK-SAME: ([[A:%.+]]: i1) -> (i1, i1, i64)
// CHECK:   cf.br ^[[BODY:.+]]([[A]] : i1)
// CHECK: ^[[BODY]]([[A1:%.+]]: i1):
// CHECK:   sim.proc.print
// CHECK:   [[MASK:%.+]] = hw.constant true
// CHECK:   arc.coroutine.yield ([[A1]], [[MASK]], {{%.+}} : i1, i1, i64), ^[[BODY]]

// CHECK-LABEL: hw.module @CombinationalSideEffects
// CHECK: arc.coroutine.instance @CombinationalSideEffects.llhd.process(%a) sensitive [true] : (i1) -> i1
// CHECK-NOT: llhd.combinational
hw.module @CombinationalSideEffects(in %a: i1, out x: i1) {
  %0 = llhd.combinational -> i1 {
    %lit = sim.fmt.literal "changed\0A"
    sim.proc.print %lit
    llhd.yield %a : i1
  }
  hw.output %0 : i1
}

// Combinational processes without side effects remain untouched.
// CHECK-LABEL: hw.module @CombinationalPure
// CHECK: llhd.combinational
hw.module @CombinationalPure(in %a: i1, out x: i1) {
  %0 = llhd.combinational -> i1 {
    %1 = comb.xor %a, %a : i1
    llhd.yield %1 : i1
  }
  hw.output %0 : i1
}

// Format strings defined outside of a process are cloned into it, since they
// cannot be passed to coroutines. Only their operands are captured.
// CHECK-LABEL: arc.coroutine.define @FormatStringCapture.llhd.process
// CHECK-SAME: (%arg0: i8)
// CHECK:     ^bb1([[X:%.+]]: i8):
// CHECK:       [[DEC:%.+]] = sim.fmt.dec [[X]] : i8
// CHECK:       [[MSG:%.+]] = sim.fmt.concat ({{%.+}}, [[DEC]])
// CHECK:       sim.proc.print [[MSG]]
hw.module @FormatStringCapture(in %x: i8) {
  %t = llhd.constant_time <1ns, 0d>
  %l = sim.fmt.literal "x="
  %d = sim.fmt.dec %x : i8
  %m = sim.fmt.concat (%l, %d)
  llhd.process {
    llhd.wait delay %t, ^bb1
  ^bb1:
    sim.proc.print %m
    llhd.halt
  }
  hw.output
}
