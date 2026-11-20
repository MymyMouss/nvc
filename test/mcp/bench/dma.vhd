-- DMA copy engine.
--
-- Specification: when start is pulsed in IDLE the engine copies len words
-- (1 to 64) from the source memory to the destination memory, one word per
-- clock.  Word idx is requested on rd_addr with rd_en high; rd_data is the
-- memory contents in the same clock, and the engine writes it to wr_addr =
-- idx with wr_en high one clock later.  After every 4 words (a burst) it
-- pauses for 2 clocks, except after the last word.  Once the last word has
-- been written, done is high for one clock and the engine returns to IDLE.
-- It never reads or writes beyond word len - 1.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity dma is
  port (clk, rst, start : in std_logic;
        len             : in unsigned(6 downto 0);
        rd_addr         : out unsigned(6 downto 0);
        rd_en           : out std_logic;
        rd_data         : in unsigned(7 downto 0);
        wr_addr         : out unsigned(6 downto 0);
        wr_en           : out std_logic;
        wr_data         : out unsigned(7 downto 0);
        done            : out std_logic);
end entity;

architecture rtl of dma is
  type state_t is (IDLE, RUN, GAP, DRAIN);
  signal state : state_t;
  signal idx   : unsigned(6 downto 0);
  signal bcnt  : unsigned(1 downto 0);
  signal gcnt  : unsigned(1 downto 0);
  signal rd    : std_logic;
begin
  rd_en   <= rd;
  rd_addr <= idx;

  process (clk)
  begin
    if rising_edge(clk) then
      done    <= '0';
      wr_en   <= rd;
      wr_addr <= idx;
      wr_data <= rd_data;
      if rst = '1' then
        state <= IDLE;
        idx   <= (others => '0');
        bcnt  <= (others => '0');
        rd    <= '0';
        wr_en <= '0';
      else
        case state is
          when IDLE =>
            if start = '1' then
              idx   <= (others => '0');
              bcnt  <= (others => '0');
              rd    <= '1';
              state <= RUN;
            end if;
          when RUN =>
            if idx = len - 1 then
              rd    <= '0';
              state <= DRAIN;
            elsif bcnt = 3 then
              bcnt  <= (others => '0');
              idx   <= idx + 1;
              rd    <= '0';
              gcnt  <= (others => '0');
              state <= GAP;
            else
              bcnt <= bcnt + 1;
              idx  <= idx + 1;
            end if;
          when GAP =>
            if gcnt = 1 then
              rd    <= '1';
              state <= RUN;
            else
              gcnt <= gcnt + 1;
            end if;
          when DRAIN =>
            done  <= '1';
            state <= IDLE;
        end case;
      end if;
    end if;
  end process;
end architecture;
