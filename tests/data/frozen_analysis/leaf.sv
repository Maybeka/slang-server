module stable_leaf #(parameter int WIDTH = types_pkg::BASE_WIDTH) ();
    logic [WIDTH-1:0] payload;
    types_pkg::word_t external_word;
    if (WIDTH > 8) begin : wide
        logic [WIDTH-1:0] generated_word;
    end
endmodule
