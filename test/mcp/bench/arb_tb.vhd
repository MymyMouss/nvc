library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity arb_tb is end entity;

architecture test of arb_tb is
  signal clk, rst : std_logic := '0';
  signal req, grant : std_logic_vector(2 downto 0) := "000";
begin
  dut : entity work.arb port map (clk, rst, req, grant);
  clk <= not clk after 5 ns;

  rst <= '1', '0' after 25 ns;

  -- Each requester raises its request, holds it for a few clocks once it is
  -- served, then drops it for one clock and asks again
  gen : for i in 0 to 2 generate
    process
      variable waited : natural;
      constant hold : natural := 2 + i;
    begin
      wait for 40 ns;
      loop
        wait until rising_edge(clk);
        req(i) <= '1';
        waited := 0;
        while grant(i) = '0' loop
          wait until rising_edge(clk);
          waited := waited + 1;
          assert waited < 20
            report "requester " & integer'image(i) & " starved for "
            & integer'image(waited) & " clocks" severity failure;
        end loop;
        for k in 1 to hold loop
          wait until rising_edge(clk);
        end loop;
        req(i) <= '0';
      end loop;
    end process;
  end generate;
end architecture;
