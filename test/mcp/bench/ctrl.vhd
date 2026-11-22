-- Transfer controller.
--
-- Specification: when go is pulsed in state IDLE the controller goes
-- IDLE -> LOAD -> SEND -> WAIT_ACK -> FINISH -> IDLE, one state per clock.
-- It stays in WAIT_ACK until the peer has pulsed ack.  An ack pulse that
-- arrives during SEND or WAIT_ACK must be remembered in ack_seen.  done is
-- high for one clock in FINISH.  busy is high in LOAD, SEND and WAIT_ACK.
library ieee;
use ieee.std_logic_1164.all;

entity ctrl is
  port (clk, rst, go, ack : in std_logic;
        busy, done        : out std_logic);
end entity;

architecture rtl of ctrl is
  type state_t is (IDLE, LOAD, SEND, WAIT_ACK, FINISH);
  signal state    : state_t;
  signal ack_seen : std_logic;
begin
  busy <= '1' when state = LOAD or state = SEND or state = WAIT_ACK else '0';
  done <= '1' when state = FINISH else '0';

  process (clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        state    <= IDLE;
        ack_seen <= '0';
      else
        case state is
          when IDLE =>
            ack_seen <= '0';
            if go = '1' then
              state <= LOAD;
            end if;
          when LOAD =>
            state <= SEND;
          when SEND =>
            state <= WAIT_ACK;
            if ack = '1' then
              ack_seen <= '1';
            end if;
          when WAIT_ACK =>
            if ack = '1' then
              ack_seen <= '1';
            end if;
            if ack = '1' or ack_seen = '1' then
              state <= FINISH;
            end if;
          when FINISH =>
            state <= IDLE;
        end case;
      end if;
    end if;
  end process;
end architecture;
