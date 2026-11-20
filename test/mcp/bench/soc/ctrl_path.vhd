-- Control path: UART receiver, receive FIFO and command parser.
--
-- Bytes received on rxd go through a 16 byte FIFO to the command parser.
-- A byte arriving while the FIFO is full is lost.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;

entity ctrl_path is
  generic (CLKS_PER_BIT : positive := 8);
  port (clk, rst  : in std_logic;
        rxd       : in std_logic;
        reg_we    : out std_logic;
        reg_addr  : out natural range 0 to 255;
        reg_wdata : out byte_t;
        rsp_req   : out std_logic;
        rsp_type  : out byte_t;
        rsp_addr  : out byte_t;
        rsp_len   : out byte_t;
        rsp_ack   : in std_logic);
end entity;

architecture rtl of ctrl_path is
  signal rx_data              : byte_t;
  signal rx_valid, rx_ferr    : std_logic;
  signal f_dout               : std_logic_vector(7 downto 0);
  signal f_empty, f_full, f_rd : std_logic;
  signal f_count              : unsigned(4 downto 0);
begin
  u_rx : entity work.uart_rx
    generic map (CLKS_PER_BIT => CLKS_PER_BIT)
    port map (clk => clk, rst => rst, rx => rxd, data => rx_data,
              valid => rx_valid, frame_err => rx_ferr);

  u_fifo : entity work.sync_fifo
    generic map (WIDTH => 8, DEPTH_LOG2 => 4)
    port map (clk => clk, rst => rst, wr => rx_valid, rd => f_rd,
              din => rx_data, dout => f_dout, full => f_full,
              empty => f_empty, count => f_count);

  u_parser : entity work.cmd_parser
    port map (clk => clk, rst => rst, fifo_dout => f_dout,
              fifo_empty => f_empty, fifo_rd => f_rd, reg_we => reg_we,
              reg_addr => reg_addr, reg_wdata => reg_wdata,
              rsp_req => rsp_req, rsp_type => rsp_type,
              rsp_addr => rsp_addr, rsp_len => rsp_len, rsp_taken => rsp_ack);
end architecture;
