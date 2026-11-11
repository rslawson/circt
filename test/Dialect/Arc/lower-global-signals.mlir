// RUN: circt-opt %s --arc-lower-global-signals | FileCheck %s

// Global signals are declared in the root modules using them, with their
// initial value computed from the initializer. Other modules receive a
// reference to the signal through an additional port.

// CHECK-NOT: llhd.global_signal
llhd.global_signal @g : i8 init {
  %c = hw.constant 42 : i8
  llhd.yield %c : i8
}
llhd.global_signal @unused : i8

// CHECK-LABEL: hw.module private @Leaf(in %g : !llhd.ref<i8>, out o : i8)
// CHECK-NEXT:    [[P:%.+]] = llhd.prb %g : i8
// CHECK-NEXT:    hw.output [[P]]
hw.module private @Leaf(out o : i8) {
  %0 = llhd.get_global_signal @g : <i8>
  %1 = llhd.prb %0 : i8
  hw.output %1 : i8
}

// CHECK-LABEL: hw.module private @Mid(in %g : !llhd.ref<i8>, out o : i8)
// CHECK-NEXT:    hw.instance "leaf" @Leaf(g: %g: !llhd.ref<i8>) -> (o: i8)
hw.module private @Mid(out o : i8) {
  %0 = hw.instance "leaf" @Leaf() -> (o: i8)
  hw.output %0 : i8
}

// CHECK-LABEL: hw.module @Top(out o : i8, out p : i8)
// CHECK-NEXT:    [[C:%.+]] = hw.constant 42 : i8
// CHECK-NEXT:    [[G:%g]] = llhd.sig [[C]] : <i8>
// CHECK-NEXT:    hw.instance "mid" @Mid(g: [[G]]: !llhd.ref<i8>) -> (o: i8)
// CHECK-NEXT:    llhd.prb [[G]] : i8
hw.module @Top(out o : i8, out p : i8) {
  %0 = hw.instance "mid" @Mid() -> (o: i8)
  %1 = llhd.get_global_signal @g : <i8>
  %2 = llhd.prb %1 : i8
  hw.output %0, %2 : i8, i8
}
