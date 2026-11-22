-- Round-robin arbiter for three requesters.
--
-- Specification: grant(i) is high for the requester being served.  A grant
-- is held for as long as that requester keeps its request high; the clock
-- after the request drops the grant is released.  While no grant is held, a
-- requester is picked among those requesting, in the order last+1, last+2,
-- last (indices modulo 3), where last is the requester served most
-- recently, so a requester that has just been served comes last.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity arb is
  port (clk, rst : in std_logic;
        req      : in std_logic_vector(2 downto 0);
        grant    : out std_logic_vector(2 downto 0));
end entity;

architecture rtl of arb is
  signal g    : std_logic_vector(2 downto 0);
  signal last : natural range 0 to 2;
begin
  grant <= g;

  process (clk)
    variable pick : natural range 0 to 2;
  begin
    if rising_edge(clk) then
      if rst = '1' then
        g    <= "000";
        last <= 2;
      elsif g /= "000" then
        if (g and req) = "000" then
          g <= "000";
        end if;
      else
        for k in 1 to 3 loop
          pick := (last + k) mod 3;
          if req(pick) = '1' then
            g <= "000";
            g(pick) <= '1';
            last <= pick;
            exit;
          end if;
        end loop;
      end if;
    end if;
  end process;
end architecture;
