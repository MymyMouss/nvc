library ieee;
use ieee.std_logic_1164.all;

entity cover33 is
end entity;

architecture test of cover33 is
    signal sig : std_logic_vector(1 downto 0) := "00";
begin

    process is
        variable v : integer := 0;
    begin
        for i in 0 to 3 loop
            if i = 0 then
                v := v + 1;
            elsif i = 1 then            -- elsif header: branch but no statement
                v := v + 2;
            else
                v := v + 3;
            end if;

            case i is
                when 0 =>               -- choice header: branch but no statement
                    v := v + 10;
                when 1 | 2 =>
                    v := v + 20;
                when others =>
                    v := v + 30;
            end case;
        end loop;

        sig <= "11";
        wait;
    end process;

end architecture;
