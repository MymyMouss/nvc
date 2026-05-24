set -xe

pwd
which nvc

nvc -a $TESTDIR/regress/cover31.vhd \
    -e --cover=all \
       --cover-spec=$TESTDIR/regress/data/cover31.spec \
       --cover-file=cover31.ncdb cover31 -r

nvc --cover-report -o html cover31.ncdb 2>&1 | grep -v '^** Debug:' | tee out.txt

diff -u $TESTDIR/regress/gold/cover31.txt out.txt
