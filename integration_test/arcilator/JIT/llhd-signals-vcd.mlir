// RUN: arcilator %s --run --jit-entry=Top_main --jit-vcd-file=%t.vcd
// RUN: FileCheck %s --input-file=%t.vcd
// REQUIRES: arcilator-jit

// LLHD signals are traced, including updates performed by processes directly
// on the signal. The last update right before the simulation terminates must
// also show up in the trace.

// CHECK:      $var wire 8 [[ID:[^ ]+]] counter $end
// CHECK:      #0
// CHECK:      b00000000 [[ID]]
// CHECK:      #1000000
// CHECK-NEXT: b00000001 [[ID]]
// CHECK:      #2000000
// CHECK-NEXT: b00000010 [[ID]]
// CHECK:      #3000000
// CHECK-NEXT: b00000011 [[ID]]
// CHECK-NOT:  [[ID]]

hw.module @Top() {
  %c0_i8 = hw.constant 0 : i8
  %c1_i8 = hw.constant 1 : i8
  %c3_i8 = hw.constant 3 : i8
  %step = llhd.constant_time <1ns, 0d>
  %counter = llhd.sig %c0_i8 : <i8>
  llhd.process {
    cf.br ^bb1
  ^bb1:
    llhd.wait delay %step, ^bb2
  ^bb2:
    %v = llhd.prb %counter : i8
    %n = comb.add %v, %c1_i8 : i8
    llhd.assign %counter, %n : i8
    %done = comb.icmp eq %n, %c3_i8 : i8
    cf.cond_br %done, ^bb3, ^bb1
  ^bb3:
    sim.terminate success, quiet
    llhd.halt
  }
  hw.output
}
