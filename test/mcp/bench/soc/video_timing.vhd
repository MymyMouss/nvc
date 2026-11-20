-- Video timing generator.
--
-- While enable is high, produces frames of width x height active pixels.
-- Each line is width active clocks followed by H_BLANK blank clocks, and
-- each frame is height active lines followed by V_BLANK blank lines.  width
-- and height are sampled at the start of each frame and hold for the whole
-- frame.  For each active pixel de is high and x, y give its position; sof
-- marks the first pixel of a frame, eol the last pixel of a line and eof
-- the last pixel of the frame.  When enable goes low the generator stops at
-- once, and the next frame starts as soon as enable is high again.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;

entity video_timing is
  generic (H_BLANK : positive := 6; V_BLANK : positive := 1);
  port (clk, rst, enable : in std_logic;
        width, height    : in byte_t;
        de, sof, eol, eof : out std_logic;
        x, y             : out natural range 0 to 255);
end entity;

architecture rtl of video_timing is
  signal running : std_logic;
  signal hc, vc  : natural range 0 to 511;
  signal w, h    : natural range 0 to 255;
begin
  process (clk)
    variable active : boolean;
  begin
    if rising_edge(clk) then
      de  <= '0';
      sof <= '0';
      eol <= '0';
      eof <= '0';
      if rst = '1' or enable = '0' then
        running <= '0';
      elsif running = '0' then
        running <= '1';
        hc <= 0;
        vc <= 0;
        w  <= to_integer(unsigned(width));
        h  <= to_integer(unsigned(height));
      else
        active := hc < w and vc < h;
        if active then
          de <= '1';
          x  <= hc;
          y  <= vc;
          if hc = 0 and vc = 0 then
            sof <= '1';
          end if;
          if hc = w - 1 then
            eol <= '1';
            if vc = h - 1 then
              eof <= '1';
            end if;
          end if;
        end if;
        if hc = w + H_BLANK - 1 then
          hc <= 0;
          if vc = h + V_BLANK - 1 then
            vc <= 0;
            w  <= to_integer(unsigned(width));
            h  <= to_integer(unsigned(height));
          else
            vc <= vc + 1;
          end if;
        else
          hc <= hc + 1;
        end if;
      end if;
    end if;
  end process;
end architecture;
