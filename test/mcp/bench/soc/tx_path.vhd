-- Transmit path: sends the bytes of the transmit FIFO on the UART.
--
-- Whenever the FIFO is not empty and the UART transmitter is idle, the
-- oldest byte is read from the FIFO and sent.
library ieee;
use ieee.std_logic_1164.all;
use work.pkg_soc.all;

entity tx_path is
  generic (CLKS_PER_BIT : positive := 8);
  port (clk, rst   : in std_logic;
        fifo_dout  : in byte_t;
        fifo_empty : in std_logic;
        fifo_rd    : out std_logic;
        txd        : out std_logic);
end entity;

architecture rtl of tx_path is
  signal busy, start : std_logic;
begin
  start   <= not fifo_empty and not busy and not rst;
  fifo_rd <= start;

  u_tx : entity work.uart_tx
    generic map (CLKS_PER_BIT => CLKS_PER_BIT)
    port map (clk => clk, rst => rst, start => start, data => fifo_dout,
              tx => txd, busy => busy);
end architecture;
