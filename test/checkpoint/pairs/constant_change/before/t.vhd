entity t is end;
architecture a of t is
  signal clk : bit := '0';
  signal cnt : integer := 0;
  signal flag : integer := 0;
  constant K : integer := 20;
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
    if cnt > K then report "big"; else report "small"; end if;
    wait for 200 ns;
    report "p3 " & integer'image(cnt + acc);
    wait;
  end process;
end;
