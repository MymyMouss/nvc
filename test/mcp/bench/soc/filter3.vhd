-- Horizontal three tap filter.
--
-- When enable is high each pixel p(x) of a line becomes
--   (p(x - 1) + 2 * p(x) + p(x + 1)) / 4   (rounded down)
-- where the missing neighbour at either end of the line is replaced by the
-- pixel itself: p(-1) = p(0) and p(width) = p(width - 1).  When enable is
-- low pixels pass unchanged.  A pixel comes out once its right neighbour
-- has arrived, and the last pixel of a line one clock after it arrived, so
-- the line needs at least one blank clock after it.  A pixel still held
-- when a new frame starts (sof), because the video was stopped in the
-- middle of a line, is dropped.  The sof, eol and eof marks follow their
-- pixels.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;

entity filter3 is
  port (clk, rst, enable    : in std_logic;
        i_valid             : in std_logic;
        i_pix               : in pixel_t;
        i_sof, i_eol, i_eof : in std_logic;
        o_valid             : out std_logic;
        o_pix               : out pixel_t;
        o_sof, o_eol, o_eof : out std_logic);
end entity;

architecture rtl of filter3 is
  signal have      : std_logic;              -- cur holds a pixel
  signal cur, prev : pixel_t;
  signal first     : std_logic;              -- cur is first of its line
  signal c_sof     : std_logic;
  signal c_eol     : std_logic;
  signal c_eof     : std_logic;

  function tap (l, c, r : pixel_t; en : std_logic) return pixel_t is
  begin
    if en = '1' then
      return to_unsigned((to_integer(l) + 2 * to_integer(c)
                          + to_integer(r)) / 4, 8);
    else
      return c;
    end if;
  end function;
begin
  process (clk)
    variable left : pixel_t;
  begin
    if rising_edge(clk) then
      o_valid <= '0';
      o_sof   <= '0';
      o_eol   <= '0';
      o_eof   <= '0';
      if rst = '1' then
        have <= '0';
      else
        if first = '1' then
          left := cur;
        else
          left := prev;
        end if;

        if have = '1' and c_eol = '1' then
          -- Last pixel of the line: its right neighbour is itself
          o_valid <= '1';
          o_pix   <= tap(left, cur, cur, enable);
          o_sof   <= c_sof;
          o_eol   <= '1';
          o_eof   <= c_eof;
          have    <= '0';
        elsif have = '1' and i_valid = '1' and i_sof = '0' then
          o_valid <= '1';
          o_pix   <= tap(left, cur, i_pix, enable);
          o_sof   <= c_sof;
        end if;

        if i_valid = '1' then
          prev  <= cur;
          cur   <= i_pix;
          have  <= '1';
          first <= i_sof or (have and c_eol) or not have;
          c_sof <= i_sof;
          c_eol <= i_eol;
          c_eof <= i_eof;
        end if;
      end if;
    end if;
  end process;
end architecture;
