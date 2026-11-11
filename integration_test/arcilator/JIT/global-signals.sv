// REQUIRES: slang
// REQUIRES: arcilator-jit
// RUN: circt-verilog %s --timescale=1ns/1ns --top top | arcilator --run --jit-entry=top_main | FileCheck %s

// Package variables are shared by all modules accessing them. `$timeformat`
// stores its settings in a global variable as well.

// CHECK:      12: counter=6
// CHECK-NEXT: 32: counter=8

package p;
  int counter = 5;
endpackage

module inc(input logic clk);
  always @(posedge clk) p::counter <= p::counter + 1;
endmodule

module top;
  logic clk = 0;
  always #5 clk = ~clk;
  inc u(.clk);
  initial begin
    $timeformat(-9, 1, " ns", 10);
    #12 $display("%0t: counter=%0d", $time, p::counter);
    #20 $display("%0t: counter=%0d", $time, p::counter);
    $finish;
  end
endmodule
