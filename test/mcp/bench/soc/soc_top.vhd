-- Top level of the SoC.
--
-- A UART (8 clocks per bit) carries command packets in and response
-- packets out (see pkg_soc).  Registers configure a video pipeline that
-- produces a test pattern, filters it, applies a gain and an offset and
-- computes statistics.  The video output is vid_*.  Interrupts come from
-- the end of each frame (IRQ_FRAME), new statistics (IRQ_STATS) and a
-- timer (IRQ_TIMER).  REG_CTRL bit 0 enables the video, bit 1 the filter,
-- bit 2 the statistics and bit 3 automatic RSP_STATS reports.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;

entity soc_top is
  port (clk, rst  : in std_logic;
        uart_rxd  : in std_logic;
        uart_txd  : out std_logic;
        vid_valid : out std_logic;
        vid_pix   : out pixel_t;
        vid_sof   : out std_logic;
        vid_eol   : out std_logic;
        vid_eof   : out std_logic;
        irq       : out std_logic);
end entity;

architecture rtl of soc_top is
  signal regs                       : reg_array_t;
  signal reg_we                     : std_logic;
  signal reg_addr, raddr            : natural range 0 to 255;
  signal reg_wdata, rdata           : byte_t;
  signal pend_clr, pending, ev_val  : byte_t;
  signal irq_src                    : std_logic_vector(2 downto 0);
  signal ev                         : std_logic;
  signal rsp_req, rsp_ack           : std_logic;
  signal rsp_type, rsp_addr, rsp_len : byte_t;
  signal tx_wr, tx_rd, tx_empty     : std_logic;
  signal tx_full                    : std_logic;
  signal tx_din, tx_dout            : std_logic_vector(7 downto 0);
  signal tx_count                   : unsigned(5 downto 0);
  signal de, sof, eol, eof          : std_logic;
  signal x, y                       : natural range 0 to 255;
  signal stat_count, stat_sum       : unsigned(15 downto 0);
  signal stats_ready, frame_done    : std_logic;
  signal timer_tick                 : std_logic;
  signal ctrl                       : byte_t;
begin
  ctrl <= regs(REG_CTRL);

  u_ctrl : entity work.ctrl_path
    port map (clk => clk, rst => rst, rxd => uart_rxd, reg_we => reg_we,
              reg_addr => reg_addr, reg_wdata => reg_wdata,
              rsp_req => rsp_req, rsp_type => rsp_type,
              rsp_addr => rsp_addr, rsp_len => rsp_len, rsp_ack => rsp_ack);

  u_regs : entity work.reg_bank
    port map (clk => clk, rst => rst, we => reg_we, waddr => reg_addr,
              wdata => reg_wdata, raddr => raddr, rdata => rdata,
              irq_pending => pending, pend_clr => pend_clr, regs => regs);

  irq_src <= (IRQ_TIMER => timer_tick, IRQ_STATS => stats_ready,
              IRQ_FRAME => frame_done);

  u_irq : entity work.irq_ctrl
    port map (clk => clk, rst => rst, src => irq_src,
              mask => regs(REG_IRQ_MASK), clr => pend_clr,
              pending => pending, irq => irq, event => ev,
              event_val => ev_val);

  u_timer : entity work.timer
    port map (clk => clk, rst => rst, period => regs(REG_TIMER),
              tick => timer_tick);

  u_timing : entity work.video_timing
    generic map (H_BLANK => 6, V_BLANK => 1)
    port map (clk => clk, rst => rst, enable => ctrl(0),
              width => regs(REG_WIDTH), height => regs(REG_HEIGHT),
              de => de, sof => sof, eol => eol, eof => eof, x => x, y => y);

  u_video : entity work.video_pipe
    port map (clk => clk, rst => rst, video_en => ctrl(0),
              filter_en => ctrl(1), stats_en => ctrl(2),
              seed => regs(REG_PATTERN), gain => regs(REG_GAIN),
              offset => regs(REG_OFFSET), thresh => regs(REG_THRESH),
              de => de, sof => sof, eol => eol, eof => eof, x => x, y => y,
              o_valid => vid_valid, o_pix => vid_pix, o_sof => vid_sof,
              o_eol => vid_eol, o_eof => vid_eof, stat_count => stat_count,
              stat_sum => stat_sum, stats_ready => stats_ready,
              frame_done => frame_done);

  u_rsp : entity work.rsp_builder
    generic map (FIFO_DEPTH => 32)
    port map (clk => clk, rst => rst, p_req => rsp_req, p_type => rsp_type,
              p_addr => rsp_addr, p_len => rsp_len, p_ack => rsp_ack,
              irq_event => ev, irq_val => ev_val, auto_en => ctrl(3),
              stats_ready => stats_ready, stat_count => stat_count,
              stat_sum => stat_sum, raddr => raddr, rdata => rdata,
              tx_count => tx_count, tx_wr => tx_wr, tx_din => tx_din);

  u_txfifo : entity work.sync_fifo
    generic map (WIDTH => 8, DEPTH_LOG2 => 5)
    port map (clk => clk, rst => rst, wr => tx_wr, rd => tx_rd,
              din => tx_din, dout => tx_dout, full => tx_full,
              empty => tx_empty, count => tx_count);

  u_txpath : entity work.tx_path
    port map (clk => clk, rst => rst, fifo_dout => tx_dout,
              fifo_empty => tx_empty, fifo_rd => tx_rd, txd => uart_txd);
end architecture;
