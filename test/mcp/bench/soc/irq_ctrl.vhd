-- Interrupt controller.
--
-- A pulse on src(i) sets bit i of pending.  A clock with clr non-zero
-- clears exactly the bits of pending that are set in clr; a source pulse in
-- the same clock wins.  irq is high when a pending bit is also set in mask.
-- event pulses for one clock, with the new pending value on event_val,
-- when a source sets a bit that was clear and is set in mask.
library ieee;
use ieee.std_logic_1164.all;
use work.pkg_soc.all;

entity irq_ctrl is
  port (clk, rst  : in std_logic;
        src       : in std_logic_vector(2 downto 0);
        mask, clr : in byte_t;
        pending   : out byte_t;
        irq       : out std_logic;
        event     : out std_logic;
        event_val : out byte_t);
end entity;

architecture rtl of irq_ctrl is
  signal p : byte_t;
begin
  pending <= p;
  irq     <= '1' when (p and mask) /= x"00" else '0';

  process (clk)
    variable s, nxt : byte_t;
  begin
    if rising_edge(clk) then
      event <= '0';
      if rst = '1' then
        p <= x"00";
      else
        s := "00000" & src;
        nxt := (p and not clr) or s;
        if (s and not p and mask) /= x"00" then
          event     <= '1';
          event_val <= nxt;
        end if;
        p <= nxt;
      end if;
    end if;
  end process;
end architecture;
