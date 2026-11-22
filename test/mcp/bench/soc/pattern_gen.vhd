-- Test pattern generator.
--
-- For each active pixel from the timing generator, produces one pixel one
-- clock later with the same sof, eol and eof marks:
--   pix = (5 * x + 17 * y + 3 * f + seed) mod 256
-- where f is the frame number: 0 for the first frame after enable goes
-- high, then one more after each eof.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;

entity pattern_gen is
  port (clk, rst, enable    : in std_logic;
        seed                : in byte_t;
        de, sof, eol, eof   : in std_logic;
        x, y                : in natural range 0 to 255;
        o_valid             : out std_logic;
        o_pix               : out pixel_t;
        o_sof, o_eol, o_eof : out std_logic);
end entity;

architecture rtl of pattern_gen is
  signal f : natural range 0 to 255;
begin
  process (clk)
  begin
    if rising_edge(clk) then
      o_valid <= de;
      o_sof   <= sof;
      o_eol   <= eol;
      o_eof   <= eof;
      o_pix   <= to_unsigned((5 * x + 17 * y + 3 * f
                              + to_integer(unsigned(seed))) mod 256, 8);
      if rst = '1' or enable = '0' then
        f <= 0;
      elsif de = '1' and eof = '1' then
        f <= (f + 1) mod 256;
      end if;
    end if;
  end process;
end architecture;
