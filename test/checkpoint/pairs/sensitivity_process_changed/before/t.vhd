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
  acc_p: process (clk)
    variable n : integer := 0;
  begin
    if clk = '1' then n := n + 1; flag <= n; end if;
  end process;
end;
