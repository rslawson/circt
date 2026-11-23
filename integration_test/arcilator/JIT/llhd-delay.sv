// REQUIRES: slang
// REQUIRES: arcilator-jit
// RUN: circt-verilog %s --timescale=1ns/1ns | arcilator --run --jit-entry=top_main | FileCheck %s

// Delayed values, such as nonblocking assignments in combinational processes
// and delayed continuous assignments, follow their input after the delay.

// The enable is passed through a delta delay, as in clock gates of the
// XuanTie cores.
module ClockGate(input logic clk, input logic en, output logic gclk);
  logic latch, en_q;
  always_latch if (!clk) latch <= en;
  always_comb en_q <= latch;
  assign gclk = clk & en_q;
endmodule

module Delay(input logic clk, input logic a, output logic q);
  logic b;
  assign #3 b = a;
  always_ff @(posedge clk) q <= b;
endmodule

module top;
  logic clk = 0, en = 0, a = 0;
  logic gclk, q;
  ClockGate gate(.clk, .en, .gclk);
  Delay delay(.clk, .a, .q);

  always #5 clk = ~clk;
  always @(posedge gclk) $display("gclk %0t", $time);
  always @(q) $display("q=%0d %0t", q, $time);

  // CHECK:      gclk 25
  // CHECK-NEXT: q=1 25
  // CHECK-NEXT: gclk 35
  // CHECK-NEXT: gclk 45
  // CHECK-NEXT: q=0 45
  // CHECK-NEXT: gclk 55
  // CHECK-NEXT: gclk 65
  // CHECK-NEXT: q=1 65
  // CHECK-NEXT: done 90
  initial begin
    #13 a = 1;  // b rises at 16
    #9  en = 1; // gclk is enabled from the next rising edge of clk at 25
    #1  a = 0;  // b falls at 26
    #8  a = 1;  // b rises at 34, before the rising edge of clk at 35
    #10 a = 0;  // b falls at 44
    #12 a = 1;  // b rises at 56, after the rising edge of clk at 55
    #19 en = 0; // gclk is disabled from the next rising edge of clk at 75
    #18 $display("done %0t", $time);
    $finish;
  end
endmodule
