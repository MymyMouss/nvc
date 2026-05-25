set -xe

pwd
which nvc

# Work on a local copy so the source can be truncated after the coverage
# database has been recorded.
cp $TESTDIR/regress/cover33.vhd .

nvc -a cover33.vhd \
    -e --cover=statement,branch,expression --cover-file=cover33.ncdb cover33 -r

# Shorten the source so a recorded multi-line span reaches beyond the number
# of lines read back at report time: the signal assignment starts at line 10
# (still in range) but its span ends at line 14 (now out of range). This used
# to overrun f->lines and SIGSEGV in cover_print_code_loc / cover_print_expr;
# the renderer must now clamp the span and report cleanly.
head -11 cover33.vhd > cover33.trunc && mv cover33.trunc cover33.vhd

nvc --cover-report -o html cover33.ncdb 2>&1 | grep -v '^** Debug:' | tee out.txt

diff -u $TESTDIR/regress/gold/cover33.txt out.txt
