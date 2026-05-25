-- Regression test for nickg/nvc#1450: expression coverage of a short
-- circuit logical operator left the left-hand-side controlling bin (10 for
-- or/nor, 01 for and/nand) permanently uncovered because the right hand
-- side, and therefore the coverage point, was skipped when the operator
-- short circuited. Driving only the short-circuiting cases must now reach
-- full expression coverage.
entity cover32 is
end entity;

architecture tb of cover32 is
  -- A user function call on the right hand side forces short-circuit
  -- lowering of the logical operators.
  function ident(x : boolean) return boolean is
  begin
    return x;
  end function;

  signal a, b          : boolean := false;
  signal r_or, r_nor   : boolean := false;
  signal r_and, r_nand : boolean := false;
begin
  r_or   <= a or   ident(b);
  r_nor  <= a nor  ident(b);
  r_and  <= a and  ident(b);
  r_nand <= a nand ident(b);

  process
  begin
    a <= false; b <= false; wait for 1 ns;
    a <= false; b <= true;  wait for 1 ns;
    a <= true;  b <= false; wait for 1 ns;
    a <= true;  b <= true;  wait for 1 ns;
    wait for 1 ns;
    std.env.stop;
  end process;
end architecture;
