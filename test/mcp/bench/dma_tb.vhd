library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity dma_tb is end entity;

architecture test of dma_tb is
  type mem_t is array (0 to 127) of unsigned(7 downto 0);
  signal clk, rst, start, rd_en, wr_en, done, clear : std_logic := '0';
  signal len     : unsigned(6 downto 0) := (others => '0');
  signal rd_addr, wr_addr : unsigned(6 downto 0);
  signal rd_data, wr_data : unsigned(7 downto 0);
  signal src, dst : mem_t;
begin
  dut : entity work.dma
    port map (clk, rst, start, len, rd_addr, rd_en, rd_data, wr_addr, wr_en,
              wr_data, done);
  clk <= not clk after 5 ns;

  rd_data <= src(to_integer(rd_addr));

  memory : process (clk)
  begin
    if rising_edge(clk) then
      if clear = '1' then
        dst <= (others => x"EE");
      elsif wr_en = '1' then
        dst(to_integer(wr_addr)) <= wr_data;
      end if;
    end if;
  end process;

  stim : process
    type len_array is array (natural range <>) of natural;
    constant lens : len_array := (3, 5, 7, 10, 4, 16, 9, 12);
  begin
    for i in src'range loop
      src(i) <= to_unsigned((i * 3 + 1) mod 256, 8);
    end loop;
    rst <= '1';
    wait for 25 ns;
    wait until rising_edge(clk);
    rst <= '0';
    for t in lens'range loop
      clear <= '1';
      wait until rising_edge(clk);
      clear <= '0';
      wait until rising_edge(clk);
      len   <= to_unsigned(lens(t), 7);
      start <= '1';
      wait until rising_edge(clk);
      start <= '0';
      for c in 1 to 200 loop
        exit when done = '1';
        wait until rising_edge(clk);
      end loop;
      assert done = '1' report "transfer " & integer'image(t) & " never finished"
        severity failure;
      wait until rising_edge(clk);
      for i in 0 to lens(t) - 1 loop
        assert dst(i) = src(i)
          report "transfer " & integer'image(t) & ": word " & integer'image(i)
          & " is wrong" severity failure;
      end loop;
      assert dst(lens(t)) = x"EE"
        report "transfer " & integer'image(t) & " wrote past its end"
        severity failure;
      for c in 1 to 5 loop
        wait until rising_edge(clk);
      end loop;
    end loop;
    report "dma_tb finished";
    wait;
  end process;
end architecture;
