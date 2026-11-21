package pk is
  constant K : integer := 7;
  type iv is array (natural range <>) of integer;
  function res (v : iv) return integer;
  subtype rint is res integer;
  function f (x : integer) return integer;
  procedure pulse (signal s : out bit; n : in integer);
end package;
package body pk is
  function res (v : iv) return integer is
    variable r : integer := 0;
  begin
    for i in v'range loop r := r + v(i); end loop;
    if r > 1000 then r := r - 1000; end if;
    return r;
  end function;
  function f (x : integer) return integer is
  begin
    if x > 50 then
      return x * 3 + K;
    else
      return x + 1;
    end if;
  end function;
  procedure pulse (signal s : out bit; n : in integer) is
  begin
    for i in 1 to n loop
      s <= '1'; wait for 3 ns; s <= '0'; wait for 4 ns;
    end loop;
  end procedure;
end package body;

use work.pk.all;
entity t is generic (G : integer := 3); end;
architecture a of t is
  signal clk : bit := '0';
  signal cnt : integer := 0;
  signal seed : integer := 11;
  signal shared_s : rint := 0;
  type st_t is (idle, run, err, rec);
  signal st : st_t := idle;
  signal q, p : bit := '0';
  signal comb, fo : integer := 0;
  constant C0 : integer := f(2);
begin
  clk <= not clk after 5 ns;
  comb <= cnt + seed;
  fo <= f(cnt);
  shared_s <= cnt;
  shared_s <= seed * 2;

  fsm: process (clk)
    variable lf : integer := 1;
  begin
    if clk = '1' and clk'event then
      cnt <= cnt + 1;
      lf := (lf * 5 + G) mod 97;
      case st is
        when idle => if cnt = 4 then st <= run; end if;
        when run  => if cnt = 60 then st <= err; end if; q <= not q;
        when err  => seed <= seed + lf; st <= rec;
        when rec  => if lf > 40 then st <= run; seed <= seed - 1; end if;
      end case;
    end if;
  end process;

  main: process
    variable acc : integer := C0;
  begin
    wait for 100 ns;
    acc := acc + cnt;
    report "p1 " & integer'image(acc) & " " & integer'image(shared_s);
    pulse(p, 3);
    wait for 400 ns;
    if cnt > 20 then acc := acc + f(cnt); else acc := acc - 1; end if;
    report "p2 " & integer'image(acc) & " " & integer'image(fo);
    pulse(p, 2);
    wait for 300 ns;
    report "p3 " & integer'image(cnt + acc + comb + seed) & " " & st_t'image(st) & integer'image(shared_s);
    wait;
  end process;

  chk: process (st)
  begin
    if st = rec then
      assert seed < 5 report "recovery with seed " & integer'image(seed) severity note;
    end if;
  end process;
end;
