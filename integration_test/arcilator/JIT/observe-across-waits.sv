// REQUIRES: slang
// REQUIRES: arcilator-jit
// RUN: circt-verilog %s --timescale=1ns/1ns | arcilator --run --jit-entry=top_main | FileCheck %s

// A process observing the same variable at different points in time must see
// its current value each time, even if it computes the same expression.

// CHECK:      1: c=0 c+1=1
// CHECK-NEXT: 5: c=2 c+1=3
// CHECK-NEXT: 9: c=4 c+1=5

module top;
  logic [7:0] c = 0;
  always #2 c = c + 1;
  initial begin
    #1 $display("%0t: c=%0d c+1=%0d", $time, c, c + 8'd1);
    #4 $display("%0t: c=%0d c+1=%0d", $time, c, c + 8'd1);
    #4 $display("%0t: c=%0d c+1=%0d", $time, c, c + 8'd1);
    $finish;
  end
endmodule
