module t (
    input [31:0] a,
    input [31:0] b,
    input [31:0] c,
    input [31:0] d,
    output [31:0] y,
    output [31:0] z,
    output [31:0] w
);
    wire [31:0] n0;
    wire [31:0] n1;
    wire [31:0] n2;

    assign n0 = a & b;
    assign n1 = c ^ d;
    assign n2 = ~n1;
    assign y = n0 | n2;
    assign z = (a ^ c) & ((~b) | d);
    assign w = y ^ z;
endmodule
