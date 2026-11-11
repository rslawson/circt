// RUN: circt-verilog --ir-moore --always-at-signals-as-comb %s | FileCheck %s
// RUN: circt-verilog --ir-moore %s | FileCheck %s --check-prefix=DEFAULT
// REQUIRES: slang

// CHECK-LABEL: moore.module @Foo
// DEFAULT-LABEL: moore.module @Foo
module Foo(input logic a, b, clk, output logic x, y, z);
  // Sensitivity lists without edges are treated as combinational logic.
  // CHECK: moore.procedure always_comb
  // DEFAULT: moore.procedure always {
  // DEFAULT: moore.wait_event
  always @(a or b) x = a & b;
  // Edges keep the event control.
  // CHECK: moore.procedure always {
  // CHECK: moore.wait_event
  always @(posedge clk) y <= a;
  // Other timing controls in the body keep the event control.
  // CHECK: moore.procedure always {
  // CHECK: moore.wait_event
  always @(a) #1 z = a;
endmodule
