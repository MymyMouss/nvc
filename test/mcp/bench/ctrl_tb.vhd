library ieee;
use ieee.std_logic_1164.all;

entity ctrl_tb is end entity;

architecture test of ctrl_tb is
  signal clk, rst, go, ack, busy, done : std_logic := '0';
  signal acked : boolean := false;
begin
  dut : entity work.ctrl port map (clk, rst, go, ack, busy, done);
  clk <= not clk after 5 ns;

  stim : process
    procedure transfer (ack_delay : natural) is
    begin
      go <= '1';
      wait until rising_edge(clk);
      go <= '0';
      acked <= false;
      for i in 1 to ack_delay loop
        wait until rising_edge(clk);
      end loop;
      ack <= '1';
      acked <= true;
      wait until rising_edge(clk);
      ack <= '0';
      -- done must follow within 6 clocks
      for i in 1 to 6 loop
        exit when done = '1';
        wait until rising_edge(clk);
      end loop;
      assert done = '1' report "transfer did not finish" severity failure;
      wait until rising_edge(clk);
      wait until rising_edge(clk);
    end procedure;
  begin
    rst <= '1';
    wait for 25 ns;
    rst <= '0';
    wait until rising_edge(clk);
    transfer(1);
    transfer(5);
    report "ctrl_tb finished";
    wait;
  end process;

  check : process (clk)
  begin
    if rising_edge(clk) then
      assert not (done = '1' and not acked) report "done before ack" severity failure;
    end if;
  end process;
end architecture;
