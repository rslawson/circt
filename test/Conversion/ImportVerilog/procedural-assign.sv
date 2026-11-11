// RUN: circt-translate --import-verilog %s | FileCheck %s
// RUN: circt-verilog --ir-moore %s
// REQUIRES: slang

// Procedural continuous assignments and forces select themselves as the active
// assignment of their target, and are applied by a module-level `always_comb`
// procedure while they are selected and the target differs from the assigned
// value.

// CHECK-LABEL: moore.module @ProceduralAssign
module ProceduralAssign;
  int a;
  logic b;
  initial begin
    // CHECK: moore.procedure initial {
    // CHECK:   [[ONE:%.+]] = moore.constant 1 : i32
    // CHECK:   moore.blocking_assign [[SEL:%.+]], [[ONE]] : i32
    // CHECK:   moore.blocking_assign %b
    assign b = (a == 2);
    // CHECK:   [[TWO:%.+]] = moore.constant 2 : i32
    // CHECK:   moore.blocking_assign [[SEL]], [[TWO]] : i32
    // CHECK:   moore.blocking_assign %b
    force b = (a == 3);
    // CHECK:   [[ZERO:%.+]] = moore.constant 0 : i32
    // CHECK:   moore.blocking_assign [[SEL]], [[ZERO]] : i32
    deassign b;
  end
  // CHECK: [[SEL]] = moore.variable {{.*}} : <i32>
  // CHECK: moore.procedure always_comb {
  // CHECK:   [[ACTIVE:%.+]] = moore.read [[SEL]]
  // CHECK:   [[ID:%.+]] = moore.constant 1 : i32
  // CHECK:   [[SELECTED:%.+]] = moore.eq [[ACTIVE]], [[ID]] : i32 -> i1
  // CHECK:   [[CURRENT:%.+]] = moore.read %b
  // CHECK:   [[DIFFERS:%.+]] = moore.case_ne [[CURRENT]], {{%.+}} : l1
  // CHECK:   [[COND:%.+]] = moore.and [[SELECTED]], [[DIFFERS]] : i1
  // CHECK:   [[BOOL:%.+]] = moore.to_builtin_int [[COND]] : i1
  // CHECK:   cf.cond_br [[BOOL]], ^[[APPLY:.+]], ^[[EXIT:.+]]
  // CHECK: ^[[APPLY]]:
  // CHECK:   moore.blocking_assign %b
  // CHECK:   cf.br ^[[EXIT]]
  // CHECK: ^[[EXIT]]:
  // CHECK:   moore.return
  // CHECK: moore.procedure always_comb {
  // CHECK:   moore.constant 2 : i32
endmodule
