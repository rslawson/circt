// REQUIRES: slang
// REQUIRES: arcilator-jit
// RUN: circt-verilog %s --timescale=1ns/1ns --top tb | arcilator --run --jit-entry=tb_main | FileCheck %s

// A large memory written by the testbench through a hierarchical reference,
// and by the design with nonblocking assignments. The memory is accessed
// elementwise, without copying it.

// CHECK:      rd 100: 62
// CHECK-NEXT: rd 65535: 7
// CHECK-NEXT: rd 3: 89
// CHECK-NEXT: rd 3: 42

module ram(input logic clk, input logic we, input logic [15:0] addr,
           input logic [7:0] din, output logic [7:0] dout);
  logic [7:0] mem [0:65535];
  always @(posedge clk) begin
    if (we) mem[addr] <= din;
    dout <= mem[addr];
  end
endmodule

module tb;
  logic clk = 0, we = 0;
  logic [15:0] addr = 0;
  logic [7:0] din = 0, dout;
  always #5 clk = ~clk;
  ram u(.clk, .we, .addr, .din, .dout);
  initial begin
    for (int i = 0; i < 65536; i++) tb.u.mem[i] = i[7:0] ^ 8'h5a;
    @(negedge clk);
    addr = 100;
    #10 $display("rd %0d: %0d", addr, dout);
    we = 1; din = 7; addr = 65535;
    #10 we = 0;
    #10 $display("rd %0d: %0d", addr, dout);
    addr = 3;
    #10 $display("rd %0d: %0d", addr, dout);
    tb.u.mem[3] = 42; addr = 4;
    #10 addr = 3;
    #10 $display("rd %0d: %0d", addr, dout);
    $finish;
  end
endmodule
