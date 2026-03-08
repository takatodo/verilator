module t (
    output [31:0] y,
    output [31:0] z,
    output [31:0] w,
    output [31:0] v
);
    // Array depth 20 > kSimAccelPreloadMaterializeMaxDepth (16).
    // All elements are referenced via constant index in combinational logic,
    // so each element is resolved to a synthetic var by the sim-accel lowering pass.
    // This validates that large arrays with fully-referenced elements are correctly
    // covered by the preload target elements[] list even when depth exceeds the
    // proactive materialization threshold.
    reg [31:0] hidden_mem [0:19] /*verilator public_flat_rw*/;

    assign y = hidden_mem[0] + hidden_mem[1] + hidden_mem[2] + hidden_mem[3]
             + hidden_mem[4] + hidden_mem[5] + hidden_mem[6] + hidden_mem[7]
             + hidden_mem[8] + hidden_mem[9];

    assign z = hidden_mem[10] ^ hidden_mem[11] ^ hidden_mem[12] ^ hidden_mem[13]
             ^ hidden_mem[14] ^ hidden_mem[15] ^ hidden_mem[16] ^ hidden_mem[17]
             ^ hidden_mem[18] ^ hidden_mem[19];

    assign w = (hidden_mem[0] & hidden_mem[10])
             | (hidden_mem[1] & hidden_mem[11])
             | (hidden_mem[2] & hidden_mem[12])
             | (hidden_mem[3] & hidden_mem[13])
             | (hidden_mem[4] & hidden_mem[14]);

    assign v = {hidden_mem[19][15:0], hidden_mem[0][15:0]};
endmodule
