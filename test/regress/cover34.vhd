
-- Regression for symmetric "count-from-undefined" toggle counting.
--
-- A std_logic signal that idles at an undefined value ('U'/'X') and is pulsed
-- to a defined level and back toggles e.g. 'U' -> '1' -> 'U'. This is the real
-- shape of the UVVM terminate_current_command reset flag: a resolved signal
-- whose driving process has not yet assigned it sits at 'U', because
-- resolve('Z','U') = 'U'.
--
-- count-from-undefined must count BOTH directions: 'U'/'X' -> '1' as 0->1 and
-- '1' -> 'U'/'X' as 1->0 (and symmetrically for 0). Before the fix only the
-- transition *from* the undefined value was counted, so such a pulse recorded
-- just one of the two toggle bins.

library ieee;
use ieee.std_logic_1164.all;

entity cover34 is
end entity;

architecture test of cover34 is

    -- pulsed 'U' -> '1' -> 'U'  (exercises 0->1 and the new 1->U as 1->0)
    signal pulse_u : std_logic := 'U';

    -- pulsed 'X' -> '0' -> 'X'  (exercises 1->0 and the new 0->X as 0->1)
    signal pulse_x : std_logic := 'X';

begin

    process
    begin
        wait for 1 ns;

        pulse_u <= '1';   -- U -> 1
        pulse_x <= '0';   -- X -> 0
        wait for 1 ns;

        pulse_u <= 'U';   -- 1 -> U  (counted as 1 -> 0 by the fix)
        pulse_x <= 'X';   -- 0 -> X  (counted as 0 -> 1 by the fix)
        wait for 1 ns;

        wait;
    end process;

end architecture;
