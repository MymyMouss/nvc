-- Response builder.
--
-- Sends response packets A5 type len payload... crc into the transmit
-- FIFO, one byte per clock, the crc covering type, len and payload (see
-- crc8_next).  Three requesters share it through a round-robin arbiter:
--   0: the command parser (p_req, held until p_ack): RSP_ACK and RSP_NAK
--      carry p_addr as their one byte payload, RSP_DATA carries p_len
--      registers from p_addr (read through raddr/rdata), RSP_STATS the
--      statistics as count high, count low, sum high, sum low;
--   1: interrupt notifications: an irq_event asks for an RSP_IRQ packet
--      whose payload is irq_val (a later event before it is sent only
--      updates the value);
--   2: automatic statistics: with auto_en high, each stats_ready asks for
--      an RSP_STATS packet.
-- A packet is only started when the FIFO has room for the longest packet,
-- MAX_PKT bytes, so that no byte is ever lost: tx_count must be at most
-- FIFO_DEPTH - MAX_PKT.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;

entity rsp_builder is
  generic (FIFO_DEPTH : positive := 32);
  port (clk, rst              : in std_logic;
        p_req                 : in std_logic;
        p_type, p_addr, p_len : in byte_t;
        p_ack                 : out std_logic;
        irq_event             : in std_logic;
        irq_val               : in byte_t;
        auto_en, stats_ready  : in std_logic;
        stat_count, stat_sum  : in unsigned(15 downto 0);
        raddr                 : out natural range 0 to 255;
        rdata                 : in byte_t;
        tx_count              : in unsigned;
        tx_wr                 : out std_logic;
        tx_din                : out byte_t);
end entity;

architecture rtl of rsp_builder is
  constant MAX_PKT : natural := 3 + NUM_REGS + 1;

  type payload_t is array (0 to 3) of byte_t;

  signal busy     : std_logic;
  signal irq_pend : std_logic;
  signal irq_r    : byte_t;
  signal auto_pnd : std_logic;
  signal req      : std_logic_vector(2 downto 0);
  signal grant    : std_logic_vector(2 downto 0);
  signal take     : std_logic;
  signal space_ok : std_logic;
  signal kind     : byte_t;
  signal addr_r   : byte_t;
  signal len_r    : natural range 0 to 255;
  signal pos      : natural range 0 to 511;
  signal snap     : payload_t;
  signal crc      : byte_t;
begin
  req(0) <= p_req;
  req(1) <= irq_pend;
  req(2) <= auto_pnd;

  space_ok <= '1' when to_integer(tx_count) <= FIFO_DEPTH - MAX_PKT
              else '0';
  take     <= '1' when busy = '0' and space_ok = '1' and grant /= "000"
              else '0';

  u_arb : entity work.rr_arbiter
    generic map (N => 3)
    port map (clk => clk, rst => rst, req => req, take => take,
              grant => grant);

  raddr <= to_integer(unsigned(addr_r)) + pos - 3 when pos >= 3 else 0;

  process (clk)
    variable b : byte_t;
  begin
    if rising_edge(clk) then
      p_ack <= '0';
      tx_wr <= '0';

      if irq_event = '1' then
        irq_pend <= '1';
        irq_r    <= irq_val;
      end if;
      if auto_en = '1' and stats_ready = '1' then
        auto_pnd <= '1';
      end if;

      if rst = '1' then
        busy     <= '0';
        irq_pend <= '0';
        auto_pnd <= '0';
      elsif take = '1' then
        busy <= '1';
        pos  <= 0;
        crc  <= x"00";
        snap <= (std_logic_vector(stat_count(15 downto 8)),
                 std_logic_vector(stat_count(7 downto 0)),
                 std_logic_vector(stat_sum(15 downto 8)),
                 std_logic_vector(stat_sum(7 downto 0)));
        if grant(0) = '1' then
          p_ack  <= '1';
          kind   <= p_type;
          addr_r <= p_addr;
          if p_type = RSP_DATA then
            len_r <= to_integer(unsigned(p_len));
          elsif p_type = RSP_STATS then
            len_r <= 4;
          else
            len_r <= 1;
          end if;
        elsif grant(1) = '1' then
          irq_pend <= '0';
          kind     <= RSP_IRQ;
          len_r    <= 1;
          snap(0)  <= irq_r;
        else
          auto_pnd <= '0';
          kind     <= RSP_STATS;
          len_r    <= 4;
        end if;
      elsif busy = '1' then
        if pos = 0 then
          b := SYNC_BYTE;
        elsif pos = 1 then
          b := kind;
        elsif pos = 2 then
          b := std_logic_vector(to_unsigned(len_r, 8));
        elsif pos < 3 + len_r then
          if kind = RSP_DATA then
            b := rdata;
          elsif kind = RSP_STATS then
            b := snap(pos - 3);
          elsif kind = RSP_IRQ then
            b := snap(0);
          else
            b := addr_r;
          end if;
        else
          b := crc;
        end if;

        tx_wr  <= '1';
        tx_din <= b;
        if pos >= 1 and pos < 3 + len_r then
          crc <= crc8_next(crc, b);
        end if;

        if pos = 3 + len_r then
          busy <= '0';
        else
          pos <= pos + 1;
        end if;
      end if;
    end if;
  end process;
end architecture;
