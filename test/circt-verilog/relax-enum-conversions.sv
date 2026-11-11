// RUN: circt-verilog --ir-moore --relax-enum-conversions %s | FileCheck %s
// RUN: not circt-verilog --ir-moore %s 2>&1 | FileCheck %s --check-prefix=ERR
// REQUIRES: slang

// Many tools allow integral values to be assigned to enums without a cast.

// ERR: error: no implicit conversion from 'logic[3:0]' to 'e'

// CHECK-LABEL: moore.module @Foo
module Foo(input logic [3:0] a);
  typedef enum logic [3:0] { A, B } e;
  e x;
  // CHECK: %x = moore.assigned_variable %a : l4
  assign x = a;
endmodule
