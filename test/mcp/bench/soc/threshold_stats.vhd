-- Frame statistics.
--
-- While enable is high, counts over each frame the pixels strictly above
-- thresh and the sum of all pixels (modulo 65536).  The running totals
-- start from zero at the first pixel of each frame (sof), so a frame cut
-- short by disabling the video does not count.  On the last pixel of a
-- frame (eof) the totals, including that pixel, are copied to count and
-- sum and ready pulses for one clock.  frame_done pulses on every eof, enabled or
-- not.  When enable is low the totals are not updated.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;

entity threshold_stats is
  port (clk, rst, enable : in std_logic;
        thresh           : in byte_t;
        i_valid          : in std_logic;
        i_pix            : in pixel_t;
        i_sof, i_eof     : in std_logic;
        count, sum       : out unsigned(15 downto 0);
        ready            : out std_logic;
        frame_done       : out std_logic);
end entity;

architecture rtl of threshold_stats is
  signal acc_n, acc_s : unsigned(15 downto 0);
begin
  process (clk)
    variable n, s : unsigned(15 downto 0);
  begin
    if rising_edge(clk) then
      ready      <= '0';
      frame_done <= '0';
      if rst = '1' then
        acc_n <= (others => '0');
        acc_s <= (others => '0');
        count <= (others => '0');
        sum   <= (others => '0');
      elsif i_valid = '1' then
        if i_sof = '1' then
          n := (others => '0');
          s := (others => '0');
        else
          n := acc_n;
          s := acc_s;
        end if;
        if enable = '1' then
          if i_pix > unsigned(thresh) then
            n := n + 1;
          end if;
          s := s + i_pix;
        end if;
        if i_eof = '1' then
          frame_done <= '1';
          if enable = '1' then
            count <= n;
            sum   <= s;
            ready <= '1';
          end if;
          acc_n <= (others => '0');
          acc_s <= (others => '0');
        else
          acc_n <= n;
          acc_s <= s;
        end if;
      end if;
    end if;
  end process;
end architecture;
