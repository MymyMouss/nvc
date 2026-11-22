-- Synchronous FIFO.
--
-- A word is written when wr is high and the FIFO is not full, and read
-- when rd is high and it is not empty; both can happen in the same clock.
-- dout is the oldest word.  count is the number of words held.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity sync_fifo is
  generic (WIDTH : positive := 8; DEPTH_LOG2 : positive := 4);
  port (clk, rst    : in std_logic;
        wr, rd      : in std_logic;
        din         : in std_logic_vector(WIDTH - 1 downto 0);
        dout        : out std_logic_vector(WIDTH - 1 downto 0);
        full, empty : out std_logic;
        count       : out unsigned(DEPTH_LOG2 downto 0));
end entity;

architecture rtl of sync_fifo is
  constant DEPTH : positive := 2 ** DEPTH_LOG2;
  type mem_t is array (0 to DEPTH - 1) of std_logic_vector(WIDTH - 1 downto 0);
  signal mem    : mem_t;
  signal wp, rp : unsigned(DEPTH_LOG2 - 1 downto 0);
  signal n      : unsigned(DEPTH_LOG2 downto 0);
begin
  full  <= '1' when n = DEPTH else '0';
  empty <= '1' when n = 0 else '0';
  count <= n;
  dout  <= mem(to_integer(rp));

  process (clk)
    variable push, pop : boolean;
  begin
    if rising_edge(clk) then
      if rst = '1' then
        wp <= (others => '0');
        rp <= (others => '0');
        n  <= (others => '0');
      else
        push := wr = '1' and n /= DEPTH;
        pop  := rd = '1' and n /= 0;
        if push then
          mem(to_integer(wp)) <= din;
          wp <= wp + 1;
        end if;
        if pop then
          rp <= rp + 1;
        end if;
        if push and not pop then
          n <= n + 1;
        elsif pop and not push then
          n <= n - 1;
        end if;
      end if;
    end if;
  end process;
end architecture;
