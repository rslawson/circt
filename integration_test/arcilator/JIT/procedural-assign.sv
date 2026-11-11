// REQUIRES: slang
// REQUIRES: arcilator-jit
// RUN: circt-verilog %s --timescale=1ns/1ns | arcilator --run --jit-entry=top_main | FileCheck %s

// Procedural continuous assignments make a variable follow an expression,
// overriding other assignments, until it is deassigned.

// CHECK:      1: b=0
// CHECK-NEXT: 2: b=1
// CHECK-NEXT: 3: b=1
// CHECK-NEXT: 4: b=0
// CHECK-NEXT: 5: b=1
// CHECK-NEXT: 6: b=1

module top;
  int a = 1;
  logic b;
  initial begin
    assign b = (a == 2);
    #1 $display("%0t: b=%0d", $time, b);
    // The assignment follows changes of the expression.
    a = 2;
    #1 $display("%0t: b=%0d", $time, b);
    // The assignment overrides other assignments.
    b = 0;
    #1 $display("%0t: b=%0d", $time, b);
    // After a deassignment, the variable can be assigned again.
    deassign b;
    b = 0;
    #1 $display("%0t: b=%0d", $time, b);
    // After a release, the variable keeps its value.
    a = 1;
    force b = (a == 1);
    #1 $display("%0t: b=%0d", $time, b);
    release b;
    a = 2;
    #1 $display("%0t: b=%0d", $time, b);
    $finish;
  end
endmodule
