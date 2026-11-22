-- Periodic timer.
--
-- With period non-zero, tick pulses once every (period + 1) * 16 clocks:
-- a prescaler divides the clock by 16 and a counter counts prescaler
-- pulses from 0 to period.  Writing period restarts nothing: the counter
-- simply compares with the new value.  With period zero the timer stops
-- and both counters stay at zero.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;

entity timer is
  port (clk, rst : in std_logic;
        period   : in byte_t;
        tick     : out std_logic);
end entity;

architecture rtl of timer is
  signal pre : unsigned(3 downto 0);
  signal cnt : unsigned(7 downto 0);
begin
  process (clk)
  begin
    if rising_edge(clk) then
      tick <= '0';
      if rst = '1' or unsigned(period) = 0 then
        pre <= (others => '0');
        cnt <= (others => '0');
      elsif pre = 15 then
        pre <= (others => '0');
        if cnt >= unsigned(period) then
          cnt  <= (others => '0');
          tick <= '1';
        else
          cnt <= cnt + 1;
        end if;
      else
        pre <= pre + 1;
      end if;
    end if;
  end process;
end architecture;
