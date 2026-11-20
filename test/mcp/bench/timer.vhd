-- 24 bit tick counter.
--
-- Specification: a tick happens every 4 clocks.  ticks is the number of
-- ticks since reset, kept as two 12 bit halves: lo counts ticks and wraps
-- from 4095 to 0, and when it wraps hi increases by one.  ticks is hi
-- followed by lo.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity timer is
  port (clk, rst : in std_logic;
        ticks    : out unsigned(23 downto 0));
end entity;

architecture rtl of timer is
  signal pre : natural range 0 to 3;
  signal lo  : unsigned(11 downto 0);
  signal hi  : unsigned(11 downto 0);
begin
  ticks <= hi & lo;

  process (clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        pre <= 0;
        lo  <= (others => '0');
        hi  <= (others => '0');
      elsif pre = 3 then
        pre <= 0;
        if lo = 4095 then
          lo <= (others => '0');
          hi <= hi + 1;
        else
          lo <= lo + 1;
        end if;
      else
        pre <= pre + 1;
      end if;
    end if;
  end process;
end architecture;
