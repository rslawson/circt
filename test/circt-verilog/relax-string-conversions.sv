// RUN: circt-verilog --ir-moore --relax-string-conversions %s | FileCheck %s
// RUN: not circt-verilog --ir-moore %s 2>&1 | FileCheck %s --check-prefix=ERR
// REQUIRES: slang

// Verilog code commonly stores file names in integral variables and passes
// them to system tasks that expect strings.

// ERR: error: no implicit conversion from 'reg[1023:0]' to 'string'

// CHECK-LABEL: moore.module @Foo
module Foo;
  reg [1023:0] name;
  integer fd;
  initial begin
    // CHECK: [[NAME:%.+]] = moore.read %name
    // CHECK: [[INT:%.+]] = moore.logic_to_int [[NAME]] : l1024
    // CHECK: [[STR:%.+]] = moore.int_to_string [[INT]] : i1024
    // CHECK: moore.builtin.fopen [[STR]]
    fd = $fopen(name, "w");
  end
endmodule
