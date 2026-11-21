set -xe

pwd
which nvc

# A stimulus file for the design to read
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
  echo "$i $((i * 10))"
done > stim.txt

nvc --std=2008 -a $TESTDIR/regress/ckpt1.vhd -e ckpt1

# Straight run
nvc --std=2008 -r ckpt1 > straight.out 2> straight.err
cp log.txt straight.log
test -s straight.log

# Run to a checkpoint and stop there, restore it in a new process and run
# to the end.  The first time is while the process p2 waits for a signal,
# the second while it is suspended inside a procedure.
check_at() {
  at=$1
  nvc --std=2008 -r --stop-time=$at --checkpoint=state.ckpt@$at ckpt1 \
      > before.out 2> before.err
  test -s state.ckpt
  grep -q "checkpoint state.ckpt written" before.err

  # The log file is recreated from the checkpoint
  rm -f log.txt
  nvc --std=2008 -r --restore=state.ckpt ckpt1 > after.out 2> after.err
  grep -q "restored checkpoint state.ckpt" after.err

  # What was printed before plus what was printed after must be what an
  # uninterrupted run printed
  grep -v "checkpoint state.ckpt" before.err > before.log
  grep -v "restored checkpoint" after.err > after.log
  cat before.log after.log > joined.err
  diff -u straight.err joined.err
  cat before.out after.out > joined.out
  diff -u straight.out joined.out
  diff -u straight.log log.txt
  rm -f state.ckpt
}

check_at 300ns
check_at 610ns
