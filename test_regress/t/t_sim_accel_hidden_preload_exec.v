module t (
    output [31:0] y,
    output [31:0] z,
    output [31:0] w
);
    reg [31:0] hidden_mem [0:3] /*verilator public_flat_rw*/;

    wire [31:0] s0;
    wire [31:0] s1;

    assign s0 = hidden_mem[0] + hidden_mem[1];
    assign s1 = hidden_mem[2] ^ hidden_mem[3];

    assign y = s0 ^ hidden_mem[0];
    assign z = (hidden_mem[0] & hidden_mem[2]) | (hidden_mem[1] + hidden_mem[3]);
    assign w = {hidden_mem[3][15:0], hidden_mem[0][15:0]} ^ s1;
endmodule
