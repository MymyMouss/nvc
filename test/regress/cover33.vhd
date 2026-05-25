-- Regression test for the --cover-report SEGV in cov-html.c (see cover33.sh).
entity cover33 is
end entity;

architecture a of cover33 is
  signal x : integer := 0;
begin
  process is
  begin
    x <= 1
         + 2
         + 3
         + 4
         + 5;
    wait;
  end process;
end architecture;
