-- Command packet parser.
--
-- Reads bytes from the receive FIFO (one per clock while it is not empty)
-- and decodes command packets: A5 cmd addr len data... crc (see pkg_soc).
-- Only CMD_WRITE packets carry data, len bytes of it (1 to 16).  The CRC
-- of cmd, addr, len and the data is kept by a crc8_unit, which is cleared
-- at reset, when a response has been accepted by the response builder,
-- and when the watchdog abandons a packet.  So every packet is checked
-- with a CRC that starts from zero.
--
-- A packet with a good CRC is executed: CMD_WRITE writes the registers
-- addr to addr + len - 1, one per clock, then asks for an RSP_ACK
-- response; CMD_READ asks for an RSP_DATA response with len registers from
-- addr; CMD_STATS asks for RSP_STATS.  Any other command gets RSP_NAK with
-- code 2, and a bad CRC RSP_NAK with code 1.  The parser asks for a
-- response by holding rsp_req high until rsp_taken, and reads nothing
-- meanwhile.  If a packet stops arriving for 200 clocks it is abandoned
-- without a response and the parser waits for the next A5.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;

entity cmd_parser is
  port (clk, rst    : in std_logic;
        fifo_dout   : in byte_t;
        fifo_empty  : in std_logic;
        fifo_rd     : out std_logic;
        reg_we      : out std_logic;
        reg_addr    : out natural range 0 to 255;
        reg_wdata   : out byte_t;
        rsp_req     : out std_logic;
        rsp_type    : out byte_t;
        rsp_addr    : out byte_t;
        rsp_len     : out byte_t;
        rsp_taken   : in std_logic);
end entity;

architecture rtl of cmd_parser is
  type state_t is (SYNC, CMD, ADDR, LEN, DATA, CHECK, EXEC, RESPOND);
  type buf_t is array (0 to 15) of byte_t;
  signal state     : state_t;
  signal cmd_r     : byte_t;
  signal addr_r    : byte_t;
  signal len_r     : byte_t;
  signal buf       : buf_t;
  signal idx       : natural range 0 to 15;
  signal take      : std_logic;
  signal receiving : std_logic;
  signal crc_en    : std_logic;
  signal crc_clr   : std_logic;
  signal crc       : byte_t;
  signal timeout   : std_logic;
  signal wd_armed  : std_logic;
begin
  receiving <= '1' when state = SYNC or state = CMD or state = ADDR
               or state = LEN or state = DATA or state = CHECK else '0';
  take      <= receiving and not fifo_empty;
  fifo_rd   <= take;
  wd_armed  <= receiving when state /= SYNC else '0';
  crc_en    <= take when state = CMD or state = ADDR or state = LEN
               or state = DATA else '0';

  u_crc : entity work.crc8_unit
    port map (clk => clk, rst => rst, clr => crc_clr, en => crc_en,
              din => fifo_dout, crc => crc);

  u_wd : entity work.watchdog
    generic map (LIMIT => 200)
    port map (clk => clk, rst => rst, armed => wd_armed, kick => take,
              timeout => timeout);

  process (clk)
  begin
    if rising_edge(clk) then
      reg_we  <= '0';
      crc_clr <= '0';
      if rst = '1' then
        state   <= SYNC;
        rsp_req <= '0';
      elsif timeout = '1' then
        crc_clr <= '1';
        state   <= SYNC;
      else
        case state is
          when SYNC =>
            if take = '1' and fifo_dout = SYNC_BYTE then
              state <= CMD;
            end if;
          when CMD =>
            if take = '1' then
              cmd_r <= fifo_dout;
              state <= ADDR;
            end if;
          when ADDR =>
            if take = '1' then
              addr_r <= fifo_dout;
              state  <= LEN;
            end if;
          when LEN =>
            if take = '1' then
              len_r <= fifo_dout;
              idx   <= 0;
              if cmd_r = CMD_WRITE and unsigned(fifo_dout) /= 0 then
                state <= DATA;
              else
                state <= CHECK;
              end if;
            end if;
          when DATA =>
            if take = '1' then
              buf(idx) <= fifo_dout;
              if idx = to_integer(unsigned(len_r)) - 1 or idx = 15 then
                state <= CHECK;
              else
                idx <= idx + 1;
              end if;
            end if;
          when CHECK =>
            if take = '1' then
              idx <= 0;
              if fifo_dout /= crc then
                rsp_type <= RSP_NAK;
                rsp_addr <= x"01";
                rsp_len  <= x"01";
                state    <= RESPOND;
              elsif cmd_r = CMD_WRITE and unsigned(len_r) /= 0 then
                state <= EXEC;
              elsif cmd_r = CMD_READ then
                rsp_type <= RSP_DATA;
                rsp_addr <= addr_r;
                rsp_len  <= len_r;
                state    <= RESPOND;
              elsif cmd_r = CMD_STATS then
                rsp_type <= RSP_STATS;
                rsp_addr <= addr_r;
                rsp_len  <= x"04";
                state    <= RESPOND;
              else
                rsp_type <= RSP_NAK;
                rsp_addr <= x"02";
                rsp_len  <= x"01";
                state    <= RESPOND;
              end if;
            end if;
          when EXEC =>
            reg_we    <= '1';
            reg_addr  <= to_integer(unsigned(addr_r)) + idx;
            reg_wdata <= buf(idx);
            if idx = to_integer(unsigned(len_r)) - 1 then
              rsp_type <= RSP_ACK;
              rsp_addr <= addr_r;
              rsp_len  <= x"01";
              state    <= RESPOND;
            else
              idx <= idx + 1;
            end if;
          when RESPOND =>
            rsp_req <= '1';
            if rsp_taken = '1' then
              rsp_req <= '0';
              crc_clr <= '1';
              state   <= SYNC;
            end if;
        end case;
      end if;
    end if;
  end process;
end architecture;
