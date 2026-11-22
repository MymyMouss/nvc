-- Reference model used by the testbench: what the video output should be.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;

package tb_pkg is
  type video_cfg_t is record
    width, height : natural;
    seed          : natural;
    filter        : boolean;
    gain          : natural;
    offset        : integer;
    thresh        : natural;
  end record;

  function pattern (x, y, f, seed : natural) return natural;
  function ref_pixel (x, y, f : natural; cfg : video_cfg_t) return natural;
  function to_s8 (v : natural) return integer;
end package;

package body tb_pkg is
  function pattern (x, y, f, seed : natural) return natural is
  begin
    return (5 * x + 17 * y + 3 * f + seed) mod 256;
  end function;

  function ref_pixel (x, y, f : natural; cfg : video_cfg_t) return natural is
    variable l, c, r, p : natural;
  begin
    c := pattern(x, y, f, cfg.seed);
    if cfg.filter then
      if x = 0 then l := c; else l := pattern(x - 1, y, f, cfg.seed); end if;
      if x = cfg.width - 1 then
        r := c;
      else
        r := pattern(x + 1, y, f, cfg.seed);
      end if;
      p := (l + 2 * c + r) / 4;
    else
      p := c;
    end if;
    return to_integer(sat8(p * cfg.gain / 16 + cfg.offset));
  end function;

  function to_s8 (v : natural) return integer is
  begin
    if v >= 128 then return v - 256; else return v; end if;
  end function;
end package body;
