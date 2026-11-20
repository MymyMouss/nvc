library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity fifo_tb is end entity;

architecture test of fifo_tb is
  signal clk, rst : std_logic := '0';
  signal valid_in, ready_out, valid_out, ready_in : std_logic := '0';
  signal data_in, data_out : unsigned(7 downto 0) := (others => '0');
  signal finished : boolean := false;
begin
  dut : entity work.fifo
    port map (clk, rst, valid_in, ready_out, data_in, valid_out, ready_in,
              data_out);
  clk <= not clk after 5 ns;

  -- Producer: sends 20, 21, ... as fast as ready allows
  producer : process
    variable n : natural := 20;
  begin
    rst <= '1';
    wait for 25 ns;
    wait until rising_edge(clk);
    rst <= '0';
    for i in 1 to 14 loop
      valid_in <= '1';
      data_in  <= to_unsigned(n, 8);
      wait until rising_edge(clk);
      while ready_out = '0' loop
        wait until rising_edge(clk);
      end loop;
      n := n + 1;
      if i = 8 then
        -- A pause long enough for the consumer to empty the FIFO
        valid_in <= '0';
        wait for 200 ns;
        wait until rising_edge(clk);
      end if;
    end loop;
    valid_in <= '0';
    wait;
  end process;

  -- Slow consumer: takes a word every third clock and checks the order
  consumer : process
    variable expect : natural := 20;
    variable got    : natural := 0;
  begin
    wait for 100 ns;
    loop
      ready_in <= '1';
      wait until rising_edge(clk);
      if valid_out = '1' then
        assert to_integer(data_out) = expect
          report "unexpected word " & integer'image(to_integer(data_out))
          & ", expected " & integer'image(expect) severity failure;
        expect := expect + 1;
        got := got + 1;
      end if;
      ready_in <= '0';
      wait until rising_edge(clk);
      wait until rising_edge(clk);
      exit when got = 14;
    end loop;
    report "fifo_tb finished";
    finished <= true;
    wait;
  end process;

  watchdog : process
  begin
    wait for 3 us;
    assert finished report "not every word came out" severity failure;
    wait;
  end process;
end architecture;
