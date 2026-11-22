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
    for i in 1 to 6 loop
      wait for 50 ns;
      acc := acc + i * 1;
    end loop;
    report "acc " & integer'image(acc);
    wait;
  end process;
end;
