set -xe

pwd
which nvc

# Restoring from the last checkpoint that a confirmation proves to be the
# state of the changed design, for a change in a loop that contains a wait

write_design() {   # divisor in the loop body: 100 changes nothing
  cat > ckpt6.vhd <<VHDL
entity ckpt6 is end entity;
architecture a of ckpt6 is
  signal clk : bit := '0';
  signal d : integer := 0;
begin
  clk <= not clk after 5 ns;
  stim: process
  begin
    wait for 20 ns;
    for i in 1 to 8 loop
      d <= d + i + i / $1;
      wait for 25 ns;
    end loop;
    report "d=" & integer'image(d);
    wait;
  end process;
end architecture;
VHDL
}

write_design 100
nvc -a ckpt6.vhd -e ckpt6
nvc -r --stop-time=300ns --checkpoint=ck@every=20ns ckpt6 2> orig.err
test $(ls ck | wc -l) -eq 15

# A fix in the last iteration of the loop: the checkpoints before it are
# the changed design's own state, and the restore is exact
write_design 8
nvc -a ckpt6.vhd -e ckpt6
nvc -r --stop-time=300ns ckpt6 2> straight.err
grep "d=" straight.err > want.txt
nvc -r --stop-time=300ns --restore-confirmed=ck --verdict=v.json \
    --confirm-result=r.json ckpt6 2> late.err
grep -q '"verdict":"exact"' v.json
grep -q '"basis":"confirmed_by_segments"' v.json
grep -q '"state_verified":true' v.json
grep -q '"first_difference_segment":[0-9]' v.json
grep -q "EXACT BY CONFIRMATION" late.err
grep "d=" late.err > got.txt
diff -u want.txt got.txt

# Segments after the first difference were not run
test $(grep -c "^confirm: segment" late.err) -le 13

# The direct restore into the changed loop is refused (it has a loop
# variable that the new code might not accept)
if nvc -r --stop-time=300ns --restore=ck/ck-00000000000120000000.ckpt \
       --allow-changes ckpt6 2> direct.err; then
  echo "resumed in a loop of changed code"
  exit 1
fi
grep -q "loop variable" direct.err

# Nothing but what the confirmation proved is used: a later design is not
# confirmed by what was settled for this one
write_design 7
nvc -a ckpt6.vhd -e ckpt6
nvc -r --stop-time=300ns --restore-confirmed=ck --confirm-result=r2.json \
    ckpt6 2> stale.err
test $(grep -c '"resumed":true' r2.json) -eq 0

# A progress file edited by hand is not believed
cp ck/confirm-progress.txt progress.good
awk '{ print $1, $2, $3, $4, $5, "0000000000000000" }' progress.good \
    > ck/confirm-progress.txt
nvc -r --stop-time=300ns --restore-confirmed=ck --confirm-result=r3.json \
    ckpt6 2> forged.err
test $(grep -c '"resumed":true' r3.json) -eq 0

# A checkpoint from another run in place of one of the files
write_design 100
nvc -a ckpt6.vhd -e ckpt6
mkdir other
sed -i 's/d + i + i \/ 100/d + i + 1/' ckpt6.vhd
nvc -a ckpt6.vhd -e ckpt6
nvc -r --stop-time=300ns --checkpoint=other@every=20ns ckpt6 2> other.err
cp other/ck-00000000000160000000.ckpt ck/ck-00000000000160000000.ckpt
write_design 8
nvc -a ckpt6.vhd -e ckpt6
nvc -r --stop-time=300ns --restore-confirmed=ck --verdict=v4.json ckpt6 \
    2> swapped.err || true
if [ -f v4.json ]; then
  # restored from before the file that is not part of this run
  python3 - <<'PY'
import json
v = json.load(open("v4.json"))
assert v["confirmation"]["checkpoint_fs"] < 160000000, v
PY
fi

# A limit on the segments run: restored from what is confirmed so far, which
# is still exact, and the verdict says that not everything was checked
write_design 100
nvc -a ckpt6.vhd -e ckpt6
cp -r ck ck3
rm -f ck3/confirm-progress.txt
nvc -r --stop-time=300ns --restore-confirmed=ck3 --confirm-max=3 \
    --verdict=v5.json ckpt6 2> limit.err
grep -q '"segments_confirmed":3,"segments":15' v5.json
grep -q '"first_difference_segment":null' v5.json
nvc -r --stop-time=300ns ckpt6 2> straight3.err
grep "d=" straight3.err > want3.txt
grep "d=" limit.err > got3.txt
diff -u want3.txt got3.txt

# Nothing confirmed: refused, or started from zero if that is asked for
write_design 100
nvc -a ckpt6.vhd -e ckpt6
rm -rf ck2
nvc -r --stop-time=300ns --checkpoint=ck2@every=20ns ckpt6 2> /dev/null
sed -i 's/d <= d + i + i \/ 100/d <= d + i + 3/' ckpt6.vhd
nvc -a ckpt6.vhd -e ckpt6
if nvc -r --stop-time=300ns --restore-confirmed=ck2 ckpt6 2> none.err; then
  echo "restored with nothing confirmed"
  exit 1
fi
grep -q "is confirmed" none.err
nvc -r --stop-time=300ns ckpt6 2> straight2.err
nvc -r --stop-time=300ns --restore-confirmed=ck2 --confirmed-or-zero \
    ckpt6 2> zero.err
grep "d=" straight2.err > want2.txt
grep "d=" zero.err > got2.txt
diff -u want2.txt got2.txt
# The first segment differed and nothing after it was run
test $(grep -c "^confirm: segment" zero.err) -le 2

# A changed input file: what was settled for the old one is not used
rm -rf in1
mkdir in1
cd in1
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
  echo "$i $((i * 10))"
done > stim.txt
nvc --std=2008 -a $TESTDIR/regress/ckpt1.vhd -e ckpt1
nvc --std=2008 -r --checkpoint=ck@every=100ns ckpt1 2> /dev/null
nvc --std=2008 -r --restore-confirmed=ck --confirm-result=a.json ckpt1 \
    2> /dev/null
nvc --std=2008 -r --restore-confirmed=ck --confirm-result=b.json ckpt1 \
    2> /dev/null
test $(grep -o '"resumed":true' b.json | wc -l) -eq 7
sed -i 's/^3 30/3 31/' stim.txt
if nvc --std=2008 -r --restore-confirmed=ck --confirm-result=c.json ckpt1 \
      2> c.err; then
  echo "restored although the input file changed"
  exit 1
fi
test $(grep -o '"resumed":true' c.json | wc -l) -eq 0
cd ..
rm -rf in1
