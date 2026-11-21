set -xe

pwd
which nvc

cat > ckpt2.vhd <<'VHDL'
entity ckpt2 is
end entity;

architecture test of ckpt2 is
    type int_ptr is access integer;
    type str_ptr is access string;
    signal s : integer := 0;
begin
    process is
        variable p : int_ptr;
        variable q : str_ptr;
    begin
        p := new integer'(42);
        q := new string'("hello");
        wait for 10 ns;
        s <= p.all + q'length;
        wait for 10 ns;
        assert s = 47 report "bad s";
        report "p=" & integer'image(p.all) & " q=" & q.all;
        wait;
    end process;
end architecture;
VHDL

nvc -a ckpt2.vhd -e ckpt2
nvc -r ckpt2 > straight.out 2>&1

nvc -r --stop-time=10ns --checkpoint=ok.ckpt@10ns ckpt2
nvc -r --restore=ok.ckpt ckpt2 > restored.out 2>&1
grep "p=42 q=hello" straight.out
grep "p=42 q=hello" restored.out

# A checkpoint for a time that is never reached is an error
if nvc -r --checkpoint=never.ckpt@1sec ckpt2; then
  echo "missing error for unreached checkpoint"
  exit 1
fi
test ! -f never.ckpt

# Restoring needs the same design: change the constant and elaborate again
sed -i 's/new integer.(42)/new integer'"'"'(43)/' ckpt2.vhd
nvc -a ckpt2.vhd -e ckpt2
if nvc -r --restore=ok.ckpt ckpt2 2> mismatch.err; then
  echo "restored a checkpoint into a different design"
  exit 1
fi
grep -q "cannot restore checkpoint" mismatch.err

# A damaged file is rejected
head -c 100 ok.ckpt > short.ckpt
if nvc -r --restore=short.ckpt ckpt2 2> short.err; then
  echo "restored a truncated checkpoint"
  exit 1
fi
grep -q "cannot restore checkpoint" short.err
if nvc -r --restore=/nonexistent.ckpt ckpt2; then
  exit 1
fi

# Forced signals cannot be saved: the message names the signal
cat > ckpt2b.vhd <<'VHDL'
library ieee;
use ieee.std_logic_1164.all;
entity ckpt2b is end entity;
architecture test of ckpt2b is
    signal x : std_logic := '0';
    signal y : std_logic := '0';
begin
    x <= not x after 5 ns;
    process is
    begin
        wait for 12 ns;
        y <= force '1';
        wait for 100 ns;
        wait;
    end process;
end architecture;
VHDL
nvc --std=2008 -a ckpt2b.vhd -e ckpt2b
if nvc --std=2008 -r --stop-time=40ns --checkpoint=bad.ckpt@30ns ckpt2b 2> bad.err; then
  echo "expected the checkpoint to be refused"
  exit 1
fi
grep -q "cannot write checkpoint" bad.err
grep -q "forcing" bad.err
test ! -f bad.ckpt
