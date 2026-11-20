library ieee;
use ieee.std_logic_1164.all;

entity uart_rx_tb is end entity;

architecture test of uart_rx_tb is
  signal clk, rst, valid, parity_err : std_logic := '0';
  signal rx : std_logic := '1';
  signal data : std_logic_vector(7 downto 0);
  signal index : natural := 0;
  type byte_array is array (natural range <>) of std_logic_vector(7 downto 0);
  constant bytes : byte_array := (x"55", x"A3", x"0F", x"81", x"3C", x"F0",
                                  x"96", x"C7", x"2A", x"E1");
  -- The frame carrying byte number 5 has a wrong parity bit
  constant bad : natural := 5;
begin
  dut : entity work.uart_rx port map (clk, rst, rx, data, valid, parity_err);
  clk <= not clk after 5 ns;

  stim : process
    procedure send_bit (b : std_logic) is
    begin
      rx <= b;
      for i in 1 to 8 loop wait until rising_edge(clk); end loop;
    end procedure;
    variable ones : natural;
  begin
    rst <= '1';
    wait for 25 ns;
    wait until rising_edge(clk);
    rst <= '0';
    for i in 1 to 20 loop wait until rising_edge(clk); end loop;
    for n in bytes'range loop
      send_bit('0');
      ones := 0;
      for b in 0 to 7 loop
        send_bit(bytes(n)(b));
        if bytes(n)(b) = '1' then ones := ones + 1; end if;
      end loop;
      if (ones mod 2 = 1) = (n /= bad) then
        send_bit('1');
      else
        send_bit('0');
      end if;
      send_bit('1');
      for i in 1 to 12 loop wait until rising_edge(clk); end loop;
    end loop;
    report "uart_rx_tb finished";
    wait;
  end process;

  --CHECK-BEGIN
  check : process (clk)
    variable n : natural := 0;
  begin
    if rising_edge(clk) then
      if valid = '1' then
        assert data = bytes(n)
          report "byte " & integer'image(n) & " received wrongly"
          severity failure;
        assert (parity_err = '1') = (n = bad)
          report "byte " & integer'image(n) & ": parity_err is "
          & std_logic'image(parity_err) severity failure;
        n := n + 1;
      end if;
    end if;
  end process;
  --CHECK-END
end architecture;
