-- Regression for the package expression-coverage crash with overloaded
-- subprograms.  Two bodies share the simple name PROCESS_WORD; without
-- distinct cover scopes their per-expression cover items collapse into one
-- scope and the item index used during lowering desyncs from the creation
-- order, which made a unary "not" fetch a binary cover item and abort in
-- lower_expr_coverge (emit_not on an invalid register).  The logical
-- expressions live in IF conditions inside CASE alternatives so they are
-- instrumented for expression coverage.
package ops_pkg is

    function process_word(sel : integer; a, b : boolean) return integer;
    function process_word(sel : integer; a, b, c : boolean) return integer;

end package;

package body ops_pkg is

    function process_word(sel : integer; a, b : boolean) return integer is
        variable r : integer := 0;
    begin
        case sel is
            when 0 =>
                if a and b then
                    r := 1;
                end if;
            when 1 =>
                if not a then
                    r := 2;
                end if;
            when others =>
                if a or b then
                    r := 3;
                end if;
        end case;
        return r;
    end function;

    function process_word(sel : integer; a, b, c : boolean) return integer is
        variable r : integer := 0;
    begin
        case sel is
            when 0 =>
                if not c then
                    r := 1;
                end if;
            when 1 =>
                if a and (b or c) then
                    r := 2;
                end if;
            when others =>
                if not (a and b) then
                    r := 3;
                end if;
        end case;
        return r;
    end function;

end package body;

-------------------------------------------------------------------------------

use work.ops_pkg.all;

entity cover31 is
end cover31;

architecture test of cover31 is
begin
    process
        variable v : integer;
    begin
        v := process_word(0, true, false);
        v := process_word(1, true, false);
        v := process_word(2, true, false);
        v := process_word(0, true, false, true);
        v := process_word(1, true, false, true);
        v := process_word(2, true, false, true);
        assert v = 3;
        wait;
    end process;
end architecture;
