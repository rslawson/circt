// RUN: arcilator %s --run --jit-entry=Top_main +verbose +count=42 +mask=c0ffee +name=AB | FileCheck %s
// RUN: arcilator %s --run --jit-entry=Top_main | FileCheck %s --check-prefix=NOARGS
// REQUIRES: arcilator-jit

// Plusargs passed to arcilator are visible to `$test$plusargs` and
// `$value$plusargs` in the simulation.

// CHECK:      verbose=1 quiet=0
// CHECK-NEXT: count=1,42 mask=1,c0ffee name=1,4142 missing=0
// NOARGS:      verbose=0 quiet=0
// NOARGS-NEXT: count=0,0 mask=0,0 name=0,0 missing=0

hw.module @Top() {
  llhd.process {
    %verbose = sim.plusargs.test "verbose"
    %quiet = sim.plusargs.test "quiet"
    %countFound, %count = sim.plusargs.value "count=%d" : i32
    %maskFound, %mask = sim.plusargs.value "mask=%h" : i64
    %nameFound, %name = sim.plusargs.value "name=%s" : i16
    %missingFound, %missing = sim.plusargs.value "missing=%d" : i8
    %l0 = sim.fmt.literal "verbose="
    %l1 = sim.fmt.literal " quiet="
    %l2 = sim.fmt.literal "count="
    %l3 = sim.fmt.literal " mask="
    %l4 = sim.fmt.literal " name="
    %l5 = sim.fmt.literal " missing="
    %comma = sim.fmt.literal ","
    %nl = sim.fmt.literal "\0A"
    %f0 = sim.fmt.dec %verbose specifierWidth 0 : i1
    %f1 = sim.fmt.dec %quiet specifierWidth 0 : i1
    %f2 = sim.fmt.dec %countFound specifierWidth 0 : i1
    %f3 = sim.fmt.dec %count specifierWidth 0 : i32
    %f4 = sim.fmt.dec %maskFound specifierWidth 0 : i1
    %f5 = sim.fmt.hex %mask, isUpper false specifierWidth 0 : i64
    %f6 = sim.fmt.dec %nameFound specifierWidth 0 : i1
    %f7 = sim.fmt.hex %name, isUpper false specifierWidth 0 : i16
    %f8 = sim.fmt.dec %missingFound specifierWidth 0 : i1
    %m0 = sim.fmt.concat (%l0, %f0, %l1, %f1, %nl)
    %m1 = sim.fmt.concat (%l2, %f2, %comma, %f3, %l3, %f4, %comma, %f5, %l4, %f6, %comma, %f7, %l5, %f8, %nl)
    sim.proc.print %m0
    sim.proc.print %m1
    llhd.halt
  }
  hw.output
}
