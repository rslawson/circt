// RUN: arcilator --run %s --jit-entry=Top_main | FileCheck %s
// REQUIRES: arcilator-jit

// Processes triggered by a clock edge observe the values of registers before
// that clock edge updates them, like `always @(posedge clk)` blocks observe
// registers before their nonblocking assignments take effect. A process
// waiting for a register to change observes the new value at the time of the
// clock edge.

// CHECK:      posedge at 5000000: count=0
// CHECK-NEXT: count changed to 1 at 5000000
// CHECK-NEXT: posedge at 15000000: count=1
// CHECK-NEXT: count changed to 2 at 15000000
// CHECK-NEXT: posedge at 25000000: count=2

hw.module @Top() {
  %true = hw.constant true
  %false = hw.constant false
  %c1_i8 = hw.constant 1 : i8
  %c2_i8 = hw.constant 2 : i8
  %half = llhd.constant_time <5ns, 0d>
  %clk = llhd.sig %false : <i1>
  %clkv = llhd.prb %clk : i1
  llhd.process {
    cf.br ^bb1
  ^bb1:
    llhd.wait delay %half, ^bb2
  ^bb2:
    %n = comb.xor %clkv, %true : i1
    llhd.assign %clk, %n : i1
    cf.br ^bb1
  }
  %clock = seq.to_clock %clkv
  %countNext = comb.add %count, %c1_i8 : i8
  %count = seq.firreg %countNext clock %clock preset 0 : i8

  // Print the count on every rising clock edge.
  llhd.process {
    cf.br ^bb1
  ^bb1:
    llhd.wait (%clkv : i1), ^bb2(%clkv : i1)
  ^bb2(%old: i1):
    %notOld = comb.xor %old, %true : i1
    %posedge = comb.and %notOld, %clkv : i1
    cf.cond_br %posedge, ^bb3, ^bb1
  ^bb3:
    %now = llhd.current_time
    %nowInt = llhd.time_to_int %now
    %l0 = sim.fmt.literal "posedge at "
    %l1 = sim.fmt.literal ": count="
    %nl = sim.fmt.literal "\0A"
    %f0 = sim.fmt.dec %nowInt specifierWidth 0 : i64
    %f1 = sim.fmt.dec %count specifierWidth 0 : i8
    %msg = sim.fmt.concat (%l0, %f0, %l1, %f1, %nl)
    sim.proc.print %msg
    %done = comb.icmp eq %count, %c2_i8 : i8
    cf.cond_br %done, ^bb4, ^bb1
  ^bb4:
    sim.terminate success, quiet
    llhd.halt
  }

  // Print whenever the count changes.
  llhd.process {
    cf.br ^bb1
  ^bb1:
    llhd.wait (%count : i8), ^bb2
  ^bb2:
    %now = llhd.current_time
    %nowInt = llhd.time_to_int %now
    %l0 = sim.fmt.literal "count changed to "
    %l1 = sim.fmt.literal " at "
    %nl = sim.fmt.literal "\0A"
    %f0 = sim.fmt.dec %count specifierWidth 0 : i8
    %f1 = sim.fmt.dec %nowInt specifierWidth 0 : i64
    %msg = sim.fmt.concat (%l0, %f0, %l1, %f1, %nl)
    sim.proc.print %msg
    cf.br ^bb1
  }
  hw.output
}
