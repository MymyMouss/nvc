-- UART receiver, 8 data bits, even parity, one stop bit, 8 clocks per bit.
--
-- Specification: the line idles high.  A low level starts a frame; the
-- receiver samples the middle of each bit: 4 clocks after the falling edge
-- for the start bit, then every 8 clocks for the 8 data bits (least
-- significant first), the parity bit and the stop bit.  valid is high for
-- one clock after the stop bit, with the byte on data.  parity_err always
-- describes the frame being received: it is set when the parity bit
-- differs from the even parity of the data bits and cleared when it
-- matches, so a good frame after a bad one reports no error.
library ieee;
use ieee.std_logic_1164.all;

entity uart_rx is
  port (clk, rst, rx : in std_logic;
        data         : out std_logic_vector(7 downto 0);
        valid        : out std_logic;
        parity_err   : out std_logic);
end entity;

architecture rtl of uart_rx is
  type state_t is (IDLE, START, BITS, PARITY, STOP);
  signal state : state_t;
  signal cnt   : natural range 0 to 7;
  signal nb    : natural range 0 to 7;
  signal shift : std_logic_vector(7 downto 0);
begin
  data <= shift;

  process (clk)
    variable even : std_logic;
  begin
    if rising_edge(clk) then
      valid <= '0';
      if rst = '1' then
        state      <= IDLE;
        parity_err <= '0';
      else
        case state is
          when IDLE =>
            if rx = '0' then
              cnt   <= 0;
              state <= START;
            end if;
          when START =>
            if cnt = 3 then
              if rx = '0' then
                cnt   <= 0;
                nb    <= 0;
                state <= BITS;
              else
                state <= IDLE;
              end if;
            else
              cnt <= cnt + 1;
            end if;
          when BITS =>
            if cnt = 7 then
              cnt   <= 0;
              shift <= rx & shift(7 downto 1);
              if nb = 7 then
                state <= PARITY;
              else
                nb <= nb + 1;
              end if;
            else
              cnt <= cnt + 1;
            end if;
          when PARITY =>
            if cnt = 7 then
              cnt  <= 0;
              even := shift(0) xor shift(1) xor shift(2) xor shift(3)
                      xor shift(4) xor shift(5) xor shift(6) xor shift(7);
              if rx /= even then
                parity_err <= '1';
              else
                parity_err <= '0';
              end if;
              state <= STOP;
            else
              cnt <= cnt + 1;
            end if;
          when STOP =>
            if cnt = 7 then
              valid <= '1';
              state <= IDLE;
            else
              cnt <= cnt + 1;
            end if;
        end case;
      end if;
    end if;
  end process;
end architecture;
