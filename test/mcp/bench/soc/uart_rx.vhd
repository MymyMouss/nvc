-- UART receiver: 8 data bits, no parity, one stop bit.
--
-- A low level on rx starts a frame.  The receiver samples the middle of
-- each bit (CLKS_PER_BIT / 2 clocks after the falling edge for the start
-- bit, then every CLKS_PER_BIT clocks), least significant data bit first.
-- valid is high for one clock with the byte on data after the stop bit,
-- unless the stop bit is low, which sets frame_err for one clock instead.
library ieee;
use ieee.std_logic_1164.all;
use work.pkg_soc.all;

entity uart_rx is
  generic (CLKS_PER_BIT : positive := 8);
  port (clk, rst, rx     : in std_logic;
        data             : out byte_t;
        valid, frame_err : out std_logic);
end entity;

architecture rtl of uart_rx is
  type state_t is (IDLE, START, BITS, STOP);
  signal state : state_t;
  signal cnt   : natural range 0 to CLKS_PER_BIT - 1;
  signal nb    : natural range 0 to 7;
  signal shift : byte_t;
  signal rx_s  : std_logic;
begin
  data <= shift;

  process (clk)
  begin
    if rising_edge(clk) then
      valid     <= '0';
      frame_err <= '0';
      rx_s      <= rx;
      if rst = '1' then
        state <= IDLE;
        rx_s  <= '1';
      else
        case state is
          when IDLE =>
            if rx_s = '0' then
              cnt   <= 0;
              state <= START;
            end if;
          when START =>
            if cnt = CLKS_PER_BIT / 2 - 2 then
              cnt <= 0;
              if rx_s = '0' then
                nb    <= 0;
                state <= BITS;
              else
                state <= IDLE;
              end if;
            else
              cnt <= cnt + 1;
            end if;
          when BITS =>
            if cnt = CLKS_PER_BIT - 1 then
              cnt   <= 0;
              shift <= rx_s & shift(7 downto 1);
              if nb = 7 then
                state <= STOP;
              else
                nb <= nb + 1;
              end if;
            else
              cnt <= cnt + 1;
            end if;
          when STOP =>
            if cnt = CLKS_PER_BIT - 1 then
              if rx_s = '1' then
                valid <= '1';
              else
                frame_err <= '1';
              end if;
              state <= IDLE;
            else
              cnt <= cnt + 1;
            end if;
        end case;
      end if;
    end if;
  end process;
end architecture;
