set -xe

pwd
which nvc

nvc -a $TESTDIR/regress/cover32.vhd \
    -e --cover=expression --cover-file=cover32.ncdb cover32 -r

nvc --cover-report -o html cover32.ncdb 2>&1 | grep -v '^** Debug:' | tee out.txt

diff -u $TESTDIR/regress/gold/cover32.txt out.txt
