entity t is end;
architecture a of t is
  signal clk : bit := '0';
  signal cnt : integer := 0;
  signal flag : integer := 0;

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
    if cnt > 20 then report "p2 big"; else report "p2 small"; end if;
    wait for 200 ns;
    report "p3 " & integer'image(cnt + acc);
    wait;
  end process;
  check: process begin
    wait for 400 ns;
    assert cnt >= 90 report "checker: too few" severity error;
    wait;
  end process;
end;
