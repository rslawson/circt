// RUN: circt-opt %s --arc-strip-sv --verify-diagnostics | FileCheck %s

// CHECK-NOT: sv.verbatim
// CHECK-NOT: sv.macro.decl
// CHECK-NOT: sv.ifdef
sv.verbatim "// Standard header to adapt well known macros to our needs." {symbols = []}
sv.macro.decl @RANDOMIZE_REG_INIT
sv.ifdef  @RANDOMIZE_REG_INIT {
  sv.verbatim "`define RANDOMIZE" {symbols = []}
}

// CHECK-LABEL: hw.module @Foo(
hw.module @Foo(in %clock: !seq.clock, in %a: i4, out z: i4) {
  // CHECK-NEXT: [[REG:%.+]] = seq.compreg %a, %clock
  %0 = seq.firreg %a clock %clock : i4
  %1 = sv.wire : !sv.net<i4>
  sv.assign %1, %0 : !sv.net<i4>, i4
  %2 = sv.read_inout %1 : !sv.net<i4>
  // CHECK-NEXT: hw.output [[REG]]
  hw.output %2 : i4
}
// CHECK-NEXT: }

// CHECK-LABEL: hw.module.extern @PeripheryBus
hw.module.extern @PeripheryBus(out clock: !seq.clock, out reset: i1)
// CHECK: hw.module @Top
hw.module @Top() {
  %c0_i7 = hw.constant 0 : i7
  // CHECK: %subsystem_pbus.clock, %subsystem_pbus.reset = hw.instance "subsystem_pbus" @PeripheryBus() -> (clock: !seq.clock, reset: i1)
  %subsystem_pbus.clock, %subsystem_pbus.reset = hw.instance "subsystem_pbus" @PeripheryBus() -> (clock: !seq.clock, reset: i1)
  // CHECK: %int_rtc_tick_value = seq.compreg %int_rtc_tick_value, %subsystem_pbus.clock reset %subsystem_pbus.reset, %c0_i7 : i7
  %int_rtc_tick_value = seq.firreg %int_rtc_tick_value clock %subsystem_pbus.clock reset sync %subsystem_pbus.reset, %c0_i7 : i7
}

// CHECK-NOT: sv.macro.decl
sv.macro.decl @RANDOM

// CHECK-LABEL: hw.module @HasBeenReset(
hw.module @HasBeenReset(in %clock: i1, in %reset: i1, out z: i1) {
  // CHECK-DAG: [[CLK:%.+]] = seq.to_clock %clock
  // CHECK-DAG: [[INIT:%.+]] = seq.initial() {
  // CHECK-DAG:   [[FALSE:%.+]] = hw.constant false
  // CHECK-DAG:   seq.yield [[FALSE]]
  // CHECK-DAG: }
  // CHECK-DAG: [[TRUE:%.+]] = hw.constant true
  // CHECK-DAG: [[REG:%.+]] = seq.compreg [[REG]], [[CLK]] reset %reset, [[TRUE]] initial [[INIT]] : i1
  // CHECK-DAG: [[NOT_RESET:%.+]] = comb.xor %reset, %{{.+}} : i1
  // CHECK-DAG: [[OUT:%.+]] = comb.and bin [[REG]], [[NOT_RESET]] : i1
  %0 = verif.has_been_reset %clock, sync %reset
  // CHECK: hw.output [[OUT]]
  hw.output %0 : i1
}

// Registers with an asynchronous reset are forced to their reset value after an
// assertion of the reset until the next clock edge. A register clocked by the
// reset is set to the inverse of a register in the clock domain, which in turn
// samples it on every clock edge. The assertion takes effect one delta cycle
// later through a signal. The registers themselves are reset synchronously.
// CHECK-LABEL: hw.module @AsyncReset
hw.module @AsyncReset(in %clk : !seq.clock, in %clk2 : !seq.clock, in %rst : i1, in %a : i8, in %b : i8, in %c : i8, out x : i8, out y : i8, out z : i8) {
  %c0_i8 = hw.constant 0 : i8
  %c1_i8 = hw.constant 1 : i8
  // CHECK-DAG: [[X:%.+]] = seq.compreg %a, %clk reset %rst, %c0_i8 : i8
  // CHECK-DAG: [[RSTCLK:%.+]] = seq.to_clock %rst
  // CHECK-DAG: [[ASSERTED:%.+]] = seq.compreg [[NOTSAMPLED:%.+]], [[RSTCLK]] initial {{%.+}} : i1
  // CHECK-DAG: [[SAMPLED:%.+]] = seq.compreg [[ASSERTED]], %clk initial {{%.+}} : i1
  // CHECK-DAG: [[NOTSAMPLED]] = comb.xor bin [[SAMPLED]], %true : i1
  // CHECK-DAG: [[SIG:%.+]] = llhd.sig %false : <i1>
  // CHECK-DAG: [[DELTA:%.+]] = llhd.constant_time <0ns, 1d>
  // CHECK-DAG: llhd.drv [[SIG]], [[ASSERTED]] after [[DELTA]] : i1
  // CHECK-DAG: [[DELAYED:%.+]] = llhd.prb [[SIG]] : i1
  // CHECK-DAG: [[PENDING:%.+]] = comb.xor bin [[DELAYED]], [[SAMPLED]] : i1
  // CHECK-DAG: [[XOUT:%.+]] = comb.mux [[PENDING]], %c0_i8, [[X]] : i8
  %x = seq.firreg %a clock %clk reset async %rst, %c0_i8 : i8
  // Registers with the same clock and reset share the override.
  // CHECK-DAG: [[Y:%.+]] = seq.compreg %b, %clk reset %rst, %c1_i8 : i8
  // CHECK-DAG: [[YOUT:%.+]] = comb.mux [[PENDING]], %c1_i8, [[Y]] : i8
  %y = seq.firreg %b clock %clk reset async %rst, %c1_i8 : i8
  // Registers with a different clock get their own override.
  // CHECK-DAG: [[Z:%.+]] = seq.compreg %c, %clk2 reset %rst, %c0_i8 : i8
  // CHECK-DAG: [[ASSERTED2:%.+]] = seq.compreg [[NOTSAMPLED2:%.+]], [[RSTCLK]] initial {{%.+}} : i1
  // CHECK-DAG: [[SAMPLED2:%.+]] = seq.compreg [[ASSERTED2]], %clk2 initial {{%.+}} : i1
  // CHECK-DAG: [[NOTSAMPLED2]] = comb.xor bin [[SAMPLED2]], %true{{.*}} : i1
  // CHECK-DAG: [[SIG2:%.+]] = llhd.sig %false{{.*}} : <i1>
  // CHECK-DAG: llhd.drv [[SIG2]], [[ASSERTED2]] after {{%.+}} : i1
  // CHECK-DAG: [[DELAYED2:%.+]] = llhd.prb [[SIG2]] : i1
  // CHECK-DAG: [[PENDING2:%.+]] = comb.xor bin [[DELAYED2]], [[SAMPLED2]] : i1
  // CHECK-DAG: [[ZOUT:%.+]] = comb.mux [[PENDING2]], %c0_i8, [[Z]] : i8
  %z = seq.firreg %c clock %clk2 reset async %rst, %c0_i8 : i8
  // CHECK: hw.output [[XOUT]], [[YOUT]], [[ZOUT]]
  hw.output %x, %y, %z : i8, i8, i8
}
