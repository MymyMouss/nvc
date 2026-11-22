-- Round-robin arbiter.
--
-- grant is one-hot (or zero when nothing is requested): the requester
-- chosen is the first one with req high in the order last + 1, last + 2,
-- ..., last (modulo N), where last is the requester most recently taken.
-- take is high in the clock where the user accepts the current grant,
-- which makes that requester the new last.
library ieee;
use ieee.std_logic_1164.all;

entity rr_arbiter is
  generic (N : positive := 3);
  port (clk, rst : in std_logic;
        req      : in std_logic_vector(N - 1 downto 0);
        take     : in std_logic;
        grant    : out std_logic_vector(N - 1 downto 0));
end entity;

architecture rtl of rr_arbiter is
  signal last : natural range 0 to N - 1;
  signal g    : std_logic_vector(N - 1 downto 0);
begin
  grant <= g;

  process (req, last)
    variable k : natural range 0 to N - 1;
  begin
    g <= (others => '0');
    for i in 1 to N loop
      k := (last + i) mod N;
      if req(k) = '1' then
        g(k) <= '1';
        exit;
      end if;
    end loop;
  end process;

  process (clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        last <= N - 1;
      elsif take = '1' then
        for i in 0 to N - 1 loop
          if g(i) = '1' then
            last <= i;
          end if;
        end loop;
      end if;
    end if;
  end process;
end architecture;
