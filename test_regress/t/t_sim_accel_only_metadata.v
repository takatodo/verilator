module t(
    input logic a,
    input logic b,
    output logic y
);
    logic tmp;
    assign tmp = a ^ b;
    assign y = tmp & a;
endmodule
