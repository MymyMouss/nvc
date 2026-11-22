set -xe

pwd
which nvc

# Checkpoints do not depend on waveform dumping, and a fix inside a process
# that is suspended at a wait can be confirmed

write_design() {   # the first increment of the sequential process
  cat > ckpt5.vhd <<VHDL
entity ckpt5 is end entity;
architecture a of ckpt5 is
  signal clk : bit := '0';
  signal d : integer := 0;
  signal log : integer := 0;
begin
  clk <= not clk after 5 ns;
  stim: process
  begin
    wait for 100 ns;
    d <= $1;
    wait for 100 ns;
    d <= d + 1;
    wait for 100 ns;
    d <= d + $2;
    wait for 100 ns;
    report "d=" & integer'image(d);
    wait;
  end process;
  mon: process (clk) begin
    if clk = '1' then log <= log + d; end if;
  end process;
end architecture;
VHDL
}

write_design "d + 1" 1
nvc -a ckpt5.vhd -e ckpt5

# The same run without a dumper, with an FST file and with a VCD file
nvc -r --stop-time=450ns --checkpoint=plain@every=50ns ckpt5
nvc -r --stop-time=450ns --wave=w.fst --checkpoint=fst@every=50ns ckpt5
nvc -r --stop-time=450ns --wave=w.vcd --format=vcd --checkpoint=vcd@every=50ns \
    ckpt5
test $(ls plain | wc -l) -eq 9
for f in plain/*.ckpt; do
  cmp $f fst/$(basename $f)
  cmp $f vcd/$(basename $f)
done

# The checkpoints of a run that recorded waveforms confirm a design that is
# the same, and one with the first increment written the other way round
nvc -r --confirm=fst --confirm-result=same.json ckpt5 2> same.err
grep -q '"result":"confirmed"' same.json

write_design "1 + d" 1
nvc -a ckpt5.vhd -e ckpt5
nvc -r --confirm=fst --jobs=2 --confirm-result=neutral.json ckpt5 2> neutral.err
grep -q '"result":"confirmed"' neutral.json
grep -q '"confirmed_up_to_fs":450000000' neutral.json

# A change in code that has already run and changes the state is found in
# the segment that has it, and one after the current wait too
write_design "d + 2" 1
nvc -a ckpt5.vhd -e ckpt5
if nvc -r --confirm=fst --confirm-result=ran.json ckpt5 2> ran.err; then
  exit 1
fi
grep -q '"result":"different"' ran.json
grep -q '"first_difference":{"segment":2' ran.json

write_design "d + 1" 5
nvc -a ckpt5.vhd -e ckpt5
if nvc -r --confirm=fst --confirm-result=later.json ckpt5 2> later.err; then
  exit 1
fi
grep -q '"result":"different"' later.json
grep -q '"first_difference":{"segment":6' later.json
