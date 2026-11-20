-- UART transmitter: 8 data bits, no parity, one stop bit.
--
-- When start is high while busy is low, the byte on data is sent: a start
-- bit, the data bits least significant first, a stop bit, each lasting
-- CLKS_PER_BIT clocks.  busy is high from the clock after start until the
-- stop bit has ended.
library ieee;
use ieee.std_logic_1164.all;
use work.pkg_soc.all;

entity uart_tx is
  generic (CLKS_PER_BIT : positive := 8);
  port (clk, rst, start : in std_logic;
        data            : in byte_t;
        tx, busy        : out std_logic);
end entity;

architecture rtl of uart_tx is
  signal frame : std_logic_vector(9 downto 0);
  signal nb    : natural range 0 to 9;
  signal cnt   : natural range 0 to CLKS_PER_BIT - 1;
  signal run   : std_logic;
begin
  busy <= run;
  tx   <= frame(0) when run = '1' else '1';

  process (clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        run <= '0';
      elsif run = '0' then
        if start = '1' then
          frame <= '1' & data & '0';
          nb    <= 0;
          cnt   <= 0;
          run   <= '1';
        end if;
      elsif cnt = CLKS_PER_BIT - 1 then
        cnt   <= 0;
        frame <= '1' & frame(9 downto 1);
        if nb = 9 then
          run <= '0';
        else
          nb <= nb + 1;
        end if;
      else
        cnt <= cnt + 1;
      end if;
    end if;
  end process;
end architecture;
