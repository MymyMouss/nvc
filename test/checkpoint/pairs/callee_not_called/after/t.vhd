entity t is end;
architecture a of t is
  signal clk : bit := '0';
  signal cnt : integer := 0;
  signal flag : integer := 0;
  function scale(x : integer) return integer is
  begin
    return x * 3;
  end function;
begin
  clk <= not clk after 5 ns;
  counter: process (clk) begin
    if clk = '1' then cnt <= cnt + 1; end if;
  end process;
  main: process
    variable acc : integer := 0;
  begin
    wait for 100 ns;
    acc := acc + cnt;
    report "p1 " & integer'image(acc);

    wait for 200 ns;
    flag <= scale(cnt);
    report "p2 " & integer'image(flag);
    wait for 200 ns;
    report "p3 " & integer'image(cnt + acc);
    wait;
  end process;
end;
