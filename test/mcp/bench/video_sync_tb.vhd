library ieee;
use ieee.std_logic_1164.all;

entity video_sync_tb is end entity;

architecture test of video_sync_tb is
  signal clk, rst, hsync, vsync, active : std_logic := '0';
begin
  dut : entity work.video_sync port map (clk, rst, hsync, vsync, active);
  clk <= not clk after 5 ns;

  process
  begin
    rst <= '1';
    wait for 25 ns;
    rst <= '0';
    wait;
  end process;

  -- Every hsync rising edge must come 16 clocks (160 ns) after the last
  check : process
    variable last : time := 0 ns;
  begin
    loop
      wait until rising_edge(hsync);
      if last > 0 ns then
        assert now - last = 160 ns
          report "line period is " & time'image(now - last)
          severity failure;
      end if;
      last := now;
      exit when now > 2 us;
    end loop;
    report "video_sync_tb finished";
    wait;
  end process;
end architecture;
