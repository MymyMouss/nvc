library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity pipe_tb is end entity;

architecture test of pipe_tb is
  signal clk, rst, in_valid, out_valid : std_logic := '0';
  signal in_data, out_data : unsigned(7 downto 0) := (others => '0');
begin
  dut : entity work.pipe
    port map (clk, rst, in_valid, in_data, out_valid, out_data);
  clk <= not clk after 5 ns;

  stim : process
  begin
    rst <= '1';
    wait for 25 ns;
    wait until rising_edge(clk);
    rst <= '0';
    for i in 1 to 12 loop
      in_valid <= '1';
      in_data  <= to_unsigned(i * 7, 8);
      wait until rising_edge(clk);
      if i mod 4 = 0 then
        in_valid <= '0';
        wait until rising_edge(clk);
      end if;
    end loop;
    in_valid <= '0';
    wait for 100 ns;
    report "pipe_tb finished";
    wait;
  end process;

  --CHECK-BEGIN
  check : process (clk)
    variable expect : natural := 7;
    variable n      : natural := 1;
  begin
    if rising_edge(clk) then
      if out_valid = '1' then
        assert to_integer(out_data) = (2 * expect + 1) mod 256
          report "wrong result " & integer'image(to_integer(out_data))
          severity failure;
        n := n + 1;
        expect := (n * 7) mod 256;
      end if;
    end if;
  end process;
  --CHECK-END
end architecture;
