entity t is end;
architecture a of t is
  signal clk : bit := '0';
  signal d : integer := 0;
  signal log : integer := 0;
begin
  clk <= not clk after 5 ns;
  stim: process
    procedure step (n : integer) is
    begin
      for j in 1 to n loop
        d <= d + j + j / 6;
        wait for 15 ns;
      end loop;
    end procedure;
    procedure outer (k : integer) is
    begin
      d <= d + k;
      step(k * 2);
    end procedure;
  begin
    wait for 20 ns;
    for i in 1 to 3 loop
      outer(i);
    end loop;
    report "d=" & integer'image(d);
    wait;
  end process;
  mon: process (clk) begin
    if clk = '1' then log <= log + d; end if;
  end process;
end;
