-- Inactivity watchdog.
--
-- While armed is high, timeout pulses for one clock when kick has been low
-- for LIMIT consecutive clocks, then the count starts again.  The count
-- restarts whenever kick is high or armed is low.
library ieee;
use ieee.std_logic_1164.all;

entity watchdog is
  generic (LIMIT : positive := 200);
  port (clk, rst, armed, kick : in std_logic;
        timeout               : out std_logic);
end entity;

architecture rtl of watchdog is
  signal cnt : natural range 0 to LIMIT;
begin
  process (clk)
  begin
    if rising_edge(clk) then
      timeout <= '0';
      if rst = '1' or armed = '0' or kick = '1' then
        cnt <= 0;
      elsif cnt = LIMIT - 1 then
        cnt     <= 0;
        timeout <= '1';
      else
        cnt <= cnt + 1;
      end if;
    end if;
  end process;
end architecture;
