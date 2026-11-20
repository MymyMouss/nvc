library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity timer_tb is end entity;

architecture test of timer_tb is
  signal clk, rst : std_logic := '0';
  signal ticks : unsigned(23 downto 0);
begin
  dut : entity work.timer port map (clk, rst, ticks);
  clk <= not clk after 5 ns;

  rst <= '1', '0' after 25 ns;

  report_proc : process
  begin
    wait for 280 us;
    report "ticks after 280 us: " & integer'image(to_integer(ticks));
    wait;
  end process;

  --CHECK-BEGIN
  -- The edge at 25 ns is the first one after reset (the reset input falls
  -- at that time too) and a tick follows every 4 edges; at an edge the
  -- counter still shows the previous edges
  check : process
    variable edges : natural := 0;
  begin
    wait for 20 ns;
    loop
      wait until rising_edge(clk);
      edges := edges + 1;
      if edges mod 100 = 0 then
        assert to_integer(ticks) = (edges - 1) / 4
          report "ticks is " & integer'image(to_integer(ticks))
          & ", expected " & integer'image((edges - 1) / 4)
          severity failure;
      end if;
    end loop;
  end process;
  --CHECK-END
end architecture;
