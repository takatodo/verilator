module t (
    output [31:0] y,
    output [31:0] z
);
    // Array depth 20 > kSimAccelPreloadMaterializeMaxDepth (16).
    // Only elements 0-9 are referenced in combinational logic.
    // Elements 10-19 are declared but not accessed in the supported subset.
    // This validates the split behavior: referenced elements appear in elements[]
    // and are applied, while truly hidden elements are stored but not applied.
    reg [31:0] hidden_mem [0:19] /*verilator public_flat_rw*/;

    assign y = hidden_mem[0] + hidden_mem[1] + hidden_mem[2] + hidden_mem[3]
             + hidden_mem[4] + hidden_mem[5] + hidden_mem[6] + hidden_mem[7]
             + hidden_mem[8] + hidden_mem[9];

    assign z = (hidden_mem[0] & hidden_mem[4]) | (hidden_mem[2] ^ hidden_mem[7]);
endmodule
