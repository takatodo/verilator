module t (
    input  [31:0] a,
    input  [31:0] b,
    input  [31:0] c,
    input  [31:0] d,
    input  [31:0] e,
    input  [31:0] f,
    output [31:0] y,
    output [31:0] z
);
    wire [31:0] a0;
    wire [31:0] a1;
    wire [31:0] a2;
    wire [31:0] b0;
    wire [31:0] b1;
    wire [31:0] b2;

    assign a0 = a & b;
    assign a1 = a0 ^ c;
    assign a2 = {a1[15:0], a0[15:0]};
    assign y = a2 + a1;

    assign b0 = d | e;
    assign b1 = b0 ^ f;
    assign b2 = b1 << d[4:0];
    assign z = b2 - b1;
endmodule
