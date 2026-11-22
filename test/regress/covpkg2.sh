set -xe

pwd
which nvc

nvc -a $TESTDIR/regress/covpkg2.vhd \
    -e --cover=all \
       --cover-spec=$TESTDIR/regress/data/covpkg2.spec \
       --cover-file=covpkg2.ncdb covpkg2 -r

nvc --cover-report -o html covpkg2.ncdb 2>&1 | grep -v '^** Debug:' | tee out.txt

diff -u $TESTDIR/regress/gold/covpkg2.txt out.txt
