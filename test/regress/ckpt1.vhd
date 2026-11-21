-- Used by ckpt1.sh: state saved at one time step and restored in another
-- process must continue exactly as an uninterrupted run
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.textio.all;

entity ckpt1 is
end entity;

architecture test of ckpt1 is
    type rec_t is record
        a : integer;
        b : std_logic_vector(3 downto 0);
    end record;

    type mem_t is array (0 to 3) of integer;

    signal clk    : std_logic := '0';
    signal cnt    : unsigned(7 downto 0) := (others => '0');
    signal r      : rec_t := (0, "0000");
    signal bus_s  : std_logic := 'Z';          -- Resolved, two drivers
    signal delayed : integer := 0;
    signal stim_a, stim_b : integer := 0;
    signal done   : boolean := false;
begin

    clk <= not clk after 5 ns when not done;

    -- Synchronous process with variables
    p1: process (clk) is
        variable acc : integer := 0;
        variable m   : mem_t := (others => 0);
    begin
        if rising_edge(clk) then
            cnt <= cnt + 1;
            acc := acc + to_integer(cnt);
            m(to_integer(cnt) mod 4) := acc;
            r <= (acc, std_logic_vector(cnt(3 downto 0)));
            delayed <= transport acc after 23 ns;
            if cnt mod 16 = 15 then
                report "p1: acc=" & integer'image(acc)
                    & " m=" & integer'image(m(0)) & "," & integer'image(m(3));
            end if;
        end if;
    end process;

    -- Two drivers of a resolved signal
    bus_s <= '1' when cnt(2) = '1' else 'Z';
    bus_s <= '0' when cnt(3) = '1' else 'Z';

    -- A procedure that waits, called repeatedly
    p2: process is
        procedure pause_and_report (n : natural; msg : string) is
            variable local : integer := n;
        begin
            for i in 1 to n loop
                wait for 7 ns;
                local := local + i;
            end loop;
            report msg & " local=" & integer'image(local)
                & " bus=" & std_logic'image(bus_s);
        end procedure;
    begin
        pause_and_report(3, "first");
        pause_and_report(4, "second");
        wait until cnt = 60;
        pause_and_report(5, "third");
        wait;
    end process;

    -- Reads a stimulus file with TEXTIO and writes a log
    p3: process is
        file f : text open read_mode is "stim.txt";
        file o : text open write_mode is "log.txt";
        variable l, ol : line;
        variable x, y, n : integer := 0;
    begin
        while not endfile(f) loop
            readline(f, l);
            read(l, x);
            read(l, y);
            n := n + 1;
            stim_a <= x;
            stim_b <= y;
            wait for 61 ns;
            write(ol, string'("n=") & integer'image(n) & " sum="
                  & integer'image(stim_a + stim_b) & " cnt="
                  & integer'image(to_integer(cnt)));
            writeline(o, ol);
            report "stim " & integer'image(n) & ": "
                & integer'image(stim_a + stim_b);
        end loop;
        wait for 20 ns;
        done <= true;
        wait;
    end process;

    -- Checks something that depends on 'event and 'last_value
    p4: process (cnt) is
    begin
        if cnt'event and cnt = 100 then
            report "cnt'last_value=" & integer'image(to_integer(cnt'last_value));
        end if;
    end process;

end architecture;
