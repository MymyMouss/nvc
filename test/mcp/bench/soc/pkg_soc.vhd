-- Shared definitions of the SoC.
--
-- Command packets received on the UART: A5 cmd addr len data... crc, where
-- crc is the CRC-8 (polynomial 0x07, initial value 0, see crc8_next) of
-- every byte after A5 and before the crc.  Response packets sent on the
-- UART: A5 type len payload... crc, the crc covering type, len and the
-- payload.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package pkg_soc is
  subtype byte_t is std_logic_vector(7 downto 0);
  subtype pixel_t is unsigned(7 downto 0);

  constant SYNC_BYTE : byte_t := x"A5";

  constant CMD_WRITE : byte_t := x"01";   -- write len registers from addr
  constant CMD_READ  : byte_t := x"02";   -- read len registers from addr
  constant CMD_STATS : byte_t := x"03";   -- ask for the frame statistics

  constant RSP_ACK   : byte_t := x"81";   -- payload: addr of the write
  constant RSP_DATA  : byte_t := x"82";   -- payload: the registers read
  constant RSP_STATS : byte_t := x"83";   -- payload: count hi, lo, sum hi, lo
  constant RSP_IRQ   : byte_t := x"84";   -- payload: pending interrupts
  constant RSP_NAK   : byte_t := x"8F";   -- payload: 1 bad crc, 2 bad cmd

  -- Registers
  constant REG_CTRL     : natural := 0;  -- bit 0 video, 1 filter, 2 stats,
                                         -- 3 automatic stats reports
  constant REG_WIDTH    : natural := 1;  -- pixels per line, 1 to 64
  constant REG_HEIGHT   : natural := 2;  -- lines per frame, 1 to 32
  constant REG_GAIN     : natural := 3;  -- unsigned 4.4 fixed point
  constant REG_OFFSET   : natural := 4;  -- signed, two's complement
  constant REG_THRESH   : natural := 5;  -- statistics threshold
  constant REG_TIMER    : natural := 6;  -- timer period / 16 - 1, 0 = off
  constant REG_IRQ_MASK : natural := 7;
  constant REG_IRQ_PEND : natural := 8;  -- write one to clear a bit
  constant REG_PATTERN  : natural := 9;  -- seed of the test pattern
  constant NUM_REGS     : natural := 10;

  -- Interrupt sources, as bits of REG_IRQ_PEND and REG_IRQ_MASK
  constant IRQ_FRAME : natural := 0;
  constant IRQ_STATS : natural := 1;
  constant IRQ_TIMER : natural := 2;

  type reg_array_t is array (0 to NUM_REGS - 1) of byte_t;

  constant REG_RESET : reg_array_t := (
    REG_CTRL => x"00", REG_WIDTH => x"10", REG_HEIGHT => x"04",
    REG_GAIN => x"10", REG_OFFSET => x"00", REG_THRESH => x"80",
    REG_TIMER => x"00", REG_IRQ_MASK => x"00", REG_IRQ_PEND => x"00",
    REG_PATTERN => x"00");

  function crc8_next (crc : byte_t; b : byte_t) return byte_t;
  function sat8 (v : integer) return pixel_t;
end package;

package body pkg_soc is
  function crc8_next (crc : byte_t; b : byte_t) return byte_t is
    variable c  : byte_t := crc;
    variable fb : std_logic;
  begin
    for i in 7 downto 0 loop
      fb := c(7) xor b(i);
      c := c(6 downto 0) & '0';
      if fb = '1' then
        c := c xor x"07";
      end if;
    end loop;
    return c;
  end function;

  function sat8 (v : integer) return pixel_t is
  begin
    if v < 0 then
      return to_unsigned(0, 8);
    elsif v > 255 then
      return to_unsigned(255, 8);
    else
      return to_unsigned(v, 8);
    end if;
  end function;
end package body;
