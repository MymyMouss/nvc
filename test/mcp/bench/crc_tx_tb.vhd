library ieee;
use ieee.std_logic_1164.all;

entity crc_tx_tb is end entity;

architecture test of crc_tx_tb is
  signal clk, rst, start, tx, busy : std_logic := '0';
  signal data : std_logic_vector(15 downto 0) := (others => '0');
  type word_array is array (natural range <>) of std_logic_vector(15 downto 0);
  constant words : word_array := (x"A5C3", x"1234", x"FFFF");

  function crc8 (w : std_logic_vector(15 downto 0))
    return std_logic_vector is
    variable crc : std_logic_vector(7 downto 0) := x"00";
    variable fb  : std_logic;
  begin
    for i in 15 downto 0 loop
      fb := crc(7) xor w(i);
      crc := crc(6 downto 0) & '0';
      if fb = '1' then crc := crc xor x"07"; end if;
    end loop;
    return crc;
  end function;
begin
  dut : entity work.crc_tx port map (clk, rst, start, data, tx, busy);
  clk <= not clk after 5 ns;

  stim : process
  begin
    rst <= '1';
    wait for 25 ns;
    wait until rising_edge(clk);
    rst <= '0';
    for i in 1 to 10 loop wait until rising_edge(clk); end loop;
    for n in words'range loop
      data  <= words(n);
      start <= '1';
      wait until rising_edge(clk);
      start <= '0';
      wait until busy = '0';
      for i in 1 to 80 loop wait until rising_edge(clk); end loop;
    end loop;
    report "crc_tx_tb finished";
    wait;
  end process;

  --CHECK-BEGIN
  -- Samples each bit in its middle and compares the CRC received
  check : process
    variable got : std_logic_vector(23 downto 0);
  begin
    for n in words'range loop
      wait until busy = '1';
      for i in 1 to 50 loop wait until rising_edge(clk); end loop;
      for b in 23 downto 0 loop
        got(b) := tx;
        for i in 1 to 100 loop wait until rising_edge(clk); end loop;
      end loop;
      assert got(23 downto 8) = words(n)
        report "frame " & integer'image(n) & ": data received wrongly"
        severity failure;
      assert got(7 downto 0) = crc8(words(n))
        report "frame " & integer'image(n) & ": wrong CRC"
        severity failure;
    end loop;
    wait;
  end process;
  --CHECK-END
end architecture;
