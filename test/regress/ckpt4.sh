set -xe

pwd
which nvc

# Periodic checkpoints and background confirmation by segments

write_design() {   # increment of the counting process, from 300 ns on
  cat > ckpt4.vhd <<VHDL
entity ckpt4 is end entity;
architecture a of ckpt4 is
  signal clk : bit := '0';
  signal n : integer := 0;
begin
  clk <= not clk after 5 ns;
  count: process (clk)
    variable v : integer := 0;
  begin
    if clk = '1' then
      if now >= 300 ns then v := v + $1; else v := v + 1; end if;
      n <= v;
    end if;
  end process;
end architecture;
VHDL
}

write_design 1
nvc -a ckpt4.vhd -e ckpt4
nvc -r --stop-time=500ns --checkpoint=ck@every=100ns ckpt4
test $(ls ck | wc -l) -eq 5

# The same design confirms
nvc -r --confirm=ck --jobs=2 --confirm-result=ok.json ckpt4 2> ok.err
grep -q '"result":"confirmed"' ok.json
grep -q '"confirmed_up_to_fs":500000000' ok.json

# A second run picks up what is settled and runs nothing
nvc -r --confirm=ck --confirm-result=again.json ckpt4 2> again.err
grep -q '"result":"confirmed"' again.json
test $(grep -o '"resumed":true' again.json | wc -l) -eq 5

# A fix that changes behaviour from 300 ns on: different, first in the
# segment that ends at 400 ns
write_design 2
nvc -a ckpt4.vhd -e ckpt4
if nvc -r --confirm=ck --jobs=2 --confirm-result=diff.json ckpt4 2> diff.err; then
  echo "confirmed a design that differs"
  exit 1
fi
grep -q '"result":"different"' diff.json
grep -q '"confirmed_up_to_fs":300000000' diff.json

# Results settled for a different design are not reused: segments 1 to 3
# were settled for the first design and the second one differs in segment 4
test $(grep -o '"resumed":true' diff.json | wc -l) -eq 0

# A budget too small to finish is reported as incomplete, never confirmed
write_design 1
nvc -a ckpt4.vhd -e ckpt4
if nvc -r --confirm=ck --confirm-max=2 --confirm-result=part.json ckpt4 2> part.err; then
  echo "partial coverage reported as complete"
  exit 1
fi
grep -q '"result":"incomplete"' part.json
grep -q '"confirmed_up_to_fs":200000000' part.json

# A changed design that stops before the time of a checkpoint (here a failed
# assertion at 150 ns) is different, in the segment that should have reached it
write_design 1
sed -i 's/n <= v;/n <= v; assert now < 150 ns report "stop" severity failure;/' ckpt4.vhd
nvc -a ckpt4.vhd -e ckpt4
if nvc -r --confirm=ck --confirm-result=ends.json ckpt4 2> ends.err; then
  echo "confirmed a design that stops early"
  exit 1
fi
grep -q '"result":"different"' ends.json
grep -q '"first_difference":{"segment":2' ends.json
grep -q "ends before the time of the checkpoint" ends.json
