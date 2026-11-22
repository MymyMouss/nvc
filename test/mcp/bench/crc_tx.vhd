-- Slow serial transmitter with a CRC.
--
-- Specification: when start is pulsed while not busy, the 16 bit word on
-- data is sent as a frame of 24 bits, each lasting 100 clocks: the 16 data
-- bits, most significant first, then the 8 bit CRC of the data bits, most
-- significant bit first.  busy is high from the clock after start until
-- the last bit has ended.  The CRC is a bitwise CRC-8 with polynomial 0x07
-- that starts from zero for every frame: for each data bit as it is sent,
-- crc <= (crc shifted left by one) xor 0x07 when (crc(7) xor bit) is 1.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity crc_tx is
  port (clk, rst, start : in std_logic;
        data            : in std_logic_vector(15 downto 0);
        tx, busy        : out std_logic);
end entity;

architecture rtl of crc_tx is
  signal word : std_logic_vector(15 downto 0);
  signal crc  : std_logic_vector(7 downto 0);
  signal nbit : natural range 0 to 23;
  signal tick : natural range 0 to 99;
  signal run  : std_logic;
  signal cur  : std_logic;
begin
  busy <= run;
  cur  <= word(15 - nbit) when nbit < 16 else crc(23 - nbit);
  tx   <= cur when run = '1' else '1';

  process (clk)
    variable fb : std_logic;
  begin
    if rising_edge(clk) then
      if rst = '1' then
        run  <= '0';
        crc  <= (others => '0');
        nbit <= 0;
        tick <= 0;
      elsif run = '0' then
        if start = '1' then
          word <= data;
          crc  <= (others => '0');
          nbit <= 0;
          tick <= 0;
          run  <= '1';
        end if;
      elsif tick = 99 then
        tick <= 0;
        if nbit < 16 then
          fb := crc(7) xor cur;
          if fb = '1' then
            crc <= (crc(6 downto 0) & '0') xor x"07";
          else
            crc <= crc(6 downto 0) & '0';
          end if;
        end if;
        if nbit = 23 then
          run <= '0';
        else
          nbit <= nbit + 1;
        end if;
      else
        tick <= tick + 1;
      end if;
    end if;
  end process;
end architecture;
