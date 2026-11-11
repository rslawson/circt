// RUN: rm -f dumpvars-test.vcd
// RUN: arcilator %s --run --jit-entry=Top_main
// RUN: FileCheck %s --input-file=dumpvars-test.vcd
// REQUIRES: arcilator-jit

// Designs that call `$dumpvars` are compiled with trace instrumentation, and
// start writing a trace to the file set by `$dumpfile` when `$dumpvars` is
// executed. The trace begins with the values at that time.

// CHECK:      $var wire 8 [[ID:[^ ]+]] counter $end
// CHECK:      $enddefinitions $end
// CHECK-NEXT: #1500000
// CHECK:      b00000001 [[ID]]
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
  %start = llhd.constant_time <1500ps, 0d>
  %counter = llhd.sig %c0_i8 : <i8>

  // Increment the counter every 1ns, until it reaches 3.
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

  // Start dumping at 1.5ns.
  llhd.process {
    sim.dump_file "dumpvars-test.vcd"
    llhd.wait delay %start, ^bb1
  ^bb1:
    sim.dump_vars
    llhd.halt
  }
  hw.output
}
