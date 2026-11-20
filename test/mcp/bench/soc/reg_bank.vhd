-- Register bank.
--
-- Holds the registers of pkg_soc, set to REG_RESET at reset.  A write with
-- we high stores wdata at waddr, except for REG_IRQ_PEND: writing it pulses
-- pend_clr with the written value for one clock, asking the interrupt
-- controller to clear those bits (write one to clear), and stores nothing.
-- Writes to addresses beyond the last register are ignored.  rdata is the
-- register at raddr, REG_IRQ_PEND reading irq_pending, and 0 beyond the
-- last register.
library ieee;
use ieee.std_logic_1164.all;
use work.pkg_soc.all;

entity reg_bank is
  port (clk, rst    : in std_logic;
        we          : in std_logic;
        waddr       : in natural range 0 to 255;
        wdata       : in byte_t;
        raddr       : in natural range 0 to 255;
        rdata       : out byte_t;
        irq_pending : in byte_t;
        pend_clr    : out byte_t;
        regs        : out reg_array_t);
end entity;

architecture rtl of reg_bank is
  signal r : reg_array_t;
begin
  regs <= r;

  rdata <= irq_pending when raddr = REG_IRQ_PEND
           else r(raddr) when raddr < NUM_REGS
           else x"00";

  process (clk)
  begin
    if rising_edge(clk) then
      pend_clr <= x"00";
      if rst = '1' then
        r <= REG_RESET;
      elsif we = '1' then
        if waddr = REG_IRQ_PEND then
          pend_clr <= wdata;
        elsif waddr < NUM_REGS then
          r(waddr) <= wdata;
        end if;
      end if;
    end if;
  end process;
end architecture;
