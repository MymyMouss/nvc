set -xe

pwd
which nvc

nvc -a $TESTDIR/regress/covpkg3.vhd \
    -e --cover=expression --cover-file=covpkg3.ncdb covpkg3 -r

nvc --cover-report -o html covpkg3.ncdb 2>&1 | grep -v '^** Debug:' | tee out.txt

diff -u $TESTDIR/regress/gold/covpkg3.txt out.txt
