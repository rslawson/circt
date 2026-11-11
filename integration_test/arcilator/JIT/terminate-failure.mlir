// RUN: arcilator --run %s --jit-entry=Pass_main
// RUN: not arcilator --run %s --jit-entry=Fail_main
// REQUIRES: arcilator-jit

// Arcilator exits with a failure code if the simulation terminates with
// failure, for example through `$fatal`.

hw.module @Pass() {
  llhd.process {
    sim.terminate success, quiet
    llhd.halt
  }
  hw.output
}

hw.module @Fail() {
  llhd.process {
    sim.terminate failure, quiet
    llhd.halt
  }
  hw.output
}
