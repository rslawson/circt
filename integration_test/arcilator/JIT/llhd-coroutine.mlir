// RUN: arcilator --run %s --jit-entry=Top_main | FileCheck %s
// REQUIRES: arcilator-jit

// End-to-end test for coroutines, the lowered form of SystemVerilog tasks. A
// clock process toggles `clk` every 5ns. A process calls a task that waits for
// a given number of rising clock edges of the signal passed into it, and then
// returns the time at which it finished. The task is called twice.

// CHECK:      task returned at 25000000
// CHECK-NEXT: task returned at 45000000
// CHECK-NEXT: done at 45000000

llhd.coroutine private @waitEdges(%clk: !llhd.ref<i1>, %n: i8) -> i64 {
  %true = hw.constant true
  %c0_i8 = hw.constant 0 : i8
  %c1_i8 = hw.constant 1 : i8
  cf.br ^bb1(%n : i8)
^bb1(%i: i8):
  %done = comb.icmp eq %i, %c0_i8 : i8
  cf.cond_br %done, ^bb4, ^bb2
^bb2:
  %old = llhd.prb %clk : i1
  llhd.wait (%old : i1), ^bb3
^bb3:
  %new = llhd.prb %clk : i1
  %notOld = comb.xor %old, %true : i1
  %posedge = comb.and %notOld, %new : i1
  %dec = comb.sub %i, %c1_i8 : i8
  %next = comb.mux %posedge, %dec, %i : i8
  cf.br ^bb1(%next : i8)
^bb4:
  %now = llhd.current_time
  %nowInt = llhd.time_to_int %now
  llhd.return %nowInt : i64
}

hw.module @Top() {
  %true = hw.constant true
  %false = hw.constant false
  %c2_i8 = hw.constant 2 : i8
  %c3_i8 = hw.constant 3 : i8
  %half = llhd.constant_time <5ns, 0d>
  %clk = llhd.sig %false : <i1>
  llhd.process {
    cf.br ^bb1
  ^bb1:
    llhd.wait delay %half, ^bb2
  ^bb2:
    %v = llhd.prb %clk : i1
    %n = comb.xor %v, %true : i1
    llhd.assign %clk, %n : i1
    cf.br ^bb1
  }
  llhd.process {
    %lit = sim.fmt.literal "task returned at "
    %nl = sim.fmt.literal "\0A"
    %t0 = llhd.call_coroutine @waitEdges(%clk, %c3_i8) : (!llhd.ref<i1>, i8) -> i64
    %f0 = sim.fmt.dec %t0 specifierWidth 0 : i64
    %m0 = sim.fmt.concat (%lit, %f0, %nl)
    sim.proc.print %m0
    %t1 = llhd.call_coroutine @waitEdges(%clk, %c2_i8) : (!llhd.ref<i1>, i8) -> i64
    %f1 = sim.fmt.dec %t1 specifierWidth 0 : i64
    %m1 = sim.fmt.concat (%lit, %f1, %nl)
    sim.proc.print %m1
    %now = llhd.current_time
    %nowInt = llhd.time_to_int %now
    %l2 = sim.fmt.literal "done at "
    %f2 = sim.fmt.dec %nowInt specifierWidth 0 : i64
    %m2 = sim.fmt.concat (%l2, %f2, %nl)
    sim.proc.print %m2
    sim.terminate success, quiet
    llhd.halt
  }
  hw.output
}
