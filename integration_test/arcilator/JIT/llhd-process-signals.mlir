// RUN: arcilator --run %s --jit-entry=Top_main | FileCheck %s
// REQUIRES: arcilator-jit

// End-to-end test for processes accessing signals directly. Every 1ns, a writer
// process increments signal `s` through a blocking assignment and updates `t`
// to the same value through a nonblocking assignment. A reader process waits
// for changes of `s` and observes the new value of `s`, but still the old value
// of `t`, since the nonblocking assignment only takes effect afterwards.

// CHECK:      s=1 t=0
// CHECK-NEXT: s=2 t=1
// CHECK-NEXT: s=3 t=2
// CHECK-NEXT: s=4 t=3
// CHECK-NOT:  s=

hw.module @Top() {
  %c0_i8 = hw.constant 0 : i8
  %c1_i8 = hw.constant 1 : i8
  %c4_i8 = hw.constant 4 : i8
  %step = llhd.constant_time <1ns, 0d>
  %delta = llhd.constant_time <0ns, 1d>
  %s = llhd.sig %c0_i8 : <i8>
  %t = llhd.sig %c0_i8 : <i8>

  llhd.process {
    cf.br ^bb1
  ^bb1:
    llhd.wait delay %step, ^bb2
  ^bb2:
    %v = llhd.prb %s : i8
    %n = comb.add %v, %c1_i8 : i8
    llhd.assign %s, %n : i8
    llhd.drv %t, %n after %delta : i8
    cf.br ^bb1
  }

  llhd.process {
    cf.br ^bb1
  ^bb1:
    %v = llhd.prb %s : i8
    llhd.wait (%v : i8), ^bb2
  ^bb2:
    %sv = llhd.prb %s : i8
    %tv = llhd.prb %t : i8
    %l0 = sim.fmt.literal "s="
    %l1 = sim.fmt.literal " t="
    %nl = sim.fmt.literal "\0A"
    %f0 = sim.fmt.dec %sv specifierWidth 0 : i8
    %f1 = sim.fmt.dec %tv specifierWidth 0 : i8
    %msg = sim.fmt.concat (%l0, %f0, %l1, %f1, %nl)
    sim.proc.print %msg
    %done = comb.icmp eq %sv, %c4_i8 : i8
    cf.cond_br %done, ^bb3, ^bb1
  ^bb3:
    sim.terminate success, quiet
    llhd.halt
  }
  hw.output
}
