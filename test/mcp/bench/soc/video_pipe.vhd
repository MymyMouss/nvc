-- Video pipeline: pattern generator, filter, gain and offset, statistics.
--
-- The pixels of the gain and offset stage are the video output; the
-- statistics are taken on them.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;

entity video_pipe is
  port (clk, rst            : in std_logic;
        video_en            : in std_logic;
        filter_en           : in std_logic;
        stats_en            : in std_logic;
        seed, gain, offset  : in byte_t;
        thresh              : in byte_t;
        de, sof, eol, eof   : in std_logic;
        x, y                : in natural range 0 to 255;
        o_valid             : out std_logic;
        o_pix               : out pixel_t;
        o_sof, o_eol, o_eof : out std_logic;
        stat_count          : out unsigned(15 downto 0);
        stat_sum            : out unsigned(15 downto 0);
        stats_ready         : out std_logic;
        frame_done          : out std_logic);
end entity;

architecture rtl of video_pipe is
  signal p_valid, p_sof, p_eol, p_eof : std_logic;
  signal p_pix                        : pixel_t;
  signal f_valid, f_sof, f_eol, f_eof : std_logic;
  signal f_pix                        : pixel_t;
  signal g_valid, g_sof, g_eol, g_eof : std_logic;
  signal g_pix                        : pixel_t;
begin
  u_pattern : entity work.pattern_gen
    port map (clk => clk, rst => rst, enable => video_en, seed => seed,
              de => de, sof => sof, eol => eol, eof => eof, x => x, y => y,
              o_valid => p_valid, o_pix => p_pix, o_sof => p_sof,
              o_eol => p_eol, o_eof => p_eof);

  u_filter : entity work.filter3
    port map (clk => clk, rst => rst, enable => filter_en,
              i_valid => p_valid, i_pix => p_pix, i_sof => p_sof,
              i_eol => p_eol, i_eof => p_eof, o_valid => f_valid,
              o_pix => f_pix, o_sof => f_sof, o_eol => f_eol,
              o_eof => f_eof);

  u_gain : entity work.gain_offset
    port map (clk => clk, rst => rst, gain => gain, offset => offset,
              i_valid => f_valid, i_pix => f_pix, i_sof => f_sof,
              i_eol => f_eol, i_eof => f_eof, o_valid => g_valid,
              o_pix => g_pix, o_sof => g_sof, o_eol => g_eol,
              o_eof => g_eof);

  u_stats : entity work.threshold_stats
    port map (clk => clk, rst => rst, enable => stats_en, thresh => thresh,
              i_valid => g_valid, i_pix => g_pix, i_sof => g_sof,
              i_eof => g_eof,
              count => stat_count, sum => stat_sum, ready => stats_ready,
              frame_done => frame_done);

  o_valid <= g_valid;
  o_pix   <= g_pix;
  o_sof   <= g_sof;
  o_eol   <= g_eol;
  o_eof   <= g_eof;
end architecture;
