// RUN: not arcilator --run %s --jit-entry=Top_main 2>&1 | FileCheck %s
// REQUIRES: arcilator-jit

// A signal that inverts itself never settles. The simulation is aborted after
// too many evaluations at the same time.

// CHECK: Error: simulation does not settle after 100000 evaluations at time 0 fs

hw.module @Top() {
  %true = hw.constant true
  %false = hw.constant false
  %s = llhd.sig %false : <i1>
  %0 = llhd.prb %s : i1
  %1 = comb.xor %0, %true : i1
  llhd.assign %s, %1 : i1
  hw.output
}
