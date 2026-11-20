-- Gain and offset.
--
-- Each pixel becomes sat8(p * gain / 16 + offset), one clock later, where
-- gain is an unsigned 4.4 fixed point number (16 is a gain of one), offset
-- a signed byte (two's complement, -128 to 127), the division rounds down
-- and sat8 clips to 0..255.  The sof, eol and eof marks follow their
-- pixels.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;

entity gain_offset is
  port (clk, rst            : in std_logic;
        gain, offset        : in byte_t;
        i_valid             : in std_logic;
        i_pix               : in pixel_t;
        i_sof, i_eol, i_eof : in std_logic;
        o_valid             : out std_logic;
        o_pix               : out pixel_t;
        o_sof, o_eol, o_eof : out std_logic);
end entity;

architecture rtl of gain_offset is
begin
  process (clk)
    variable scaled : integer;
  begin
    if rising_edge(clk) then
      o_valid <= i_valid and not rst;
      o_sof   <= i_sof;
      o_eol   <= i_eol;
      o_eof   <= i_eof;
      if i_valid = '1' then
        scaled := to_integer(i_pix) * to_integer(unsigned(gain)) / 16;
        o_pix  <= sat8(scaled + to_integer(signed(offset)));
      end if;
    end if;
  end process;
end architecture;
