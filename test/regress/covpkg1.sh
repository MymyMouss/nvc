set -xe

pwd
which nvc

nvc --work=OTHER_LIB:other_lib -a $TESTDIR/regress/covpkg1.vhd

nvc --map=OTHER_LIB:other_lib \
    -a $TESTDIR/regress/covpkg1.vhd \
    -e --cover=all \
       --cover-spec=$TESTDIR/regress/data/covpkg1.spec \
       --cover-file=covpkg1.ncdb covpkg1 -r

nvc --cover-report -o html covpkg1.ncdb 2>&1 | grep -v '^** Debug:' | tee out.txt

diff -u $TESTDIR/regress/gold/covpkg1.txt out.txt
