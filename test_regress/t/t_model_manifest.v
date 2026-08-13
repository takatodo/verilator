// DESCRIPTION: Verilator: Model manifest test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

interface status_if;
  logic done;
endinterface

module t (
    input logic clk,
    input logic [7:0] data_i,
    output logic [7:0] data_o
);
  logic [7:0] state_q;
  logic __Vuser_q;
  status_if status ();

  always_ff @(posedge clk) begin
    state_q <= data_i;
    __Vuser_q <= data_i[0];
    status.done <= data_i[1];
  end
  assign data_o = state_q ^ {6'b0, status.done, __Vuser_q};
endmodule
