set -xe

pwd
which nvc

# Plain toggle: transitions through 'U'/'X' are not counted at all (0/4).
nvc -a $TESTDIR/regress/cover34.vhd -e --cover=toggle cover34 -r
nvc --cover-report -o html cover34.ncdb 2>&1 | grep -v '^** Debug:' | tee out.txt

# count-from-undefined: 'U'/'X' <-> '1'/'0' counts both bins in each direction,
# so a 'U' -> '1' -> 'U' (or 'X' -> '0' -> 'X') pulse closes both toggle bins (4/4).
nvc -a $TESTDIR/regress/cover34.vhd -e --cover=toggle,count-from-undefined cover34 -r
nvc --cover-report -o html cover34.ncdb 2>&1 | grep -v '^** Debug:' | tee -a out.txt

if [ ! -f html/index.html ]; then
  echo "missing coverage report"
  exit 1
fi

diff -u $TESTDIR/regress/gold/cover34.txt out.txt
