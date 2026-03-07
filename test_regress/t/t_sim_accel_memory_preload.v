module t (
    input [31:0] mem_0,
    input [31:0] mem_1,
    input [31:0] mem_2,
    input [31:0] mem_3,
    output [31:0] y,
    output [31:0] z
);
    wire [31:0] s0;
    wire [31:0] s1;
    wire [31:0] s2;

    assign s0 = mem_0 + mem_1;
    assign s1 = mem_2 ^ mem_3;
    assign s2 = {mem_0[15:0], mem_2[15:0]};

    assign y = s0 ^ s1;
    assign z = ((mem_0 & mem_2) | (mem_1 + mem_3)) ^ s2;
endmodule
