// RUN: circt-opt %s --convert-to-arcs="tap-registers=0" | FileCheck %s --check-prefixes=CHECK,CHECK-TAP-OFF
// RUN: circt-opt %s --convert-to-arcs="tap-registers=1" | FileCheck %s --check-prefixes=CHECK,CHECK-TAP-ON

// CHECK-LABEL: hw.module @Trivial(
hw.module @Trivial(in %clock: !seq.clock, in %i0: i4, in %reset: i1, out o0: i4) {
  // CHECK: arc.state {{@.+}}(%i0) clock %clock latency 1
  // CHECK-TAP-OFF-NOT: names = ["foo"]
  // CHECK-TAP-ON: names = ["foo"]
  %foo = seq.compreg %i0, %clock : i4
  hw.output %foo : i4
}

// Unnamed registers get an empty name when tapping registers.
// CHECK-LABEL: hw.module @UnnamedRegisters
hw.module @UnnamedRegisters(in %clk : !seq.clock, in %a : i8, in %b : i8, out x : i8, out y : i8) {
  // CHECK-TAP-ON: arc.state {{.*}} {names = ["", "named"]}
  %0 = seq.compreg %a, %clk : i8
  %1 = seq.compreg %b, %clk {name = "named"} : i8
  hw.output %0, %1 : i8, i8
}
