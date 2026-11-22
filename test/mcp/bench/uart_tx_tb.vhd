library ieee;
use ieee.std_logic_1164.all;

entity uart_tx_tb is end entity;

architecture test of uart_tx_tb is
  signal clk, rst, start, tx, busy : std_logic := '0';
  signal data : std_logic_vector(7 downto 0) := (others => '0');
begin
  dut : entity work.uart_tx port map (clk, rst, start, data, tx, busy);
  clk <= not clk after 5 ns;

  stim : process
    type byte_array is array (natural range <>) of std_logic_vector(7 downto 0);
    constant bytes : byte_array := (x"55", x"0F", x"A3", x"3C", x"81");
  begin
    rst <= '1';
    wait for 25 ns;
    rst <= '0';
    wait until rising_edge(clk);
    for i in bytes'range loop
      data  <= bytes(i);
      start <= '1';
      wait until rising_edge(clk);
      start <= '0';
      wait until busy = '0';
      wait until rising_edge(clk);
      wait until rising_edge(clk);
    end loop;
    report "uart_tx_tb finished";
    wait;
  end process;

  -- Receiver: samples the line in the middle of each bit
  rx : process
    type byte_array is array (natural range <>) of std_logic_vector(7 downto 0);
    constant bytes : byte_array := (x"55", x"0F", x"A3", x"3C", x"81");
    variable got  : std_logic_vector(7 downto 0);
    variable ones : natural;
    variable par  : std_logic;
  begin
    for i in bytes'range loop
      wait until tx = '0';
      for t in 1 to 6 loop wait until rising_edge(clk); end loop;
      for b in 0 to 7 loop
        got(b) := tx;
        for t in 1 to 4 loop wait until rising_edge(clk); end loop;
      end loop;
      par := tx;
      ones := 0;
      for b in 0 to 7 loop
        if got(b) = '1' then ones := ones + 1; end if;
      end loop;
      if par = '1' then ones := ones + 1; end if;
      assert got = bytes(i) report "wrong data received" severity failure;
      assert ones mod 2 = 0 report "parity error" severity failure;
      wait until busy = '0';
    end loop;
    wait;
  end process;
end architecture;
