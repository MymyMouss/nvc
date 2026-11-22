set -xe

pwd
which nvc

# Restore into a design that was changed after the checkpoint was taken

write_design() {   # late threshold, increment of the counting process
  cat > ckpt3.vhd <<VHDL
entity ckpt3 is end entity;
architecture a of ckpt3 is
  signal clk : bit := '0';
  signal x : integer := 0;
  signal n : integer := 0;
begin
  clk <= not clk after 5 ns;
  count: process (clk)
    variable v : integer := 0;
  begin
    if clk = '1' then v := v + $2; n <= v; end if;
  end process;
  main: process
  begin
    wait for 100 ns; x <= 1;
    wait for 100 ns;
    report "late: x=" & integer'image(x) & " hit=" & boolean'image(x = $1);
    wait;
  end process;
end architecture;
VHDL
}

write_design 1 1
nvc -a ckpt3.vhd -e ckpt3
nvc -r --stop-time=100ns --checkpoint=state.ckpt@100ns ckpt3 2> before.err

# Without the option a changed design is refused and the message says how
write_design 2 1
nvc -a ckpt3.vhd -e ckpt3
if nvc -r --restore=state.ckpt ckpt3 2> refused.err; then
  echo "restored into a changed design without --allow-changes"
  exit 1
fi
grep -q "allow-changes" refused.err

# The fix is in code that had not run: exact, and what follows is what
# the fixed design does from time zero
nvc -r --stop-time=300ns --restore=state.ckpt --allow-changes --verdict=v.json ckpt3 \
    2> after.err
grep -q '"verdict":"exact"' v.json
grep -q "restore verdict: EXACT" after.err
nvc -r --stop-time=300ns ckpt3 2> straight.err
grep "late: x=" straight.err > want.txt
grep "late: x=" after.err > got.txt
diff -u want.txt got.txt

# The same fix as 'late' but the changed unit had run: approximate, and
# the list names it
write_design 2 2
nvc -a ckpt3.vhd -e ckpt3
nvc -r --stop-time=300ns --restore=state.ckpt --allow-changes --verdict=v2.json ckpt3 \
    2> after2.err
grep -q '"verdict":"approximate"' v2.json
grep -q '"executed_before_checkpoint":true' v2.json
grep -q "APPROXIMATE" after2.err

# A process suspended in changed code that had run is resumed at the wait
# with the same number, and the verdict says so
write_design 1 1
sed -i 's/wait for 100 ns; x <= 1;/wait for 100 ns; x <= 7;/' ckpt3.vhd
nvc -a ckpt3.vhd -e ckpt3
nvc -r --stop-time=300ns --restore=state.ckpt --allow-changes --verdict=v3.json \
    ckpt3 2> after3.err
grep -q '"verdict":"approximate"' v3.json
grep -q '"resumed_by_number":\["process' v3.json
grep -q "right only if the change does not add, remove or reorder waits" v3.json

# It is refused where the new code has no wait with the saved number
write_design 1 1
sed -i 's/wait for 100 ns; x <= 1;/wait for 100 ns; x <= 1; wait for 1 ns;/' ckpt3.vhd
nvc -a ckpt3.vhd -e ckpt3
if nvc -r --stop-time=300ns --restore=state.ckpt --allow-changes ckpt3 \
     2> refused2.err; then
  echo "resumed a process at a wait that does not exist"
  exit 1
fi
grep -q "number of waits\|no wait with that number" refused2.err

# A different build is never accepted, and nor is a damaged file
head -c 100 state.ckpt > short.ckpt
if nvc -r --restore=short.ckpt --allow-changes ckpt3 2> short.err; then
  exit 1
fi
