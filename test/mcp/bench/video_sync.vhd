-- Video sync generator.
--
-- Specification: a line is exactly 16 clocks: hcount runs 0 to 15 then
-- returns to 0.  When hcount returns to 0, vcount increments; a frame is
-- 6 lines, vcount runs 0 to 5 then returns to 0.  hsync is high while
-- hcount is 12 to 15.  vsync is high during the whole of line 5.
-- active is high for hcount 0 to 9 on lines 0 to 3.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity video_sync is
  port (clk, rst : in std_logic;
        hsync, vsync, active : out std_logic);
end entity;

architecture rtl of video_sync is
  signal hcount : unsigned(4 downto 0);
  signal vcount : unsigned(2 downto 0);
begin
  hsync  <= '1' when hcount >= 12 else '0';
  vsync  <= '1' when vcount = 5 else '0';
  active <= '1' when hcount < 10 and vcount < 4 else '0';

  process (clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        hcount <= (others => '0');
        vcount <= (others => '0');
      elsif hcount = 15 then
        hcount <= (others => '0');
        if vcount = 5 then
          vcount <= (others => '0');
        else
          vcount <= vcount + 1;
        end if;
      else
        hcount <= hcount + 1;
      end if;
    end if;
  end process;
end architecture;
