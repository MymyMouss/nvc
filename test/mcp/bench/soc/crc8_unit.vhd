-- Running CRC-8 over a byte stream.
--
-- clr sets crc to zero.  Otherwise, when en is high, crc becomes
-- crc8_next(crc, din).  clr wins over en.
library ieee;
use ieee.std_logic_1164.all;
use work.pkg_soc.all;

entity crc8_unit is
  port (clk, rst, clr, en : in std_logic;
        din               : in byte_t;
        crc               : out byte_t);
end entity;

architecture rtl of crc8_unit is
  signal c : byte_t;
begin
  crc <= c;

  process (clk)
  begin
    if rising_edge(clk) then
      if rst = '1' or clr = '1' then
        c <= (others => '0');
      elsif en = '1' then
        c <= crc8_next(c, din);
      end if;
    end if;
  end process;
end architecture;
