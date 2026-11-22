entity t is end;
architecture a of t is
  signal clk : bit := '0';
  signal d : integer := 0;
  signal log : integer := 0;
begin
  clk <= not clk after 5 ns;
  stim: process
  begin
    wait for 20 ns;
    for i in 1 to 8 loop
      d <= d + i + i / 8;
      wait for 25 ns;
    end loop;
    report "d=" & integer'image(d);
    wait;
  end process;
  mon: process (clk) begin
    if clk = '1' then log <= log + d; end if;
  end process;
end;
