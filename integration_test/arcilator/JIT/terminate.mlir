// RUN: arcilator --run %s --jit-entry=Foo_main | FileCheck %s
// REQUIRES: arcilator-jit

// A free-running process ticks every 1ns forever. A second process terminates
// the simulation after 3.5ns. The generated driver must stop evaluating the
// model as soon as termination has been requested.

// CHECK:      tick 0
// CHECK-NEXT: tick 1000000
// CHECK-NEXT: tick 2000000
// CHECK-NEXT: tick 3000000
// CHECK-NEXT: finish at 3500000
// CHECK-NOT:  tick

hw.module @Foo() {
  %0 = llhd.constant_time <1ns, 0d>
  %1 = llhd.constant_time <3500ps, 0d>
  llhd.process {
    cf.br ^bb1
  ^bb1:
    %now = llhd.current_time
    %nowInt = llhd.time_to_int %now
    %lit = sim.fmt.literal "tick "
    %dec = sim.fmt.dec %nowInt : i64
    %nl = sim.fmt.literal "\0A"
    %msg = sim.fmt.concat (%lit, %dec, %nl)
    sim.proc.print %msg
    llhd.wait delay %0, ^bb1
  }
  llhd.process {
    llhd.wait delay %1, ^bb1
  ^bb1:
    %now = llhd.current_time
    %nowInt = llhd.time_to_int %now
    %lit = sim.fmt.literal "finish at "
    %dec = sim.fmt.dec %nowInt : i64
    %nl = sim.fmt.literal "\0A"
    %msg = sim.fmt.concat (%lit, %dec, %nl)
    sim.proc.print %msg
    sim.terminate success, quiet
    ub.unreachable
  }
  hw.output
}
