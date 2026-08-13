// DESCRIPTION: Verilator: Model manifest effect classification test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-License-Identifier: CC0-1.0

module t (
    input  logic        clk,
    input  logic [31:0] value_i,
    output logic [31:0] value_o
);
  import "DPI-C" pure function int host_transform(input int value);

  logic delayed_q;

  initial begin
    delayed_q = 1'b0;
    #1 delayed_q = 1'b1;
  end

  always_comb value_o = host_transform(value_i) ^ {31'b0, delayed_q ^ clk};
endmodule
