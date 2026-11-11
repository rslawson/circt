// RUN: arcilator --run %s --jit-entry=Top_main | FileCheck %s
// REQUIRES: arcilator-jit

// End-to-end test for LLHD signals. A clock process toggles the `clk` signal
// every 5ns, which clocks a counter register. A second process swaps the values
// of signals `a` and `b` on every rising clock edge through delayed drives,
// which must not observe each other's updates. A monitor process prints all
// values on every falling clock edge and terminates after a few cycles.

// CHECK:      cnt=1 a=2 b=1
// CHECK-NEXT: cnt=2 a=1 b=2
// CHECK-NEXT: cnt=3 a=2 b=1
// CHECK-NEXT: cnt=4 a=1 b=2
// CHECK-NOT:  cnt=

hw.module @Top() {
  %true = hw.constant true
  %false = hw.constant false
  %c0_i8 = hw.constant 0 : i8
  %c1_i8 = hw.constant 1 : i8
  %c2_i8 = hw.constant 2 : i8
  %c4_i8 = hw.constant 4 : i8
  %half = llhd.constant_time <5ns, 0d>
  %delta = llhd.constant_time <0ns, 1d>

  // The clock signal and its generator.
  %clk = llhd.sig %false : <i1>
  %clkv = llhd.prb %clk : i1
  %gen:2 = llhd.process -> i1, i1 {
    cf.br ^bb1(%false, %false : i1, i1)
  ^bb1(%v: i1, %en: i1):
    llhd.wait yield (%v, %en : i1, i1), delay %half, ^bb2
  ^bb2:
    %n = comb.xor %clkv, %true : i1
    cf.br ^bb1(%n, %true : i1, i1)
  }
  llhd.assign %clk, %gen#0 if %gen#1 : i1

  // A counter clocked by the signal.
  %clock = seq.to_clock %clkv
  %cntNext = comb.add %cnt, %c1_i8 : i8
  %cnt = seq.firreg %cntNext clock %clock preset 0 : i8

  // Swap `a` and `b` on every rising edge through delayed drives.
  %a = llhd.sig %c1_i8 : <i8>
  %b = llhd.sig %c2_i8 : <i8>
  %av = llhd.prb %a : i8
  %bv = llhd.prb %b : i8
  %swap:3 = llhd.process -> i8, i8, i1 {
    cf.br ^bb1(%c0_i8, %c0_i8, %false : i8, i8, i1)
  ^bb1(%x: i8, %y: i8, %en: i1):
    llhd.wait yield (%x, %y, %en : i8, i8, i1), (%clkv : i1), ^bb2(%clkv : i1)
  ^bb2(%old: i1):
    %notOld = comb.xor %old, %true : i1
    %posedge = comb.and %notOld, %clkv : i1
    cf.cond_br %posedge, ^bb1(%bv, %av, %true : i8, i8, i1), ^bb1(%c0_i8, %c0_i8, %false : i8, i8, i1)
  }
  llhd.drv %a, %swap#0 after %delta if %swap#2 : i8
  llhd.drv %b, %swap#1 after %delta if %swap#2 : i8

  // Print everything on falling edges.
  llhd.process {
    cf.br ^bb1
  ^bb1:
    llhd.wait (%clkv : i1), ^bb2(%clkv : i1)
  ^bb2(%old: i1):
    %notNew = comb.xor %clkv, %true : i1
    %negedge = comb.and %old, %notNew : i1
    cf.cond_br %negedge, ^bb3, ^bb1
  ^bb3:
    %l0 = sim.fmt.literal "cnt="
    %l1 = sim.fmt.literal " a="
    %l2 = sim.fmt.literal " b="
    %nl = sim.fmt.literal "\0A"
    %f0 = sim.fmt.dec %cnt specifierWidth 0 : i8
    %f1 = sim.fmt.dec %av specifierWidth 0 : i8
    %f2 = sim.fmt.dec %bv specifierWidth 0 : i8
    %msg = sim.fmt.concat (%l0, %f0, %l1, %f1, %l2, %f2, %nl)
    sim.proc.print %msg
    %done = comb.icmp eq %cnt, %c4_i8 : i8
    cf.cond_br %done, ^bb4, ^bb1
  ^bb4:
    sim.terminate success, quiet
    llhd.halt
  }
  hw.output
}
