set -xe

pwd
which nvc

# Work on a local copy so the source can be truncated after the coverage
# database has been recorded.
cp $TESTDIR/regress/covpkg4.vhd .

nvc -a covpkg4.vhd \
    -e --cover=statement,branch,expression --cover-file=covpkg4.ncdb covpkg4 -r

# Shorten the source so a recorded multi-line span reaches beyond the number
# of lines read back at report time: the signal assignment starts at line 10
# (still in range) but its span ends at line 14 (now out of range). This used
# to overrun f->lines and SIGSEGV in cover_print_code_loc / cover_print_expr;
# the renderer must now clamp the span and report cleanly.
head -11 covpkg4.vhd > covpkg4.trunc && mv covpkg4.trunc covpkg4.vhd

nvc --cover-report -o html covpkg4.ncdb 2>&1 | grep -v '^** Debug:' | tee out.txt

diff -u $TESTDIR/regress/gold/covpkg4.txt out.txt
