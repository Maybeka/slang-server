module top;
    stable_leaf #(.WIDTH(types_pkg::BASE_WIDTH)) u_frozen();
    stable_leaf #(.WIDTH(16)) u_other();
endmodule
