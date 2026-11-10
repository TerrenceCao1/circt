// Kogge-Stone carry-lookahead for Yosys $lcu (used by $add/$sub/$lt via $alu).
// Use with:  synth -extra-map kogge_stone_lcu.v
(* techmap_celltype = "$lcu" *)
module _80_lcu_kogge_stone (P, G, CI, CO);
  parameter WIDTH = 2;
  input  [WIDTH-1:0] P, G;
  input  CI;
  output [WIDTH-1:0] CO;
  integer i, j;
  reg [WIDTH-1:0] p, g;
  always @* begin
    p = P; g = G;
    g[0] = g[0] | (p[0] & CI);
    for (i = 0; i < $clog2(WIDTH); i = i + 1)
      for (j = WIDTH - 1; j >= 2**i; j = j - 1) begin   // descending: reads old values
        g[j] = g[j] | (p[j] & g[j - 2**i]);
        p[j] = p[j] & p[j - 2**i];
      end
    CO = g;
  end
endmodule
