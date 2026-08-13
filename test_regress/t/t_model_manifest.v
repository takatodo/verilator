// DESCRIPTION: Verilator: Model manifest test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t (
    input  logic       clk,
    input  logic [7:0] data_i,
    output logic [7:0] data_o
);
    logic [7:0] state_q;

    always_ff @(posedge clk) state_q <= data_i;
    assign data_o = state_q;
endmodule
