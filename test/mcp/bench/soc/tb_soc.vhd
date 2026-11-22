-- Testbench of the SoC.
--
-- Drives the UART with command packets and checks every response packet,
-- checks every video pixel against the reference model of tb_pkg, and
-- runs these phases:
--   1. read all registers after reset
--   2. video with stats, no filter
--   3. filter, gain and offset
--   4. negative offset
--   5. interrupts: timer interrupt, then clearing one pending bit
--   6. protocol errors: bad CRC, then a packet cut short
--   7. several reads sent back to back while automatic statistics run
-- Statistics are checked after each video phase against the pixels seen.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pkg_soc.all;
use work.tb_pkg.all;

entity tb_soc is end entity;

architecture test of tb_soc is
  constant BIT_TIME : time := 80 ns;   -- 8 clocks per bit

  signal clk       : std_logic := '0';
  signal rst       : std_logic := '1';
  signal uart_rxd  : std_logic := '1';
  signal uart_txd  : std_logic;
  signal vid_valid : std_logic;
  signal vid_pix   : pixel_t;
  signal vid_sof, vid_eol, vid_eof, irq : std_logic;

  -- Response packets decoded by the monitor
  type payload_t is array (0 to 15) of natural;
  type pkt_t is record
    kind    : natural;
    len     : natural;
    payload : payload_t;
  end record;
  type pkt_array_t is array (0 to 255) of pkt_t;
  signal pkts  : pkt_array_t;
  signal npkts : natural := 0;

  -- Video model state, set by the stimulus before enabling the video
  signal cfg     : video_cfg_t := (16, 4, 0, false, 16, 0, 128);
  signal session : natural := 0;
  signal frames  : natural := 0;   -- frames completed in this session
  signal last_count, last_sum : natural := 0;
begin
  dut : entity work.soc_top
    port map (clk => clk, rst => rst, uart_rxd => uart_rxd,
              uart_txd => uart_txd, vid_valid => vid_valid,
              vid_pix => vid_pix, vid_sof => vid_sof, vid_eol => vid_eol,
              vid_eof => vid_eof, irq => irq);

  clk <= not clk after 5 ns;

  ------------------------------------------------------------------------
  -- Response monitor: decodes bytes on uart_txd into packets

  monitor : process
    variable b, crc : byte_t;
    variable bytes  : payload_t;
    variable state  : natural := 0;
    variable kind, len, pos, n : natural := 0;
  begin
    wait until uart_txd = '0';
    wait for BIT_TIME / 2;
    for i in 0 to 7 loop
      wait for BIT_TIME;
      b(i) := uart_txd;
    end loop;
    wait for BIT_TIME;
    assert uart_txd = '1' report "response byte without a stop bit"
      severity failure;

    case state is
      when 0 =>
        assert b = SYNC_BYTE
          report "response does not start with A5" severity failure;
        crc := x"00";
        state := 1;
      when 1 =>
        kind := to_integer(unsigned(b));
        crc := crc8_next(crc, b);
        state := 2;
      when 2 =>
        len := to_integer(unsigned(b));
        assert len <= 16 report "response too long" severity failure;
        crc := crc8_next(crc, b);
        pos := 0;
        if len = 0 then state := 4; else state := 3; end if;
      when 3 =>
        bytes(pos) := to_integer(unsigned(b));
        crc := crc8_next(crc, b);
        pos := pos + 1;
        if pos = len then state := 4; end if;
      when others =>
        assert b = crc
          report "response packet " & integer'image(n) & " has a bad CRC"
          severity failure;
        pkts(n) <= (kind, len, bytes);
        n := n + 1;
        npkts <= n;
        state := 0;
    end case;
  end process;

  ------------------------------------------------------------------------
  -- Video checker: every pixel against the reference model

  checker : process (clk)
    variable x, y, f, cnt, sum : natural := 0;
    variable cur_session       : natural := 0;
    variable expect            : natural;
  begin
    if rising_edge(clk) then
      if session /= cur_session then
        cur_session := session;
        f := 0;
        frames <= 0;
      end if;
      if vid_valid = '1' then
        if vid_sof = '1' then
          x := 0;
          y := 0;
          cnt := 0;
          sum := 0;
        end if;
        expect := ref_pixel(x, y, f, cfg);
        assert to_integer(vid_pix) = expect
          report "frame " & integer'image(f) & " of session "
          & integer'image(cur_session) & ": pixel (" & integer'image(x)
          & ", " & integer'image(y) & ") is "
          & integer'image(to_integer(vid_pix)) & ", expected "
          & integer'image(expect) severity failure;
        assert (vid_eol = '1') = (x = cfg.width - 1)
          report "end of line mark at the wrong pixel" severity failure;
        assert (vid_eof = '1') = (x = cfg.width - 1 and y = cfg.height - 1)
          report "end of frame mark at the wrong pixel" severity failure;
        if to_integer(vid_pix) > cfg.thresh then
          cnt := cnt + 1;
        end if;
        sum := (sum + to_integer(vid_pix)) mod 65536;
        if vid_eof = '1' then
          f := f + 1;
          frames <= f;
          last_count <= cnt;
          last_sum <= sum;
        elsif vid_eol = '1' then
          x := 0;
          y := y + 1;
        else
          x := x + 1;
        end if;
      end if;
    end if;
  end process;

  ------------------------------------------------------------------------
  -- Stimulus

  stim : process
    type data_t is array (natural range <>) of natural;
    variable seen : natural := 0;
    variable auto : boolean := false;

    procedure send_byte (v : natural) is
      variable b : byte_t := std_logic_vector(to_unsigned(v, 8));
    begin
      uart_rxd <= '0';
      wait for BIT_TIME;
      for i in 0 to 7 loop
        uart_rxd <= b(i);
        wait for BIT_TIME;
      end loop;
      uart_rxd <= '1';
      wait for BIT_TIME;
    end procedure;

    procedure send_packet (cmd, addr : natural; data : data_t;
                           len : natural; bad_crc : boolean := false) is
      variable crc : byte_t := x"00";
      procedure put (v : natural) is
      begin
        crc := crc8_next(crc, std_logic_vector(to_unsigned(v, 8)));
        send_byte(v);
      end procedure;
    begin
      send_byte(16#A5#);
      put(cmd);
      put(addr);
      put(len);
      for i in data'range loop
        put(data(i));
      end loop;
      if bad_crc then
        send_byte(to_integer(unsigned(crc xor x"FF")));
      else
        send_byte(to_integer(unsigned(crc)));
      end if;
    end procedure;

    -- The next response to a command, skipping interrupt notifications
    -- and, while they are on, automatic statistics
    procedure next_response (variable p : out pkt_t) is
    begin
      loop
        while npkts = seen loop
          wait until npkts'event for 200 us;
          assert npkts /= seen report "no response within 200 us"
            severity failure;
        end loop;
        p := pkts(seen);
        seen := seen + 1;
        exit when p.kind /= 16#84# and not (auto and p.kind = 16#83#);
      end loop;
    end procedure;

    procedure write_regs (addr : natural; data : data_t) is
      variable p : pkt_t;
    begin
      send_packet(16#01#, addr, data, data'length);
      next_response(p);
      assert p.kind = 16#81# and p.payload(0) = addr
        report "write to register " & integer'image(addr)
        & " not acknowledged (response type " & integer'image(p.kind) & ")"
        severity failure;
    end procedure;

    procedure write_reg (addr, value : natural) is
    begin
      write_regs(addr, (0 => value));
    end procedure;

    procedure read_check (addr : natural; expect : data_t) is
      variable p : pkt_t;
    begin
      send_packet(16#02#, addr, (1 to 0 => 0), expect'length);
      next_response(p);
      assert p.kind = 16#82# and p.len = expect'length
        report "read of register " & integer'image(addr)
        & " got response type " & integer'image(p.kind) severity failure;
      for i in expect'range loop
        assert p.payload(i - expect'low) = expect(i)
          report "register " & integer'image(addr + i - expect'low)
          & " reads " & integer'image(p.payload(i - expect'low))
          & ", expected " & integer'image(expect(i)) severity failure;
      end loop;
    end procedure;

    procedure check_stats is
      variable p : pkt_t;
    begin
      send_packet(16#03#, 0, (1 to 0 => 0), 0);
      next_response(p);
      assert p.kind = 16#83# report "no statistics response"
        severity failure;
      assert p.payload(0) * 256 + p.payload(1) = last_count
        report "statistics count is "
        & integer'image(p.payload(0) * 256 + p.payload(1))
        & ", the last frame had " & integer'image(last_count)
        & " pixels above the threshold" severity failure;
      assert p.payload(2) * 256 + p.payload(3) = last_sum
        report "statistics sum is "
        & integer'image(p.payload(2) * 256 + p.payload(3))
        & ", the last frame summed to " & integer'image(last_sum)
        severity failure;
    end procedure;

    procedure run_video (c : video_cfg_t; ctrl, nframes : natural) is
    begin
      cfg <= c;
      session <= session + 1;
      write_reg(REG_CTRL, ctrl);
      while frames < nframes loop
        wait until frames'event for 500 us;
        assert frames'event report "video frames stopped" severity failure;
      end loop;
      write_reg(REG_CTRL, 0);
      wait for 2 us;
    end procedure;

    variable p : pkt_t;
    variable c : video_cfg_t;
  begin
    rst <= '1';
    wait for 45 ns;
    rst <= '0';
    wait for 1 us;

    report "phase 1: registers after reset";
    read_check(0, (16#00#, 16#10#, 16#04#, 16#10#, 16#00#, 16#80#, 16#00#,
                   16#00#, 16#00#, 16#00#));

    report "phase 2: video and statistics";
    write_regs(REG_WIDTH, (12, 4));
    write_reg(REG_PATTERN, 3);
    write_reg(REG_THRESH, 60);
    c := (12, 4, 3, false, 16, 0, 60);
    run_video(c, 16#05#, 4);
    check_stats;

    report "phase 3: filter, gain and offset";
    write_regs(REG_GAIN, (16#18#, 5, 100));
    c := (12, 4, 3, true, 16#18#, 5, 100);
    run_video(c, 16#07#, 4);
    check_stats;

    report "phase 4: negative offset";
    write_regs(REG_GAIN, (16#10#, 16#EC#));
    write_regs(REG_WIDTH, (9, 3));
    c := (9, 3, 3, true, 16#10#, -20, 100);
    run_video(c, 16#07#, 3);
    check_stats;

    report "phase 5: interrupts";
    -- The frame and statistics bits are pending from the video phases
    read_check(REG_IRQ_PEND, (0 => 16#03#));
    write_reg(REG_IRQ_MASK, 16#04#);
    write_reg(REG_TIMER, 2);
    loop
      while npkts = seen loop
        wait until npkts'event for 100 us;
        assert npkts /= seen report "no timer interrupt" severity failure;
      end loop;
      p := pkts(seen);
      seen := seen + 1;
      exit when p.kind = 16#84#;
    end loop;
    assert p.payload(0) = 16#07#
      report "interrupt packet says " & integer'image(p.payload(0))
      & ", expected 7" severity failure;
    write_reg(REG_TIMER, 0);
    write_reg(REG_IRQ_PEND, 16#04#);
    read_check(REG_IRQ_PEND, (0 => 16#03#));
    write_reg(REG_IRQ_PEND, 16#03#);
    read_check(REG_IRQ_PEND, (0 => 16#00#));

    report "phase 6: protocol errors";
    send_packet(16#01#, REG_PATTERN, (0 => 7), 1, bad_crc => true);
    next_response(p);
    assert p.kind = 16#8F# and p.payload(0) = 1
      report "a bad CRC was not refused" severity failure;
    -- A write cut short after its first data byte
    send_byte(16#A5#);
    send_byte(16#01#);
    send_byte(REG_PATTERN);
    send_byte(2);
    send_byte(9);
    wait for 5 us;
    read_check(REG_WIDTH, (9, 3));

    report "phase 7: back to back reads with automatic statistics";
    write_regs(REG_WIDTH, (12, 4));
    c := (12, 4, 3, false, 16#10#, -20, 100);
    cfg <= c;
    session <= session + 1;
    auto := true;
    write_reg(REG_CTRL, 16#0D#);
    for i in 1 to 4 loop
      send_packet(16#02#, 0, (1 to 0 => 0), 10);
    end loop;
    for i in 1 to 4 loop
      next_response(p);
      assert p.kind = 16#82# and p.len = 10 and p.payload(1) = 12
        report "back to back read " & integer'image(i) & " got a bad response"
        severity failure;
    end loop;
    write_reg(REG_CTRL, 0);
    auto := false;

    report "tb_soc finished";
    wait for 20 us;
    std.env.finish;
  end process;
end architecture;
