-- Three stage arithmetic pipeline.
--
-- Specification: out_data = 2 * in_data + 1 (modulo 256), and it appears
-- exactly 3 clocks after the input.  out_valid is the input in_valid
-- delayed by the same 3 clocks, so it is high exactly when out_data is
-- meaningful.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity pipe is
  port (clk, rst  : in std_logic;
        in_valid  : in std_logic;
        in_data   : in unsigned(7 downto 0);
        out_valid : out std_logic;
        out_data  : out unsigned(7 downto 0));
end entity;

architecture rtl of pipe is
  signal v1, v2, v3 : std_logic;
  signal d1, d2, d3 : unsigned(7 downto 0);
begin
  out_valid <= v3;
  out_data  <= d3;

  process (clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        v1 <= '0'; v2 <= '0'; v3 <= '0';
      else
        v1 <= in_valid;
        v2 <= v1;
        v3 <= v2;
      end if;
      d1 <= in_data;
      d2 <= d1(6 downto 0) & '0';
      d3 <= d2 + 1;
    end if;
  end process;
end architecture;
