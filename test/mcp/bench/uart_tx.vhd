-- UART transmitter, 8 data bits, even parity, one stop bit.
--
-- Specification: when start is pulsed while idle, the byte in data is sent
-- as: start bit (0), the 8 data bits least significant first, the parity
-- bit, the stop bit (1).  Each bit lasts 4 clocks.  The parity bit makes
-- the number of ones among data and parity even (all 8 data bits count).
-- busy is high from the clock after start until the stop bit has ended.
library ieee;
use ieee.std_logic_1164.all;

entity uart_tx is
  port (clk, rst, start : in std_logic;
        data            : in std_logic_vector(7 downto 0);
        tx, busy        : out std_logic);
end entity;

architecture rtl of uart_tx is
  type state_t is (IDLE, SHIFT);
  signal state  : state_t;
  signal frame  : std_logic_vector(10 downto 0);
  signal nbits  : natural range 0 to 11;
  signal tick   : natural range 0 to 3;
  signal parity : std_logic;
begin
  parity <= data(0) xor data(1) xor data(2) xor data(3)
            xor data(4) xor data(5) xor data(6) xor data(7);
  tx   <= frame(0) when state = SHIFT else '1';
  busy <= '1' when state = SHIFT else '0';

  process (clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        state <= IDLE;
        nbits <= 0;
        tick  <= 0;
      else
        case state is
          when IDLE =>
            if start = '1' then
              frame <= '1' & parity & data & '0';
              nbits <= 11;
              tick  <= 0;
              state <= SHIFT;
            end if;
          when SHIFT =>
            if tick = 3 then
              tick <= 0;
              frame <= '1' & frame(10 downto 1);
              if nbits = 1 then
                state <= IDLE;
              end if;
              nbits <= nbits - 1;
            else
              tick <= tick + 1;
            end if;
        end case;
      end if;
    end if;
  end process;
end architecture;
