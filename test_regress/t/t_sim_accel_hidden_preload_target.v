module t (
    input [31:0] a,
    input [31:0] b,
    input [31:0] c,
    input [31:0] d,
    output [31:0] y,
    output [31:0] z
);
    reg [31:0] hidden_mem [0:3] /*verilator public_flat_rw*/;

    assign y = a + b;
    assign z = (c ^ d) + a;
endmodule
