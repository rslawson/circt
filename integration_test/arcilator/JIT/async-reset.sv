// REQUIRES: slang
// REQUIRES: arcilator-jit
// RUN: circt-verilog %s --timescale=1ns/1ns | arcilator --run --jit-entry=top_main | FileCheck %s

// Registers with an asynchronous reset take on their reset value as soon as
// the reset is asserted, and keep it until the first clock edge after the
// reset is released, even if the reset pulse falls between two clock edges.

// CHECK:      2 q=100
// CHECK-NEXT: 5 q=101
// CHECK-NEXT: 15 q=102
// CHECK-NEXT: 24 now=102
// CHECK-NEXT: 25 q=103
// CHECK-NEXT: 27 q=100
// CHECK-NEXT: 28 now=100
// CHECK-NEXT: 39 now=100
// CHECK-NEXT: 45 q=101
// CHECK-NEXT: 55 q=102
// CHECK-NEXT: 59 now=102
// CHECK-NEXT: 61 q=100
// CHECK-NEXT: 63 now=100
// CHECK-NEXT: 65 q=101
// CHECK-NEXT: 73 now=101
// CHECK-NEXT: 75 q=100
// CHECK-NEXT: 79 now=100

module top;
  logic clk = 0, rst_n = 1;
  logic [7:0] q;
  always #5 clk = ~clk;

  always @(posedge clk or negedge rst_n)
    if (!rst_n) q <= 8'd100;
    else q <= q + 1;

  initial begin
    // Reset between two clock edges.
    #2 rst_n = 0;
    #2 rst_n = 1;
    #20 $display("%0t now=%0d", $time, q);
    // Reset spanning a clock edge.
    #3 rst_n = 0;
    #1 $display("%0t now=%0d", $time, q);
    #10 rst_n = 1;
    #1 $display("%0t now=%0d", $time, q);
    #20 $display("%0t now=%0d", $time, q);
    // Short reset pulse.
    #2 rst_n = 0;
    #1 rst_n = 1;
    #1 $display("%0t now=%0d", $time, q);
    #10 $display("%0t now=%0d", $time, q);
    // Two reset pulses between clock edges.
    #2 rst_n = 0;
    #1 rst_n = 1;
    #1 rst_n = 0;
    #1 rst_n = 1;
    #1 $display("%0t now=%0d", $time, q);
    $finish;
  end

  always @(q) $display("%0t q=%0d", $time, q);
endmodule
