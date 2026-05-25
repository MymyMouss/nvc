-- Regression test for nickg/nvc#1408: a fatal "missing vcode/body" was raised
-- for an overloaded protected-type method (the string overload, mangled "(S)")
-- when reached through the lazy lowering of a package body, as happens with
-- UVVM_UTIL.RAND_PKG.T_RAND.SET_RAND_SEEDS(S) called from t_coverpoint.
package issue1408_pkg is

  type t_pos_vec is array (natural range <>) of positive;

  -- Inner protected type with overloaded set_seeds, one taking a string.
  type t_inner is protected
    procedure set_seeds(constant str : in string);
    procedure set_seeds(constant seed1 : in positive; constant seed2 : in positive);
    procedure set_seeds(constant seeds : in t_pos_vec(0 to 1));
    impure function value return integer;
  end protected;

  -- Outer protected type holding an inner one and calling the string overload
  -- lazily from its own method, mirroring t_coverpoint -> priv_rand_gen.
  type t_outer is protected
    procedure init(constant name : in string);
    impure function value return integer;
  end protected;

end package;

package body issue1408_pkg is

  type t_inner is protected body
    variable priv_s1 : positive := 1;
    variable priv_s2 : positive := 1;

    procedure set_seeds(constant str : in string) is
      variable acc : natural := 0;
    begin
      for i in str'range loop
        acc := acc + character'pos(str(i));
      end loop;
      priv_s1 := (acc mod 97) + 1;
      priv_s2 := (acc mod 89) + 1;
    end procedure;

    procedure set_seeds(constant seed1 : in positive; constant seed2 : in positive) is
    begin
      priv_s1 := seed1;
      priv_s2 := seed2;
    end procedure;

    procedure set_seeds(constant seeds : in t_pos_vec(0 to 1)) is
    begin
      priv_s1 := seeds(0);
      priv_s2 := seeds(1);
    end procedure;

    impure function value return integer is
    begin
      return priv_s1 * 1000 + priv_s2;
    end function;
  end protected body;

  type t_outer is protected body
    variable priv_inner : t_inner;

    procedure init(constant name : in string) is
    begin
      -- The (S) overload reached through two levels of lazy lowering.
      priv_inner.set_seeds(name);
    end procedure;

    impure function value return integer is
    begin
      return priv_inner.value;
    end function;
  end protected body;

end package body;

-------------------------------------------------------------------------------

use work.issue1408_pkg.all;

entity issue1408 is
end entity;

architecture tb of issue1408 is
begin
  process
    variable o : t_outer;
  begin
    o.init("issue1408");
    -- "issue1408" sums to 758 -> s1 = 758 mod 97 + 1 = 80, s2 = 758 mod 89 + 1 = 47
    assert o.value = 80 * 1000 + 47
      report "unexpected value " & integer'image(o.value) severity failure;
    wait;
  end process;
end architecture;
