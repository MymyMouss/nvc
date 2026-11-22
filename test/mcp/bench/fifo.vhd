-- Four word FIFO with valid/ready handshakes on both sides.
--
-- Specification: a word is accepted when valid_in and ready_out are both
-- high at a clock edge; ready_out is high exactly when the FIFO holds
-- fewer than 4 words.  A word is consumed when valid_out and ready_in are
-- both high at a clock edge.  Words come out in the order they went in,
-- each exactly once.  valid_out is high when the FIFO is not empty.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity fifo is
  port (clk, rst   : in std_logic;
        valid_in   : in std_logic;
        ready_out  : out std_logic;
        data_in    : in unsigned(7 downto 0);
        valid_out  : out std_logic;
        ready_in   : in std_logic;
        data_out   : out unsigned(7 downto 0));
end entity;

architecture rtl of fifo is
  type mem_t is array (0 to 3) of unsigned(7 downto 0);
  signal mem   : mem_t;
  signal wr_p  : unsigned(1 downto 0);
  signal rd_p  : unsigned(1 downto 0);
  signal count : unsigned(2 downto 0);
begin
  ready_out <= '1' when count < 4 else '0';
  valid_out <= '1' when count > 0 else '0';
  data_out  <= mem(to_integer(rd_p));

  process (clk)
    variable push, pop : boolean;
  begin
    if rising_edge(clk) then
      if rst = '1' then
        wr_p  <= (others => '0');
        rd_p  <= (others => '0');
        count <= (others => '0');
      else
        push := valid_in = '1' and count < 4;
        pop  := ready_in = '1' and count > 0;
        if push then
          mem(to_integer(wr_p)) <= data_in;
          wr_p <= wr_p + 1;
        end if;
        if pop then
          rd_p <= rd_p + 1;
        end if;
        if push and not pop then
          count <= count + 1;
        elsif pop and not push then
          count <= count - 1;
        end if;
      end if;
    end if;
  end process;
end architecture;
