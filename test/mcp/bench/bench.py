#!/usr/bin/env python3
#
# Bug-diagnosis bench for the nvc MCP server.
#
# Each scenario is a small VHDL design with one injected bug.  The ground
# truth is computed, not written by hand: the design is simulated with and
# without the bug and the first signal value of the instance dut that
# differs gives the time and the signal an agent should find.
#
#   python3 bench.py --nvc build/bin/nvc check        # validate the bench
#   python3 bench.py --nvc build/bin/nvc run --model claude-haiku-5-5

import argparse
import asyncio
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# name -> (design file, testbench file, patches on the design, stop time)
# A patch is (old, new) text or a list of them.  A testbench named X_silent
# is X without its checks, so the bug is only visible in the outputs.
BUGS = {
    "ctrl_skip": ("ctrl", "ctrl_tb",
                  ("state <= WAIT_ACK;", "state <= FINISH;"), "5us"),
    "ctrl_stuck": ("ctrl", "ctrl_tb",
                   ("            state <= WAIT_ACK;\n            if ack = '1' then\n"
                    "              ack_seen <= '1';\n            end if;\n",
                    "            state <= WAIT_ACK;\n"), "5us"),
    "fifo_drop": ("fifo", "fifo_tb",
                  [("count < 4 else '0'", "count <= 4 else '0'"),
                   ("valid_in = '1' and count < 4",
                    "valid_in = '1' and count <= 4")], "5us"),
    "fifo_dup": ("fifo", "fifo_tb",
                 ("if pop then\n          rd_p",
                  "if pop and count > 1 then\n          rd_p"), "5us"),
    "uart_parity": ("uart_tx", "uart_tx_tb",
                    ("data(4) xor data(5) xor data(6) xor data(7)",
                     "data(4) xor data(5) xor data(6)"), "5us"),
    "video_line": ("video_sync", "video_sync_tb",
                   ("elsif hcount = 15 then", "elsif hcount = 16 then"), "5us"),
    "pipe_valid": ("pipe", "pipe_tb", ("v2 <= v1;", "v2 <= in_valid;"), "5us"),
    "pipe_silent": ("pipe", "pipe_tb_silent",
                    ("v2 <= v1;", "v2 <= in_valid;"), "5us"),
    # Larger designs with bugs that only show late or through a chain of
    # signals
    "dma_burst_end": ("dma", "dma_tb", (
        "            if idx = len - 1 then\n              rd    <= '0';\n"
        "              state <= DRAIN;\n            elsif bcnt = 3 then\n"
        "              bcnt  <= (others => '0');\n              idx   <= idx + 1;\n"
        "              rd    <= '0';\n              gcnt  <= (others => '0');\n"
        "              state <= GAP;\n            else\n",
        "            if bcnt = 3 then\n"
        "              bcnt  <= (others => '0');\n              idx   <= idx + 1;\n"
        "              rd    <= '0';\n              gcnt  <= (others => '0');\n"
        "              state <= GAP;\n            elsif idx = len - 1 then\n"
        "              rd    <= '0';\n              state <= DRAIN;\n            else\n"),
        "40us"),
    "arb_starve": ("arb", "arb_tb",
                   ("for k in 1 to 3 loop", "for k in 0 to 2 loop"), "3us"),
    "rx_noclear": ("uart_rx", "uart_rx_tb", (
        "              if rx /= even then\n                parity_err <= '1';\n"
        "              else\n                parity_err <= '0';\n              end if;\n",
        "              if rx /= even then\n                parity_err <= '1';\n"
        "              end if;\n"), "15us"),
    "rx_7bit": ("uart_rx", "uart_rx_tb",
                ("xor shift(4) xor shift(5) xor shift(6) xor shift(7);",
                 "xor shift(4) xor shift(5) xor shift(6);"), "15us"),
    "crc_noreset": ("crc_tx", "crc_tx_tb",
                    ("          word <= data;\n          crc  <= (others => '0');\n",
                     "          word <= data;\n"), "100us"),
    "crc_poly": ("crc_tx", "crc_tx_tb",
                 ("& '0') xor x\"07\";", "& '0') xor x\"0F\";"), "100us"),
    "crc_silent": ("crc_tx", "crc_tx_tb_silent",
                   ("          word <= data;\n          crc  <= (others => '0');\n",
                    "          word <= data;\n"), "100us"),
    "timer_carry": ("timer", "timer_tb",
                    ("if lo = 4095 then", "if lo = 4094 then"), "300us"),
    "timer_prescale": ("timer", "timer_tb",
                       ("elsif pre = 3 then", "elsif pre = 2 then"), "300us"),
    "timer_silent": ("timer", "timer_tb_silent",
                     ("if lo = 4095 then", "if lo = 4094 then"), "300us"),
}

PROMPTS = {
    "ctrl_skip": "the controller (instance dut in ctrl_tb)",
    "ctrl_stuck": "the controller (instance dut in ctrl_tb)",
    "fifo_drop": "the FIFO (instance dut in fifo_tb)",
    "fifo_dup": "the FIFO (instance dut in fifo_tb)",
    "uart_parity": "the UART transmitter (instance dut in uart_tx_tb)",
    "video_line": "the sync generator (instance dut in video_sync_tb)",
    "pipe_valid": "the pipeline (instance dut in pipe_tb)",
    "pipe_silent": "the pipeline (instance dut in pipe_tb_silent), whose "
                   "testbench checks nothing",
    "dma_burst_end": "the DMA copy engine (instance dut in dma_tb)",
    "arb_starve": "the round-robin arbiter (instance dut in arb_tb)",
    "rx_noclear": "the UART receiver (instance dut in uart_rx_tb)",
    "rx_7bit": "the UART receiver (instance dut in uart_rx_tb)",
    "crc_noreset": "the serial transmitter with a CRC (instance dut in "
                   "crc_tx_tb)",
    "crc_poly": "the serial transmitter with a CRC (instance dut in "
                "crc_tx_tb)",
    "crc_silent": "the serial transmitter with a CRC (instance dut in "
                  "crc_tx_tb_silent), whose testbench checks only the data "
                  "frames' length",
    "timer_carry": "the tick counter (instance dut in timer_tb)",
    "timer_prescale": "the tick counter (instance dut in timer_tb)",
    "timer_silent": "the tick counter (instance dut in timer_tb_silent), "
                    "whose testbench only reports the final count: "
                    "280 us is 28000 clocks, so 7000 ticks",
}


SOC_DIR = os.path.join(HERE, "soc")
SOC_FILES = ["pkg_soc", "uart_rx", "uart_tx", "sync_fifo", "crc8_unit",
             "watchdog", "cmd_parser", "reg_bank", "irq_ctrl", "timer",
             "video_timing", "pattern_gen", "filter3", "gain_offset",
             "threshold_stats", "rr_arbiter", "rsp_builder", "tx_path",
             "ctrl_path", "video_pipe", "soc_top", "tb_pkg", "tb_soc"]

# name -> (file patched, patches, short description for the prompt)
SOC_BUGS = {
    "soc_rsp_space": ("rsp_builder", [(
        "space_ok <= '1' when to_integer(tx_count) <= FIFO_DEPTH - MAX_PKT",
        "space_ok <= '1' when to_integer(tx_count) <= FIFO_DEPTH - 4")]),
    "soc_crc_abort": ("cmd_parser", [(
        "      elsif timeout = '1' then\n        crc_clr <= '1';\n",
        "      elsif timeout = '1' then\n")]),
    "soc_filter_edge": ("filter3", [(
        "o_pix   <= tap(left, cur, cur, enable);",
        "o_pix   <= tap(left, cur, left, enable);")]),
    "soc_offset_sign": ("gain_offset", [(
        "to_integer(signed(offset))", "to_integer(unsigned(offset))")]),
    "soc_irq_clear": ("irq_ctrl", [(
        "        nxt := (p and not clr) or s;\n",
        "        if clr /= x\"00\" then\n          nxt := s;\n        else\n"
        "          nxt := p or s;\n        end if;\n")]),
    "soc_frame_restart": ("pattern_gen", [(
        "if rst = '1' or enable = '0' then", "if rst = '1' then")]),
    "soc_port_swap": ("soc_top", [(
        "filter_en => ctrl(1), stats_en => ctrl(2),",
        "filter_en => ctrl(2), stats_en => ctrl(1),")],
        # The miswired enables are the cause even though a port that
        # follows a register is not dumped: accept them, at the time the
        # control register changes, shortly before the first wrong pixel
        ["u_video/filter_en", "u_video/stats_en", "u_video/u_filter/enable",
         "u_video/u_stats/enable"]),
    "soc_stats_last": ("threshold_stats", [(
        "            count <= n;\n            sum   <= s;",
        "            count <= acc_n;\n            sum   <= acc_s;")]),
    "soc_size_latch": ("video_timing", [(
        "        running <= '1';\n        hc <= 0;\n        vc <= 0;\n"
        "        w  <= to_integer(unsigned(width));\n"
        "        h  <= to_integer(unsigned(height));\n",
        "        running <= '1';\n        hc <= 0;\n        vc <= 0;\n")]),
    "soc_watchdog": ("cmd_parser", [(
        "generic map (LIMIT => 200)", "generic map (LIMIT => 2000)")]),
}
SOC_STOP = "3ms"


def soc_sources(bug, buggy):
    """(name, text) of every file in compile order"""
    out = []
    target, patches = SOC_BUGS[bug][:2] if bug else (None, [])
    for name in SOC_FILES:
        with open(os.path.join(SOC_DIR, name + ".vhd")) as f:
            text = f.read()
        if buggy and name == target:
            for old, new in patches:
                assert old in text, (bug, old)
                text = text.replace(old, new)
        out.append((name, text))
    return out


def soc_bug_line(bug):
    """File and line numbers of the faulty code in the buggy design"""
    target, patches = SOC_BUGS[bug][:2]
    text = dict(soc_sources(bug, True))[target]
    old, new = patches[0]
    start = text.index(new) if new.strip() else text.index(old)
    first = text[:start].count("\n") + 1
    return target + ".vhd", first, first + max(new.count("\n"), 1)


def read(name):
    with open(os.path.join(HERE, name + ".vhd")) as f:
        return f.read()


def silent_tb(tb):
    base = tb[:-len("_silent")]
    text = read(base)
    text = re.sub(r"  --CHECK-BEGIN.*--CHECK-END\n", "", text, flags=re.S)
    return text.replace("entity %s " % base, "entity %s " % tb) \
               .replace("of %s " % base, "of %s " % tb)


def sources(bug, buggy):
    design, tb, patch, _ = BUGS[bug]
    src = read(design)
    if buggy:
        for old, new in (patch if isinstance(patch, list) else [patch]):
            assert old in src, (bug, old)
            src = src.replace(old, new)
    tbsrc = silent_tb(tb) if tb.endswith("_silent") else read(tb)
    return design, src, tb, tbsrc


def make_workspace(bug, buggy):
    ws = tempfile.mkdtemp(prefix="nvc-bench-")
    os.makedirs(os.path.join(ws, "src"))
    if bug in SOC_BUGS:
        for name, text in soc_sources(bug, buggy):
            with open(os.path.join(ws, "src", name + ".vhd"), "w") as f:
                f.write(text)
        with open(os.path.join(ws, "src", "files.txt"), "w") as f:
            f.write("\n".join("src/%s.vhd" % n for n in SOC_FILES) + "\n")
        return ws, "soc_top", "tb_soc"
    design, src, tb, tbsrc = sources(bug, buggy)
    for name, text in ((design, src), (tb, tbsrc)):
        with open(os.path.join(ws, "src", name + ".vhd"), "w") as f:
            f.write(text)
    return ws, design, tb


def nvc_env(nvc):
    env = dict(os.environ)
    env["NVC_LIBPATH"] = os.path.join(os.path.dirname(nvc), "..", "lib")
    return env


def simulate(nvc, bug, buggy):
    """Run the testbench; return (exit status, stderr text, fst events)."""
    ws, design, tb = make_workspace(bug, buggy)
    try:
        env = nvc_env(nvc)
        fst = os.path.join(ws, "w.fst")
        if bug in SOC_BUGS:
            files = ["src/%s.vhd" % n for n in SOC_FILES]
            stop = SOC_STOP
        else:
            files = ["src/%s.vhd" % design, "src/%s.vhd" % tb]
            stop = BUGS[bug][3]
        r = subprocess.run(
            [nvc, "--std=2008", "-a"] + files +
            ["-e", tb, "-r", "--stop-time=" + stop, "--wave=" + fst],
            cwd=ws, env=env, capture_output=True, text=True)
        events = []
        if os.path.exists(fst):
            dump = os.path.join(os.path.dirname(nvc), "fstdump")
            out = subprocess.run([dump, fst], capture_output=True, text=True)
            for line in out.stdout.splitlines():
                m = re.match(r"#(\d+) (\S+) (.*)", line)
                if m and ".dut." in m.group(2):
                    path = m.group(2).split(".dut.", 1)[1]
                    path = re.sub(r"\[.*\]$", "", path).replace(".", "/")
                    events.append((int(m.group(1)), path, m.group(3)))
        return r.returncode, r.stdout + r.stderr, events
    finally:
        shutil.rmtree(ws, ignore_errors=True)


def truth(nvc, bug):
    """First (time_fs, {signals}) at which the buggy run differs."""
    _, _, good = simulate(nvc, bug, False)
    status, text, bad = simulate(nvc, bug, True)
    diff = sorted(set(bad) ^ set(good))
    if not diff:
        return None, status, text
    first = diff[0][0]
    return (first, sorted({p for t, p, v in diff if t == first})), status, text


def selected(args):
    fam = getattr(args, "family", "all")
    names = []
    if fam in ("all", "basic"):
        names += list(BUGS)
    if fam in ("all", "soc"):
        names += list(SOC_BUGS)
    if getattr(args, "scenario", None):
        names = [n for n in names if n in args.scenario]
    return names


def cmd_check(args):
    ok = True
    for bug in selected(args):
        gstatus, gtext, _ = simulate(args.nvc, bug, False)
        t, status, text = truth(args.nvc, bug)
        silent = bug.endswith("silent")
        flag = "ok"
        if gstatus != 0:
            flag = "GOOD DESIGN FAILS: " + gtext[-200:]
            ok = False
        elif t is None:
            flag = "NO DIVERGENCE"
            ok = False
        elif not silent and status == 0:
            flag = "TESTBENCH DOES NOT DETECT THE BUG"
            ok = False
        elif silent and status != 0:
            flag = "SILENT TESTBENCH FAILS"
            ok = False
        when = "%g ns" % (t[0] / 1e6) if t else "-"
        print("%-17s good=%d bad=%d first difference %s %s  %s"
              % (bug, gstatus, status, when, t[1] if t else "", flag),
              flush=True)
    return 0 if ok else 1


def spec_of(design):
    lines = []
    for line in read(design).splitlines():
        if not line.startswith("--"):
            break
        lines.append(line[2:].strip())
    return " ".join(x for x in lines if x)


SOC_PROMPT = (
    "src/ holds a VHDL design of {n} files: a small SoC (top level soc_top) "
    "with a UART command interface, registers, an interrupt controller, a "
    "timer and a video pipeline, and its self-checking testbench tb_soc. "
    "src/files.txt lists the files in compilation order (VHDL-2008). The "
    "comment at the top of each file is its specification. One bug has "
    "been introduced in the design (not in the testbench, tb_pkg or "
    "pkg_soc), so the testbench fails. Find the root cause: the earliest "
    "simulated time at which a signal inside the instance dut holds a "
    "wrong value according to the specification (not the time the "
    "testbench notices the problem), which signal it is, and the file and "
    "line of the faulty code. Do not edit any file. Finish with exactly "
    "one line in this form: RESULT: <time in ns> ns <signal path> "
    "<file>:<line>, for example RESULT: 1234 ns /dut/u_video/u_filter/cur "
    "filter3.vhd:57")


def prompt_for(bug, tb, blind=False):
    if bug in SOC_BUGS:
        return SOC_PROMPT.format(n=len(SOC_FILES))
    design = BUGS[bug][0]
    what = PROMPTS[bug]
    if blind:
        # No file tools: the specification is in the prompt and the agent
        # has to work from the simulation alone
        if bug.endswith("silent"):
            fail = ("Its testbench %s runs without any complaint, yet the "
                    "outputs are wrong." % tb)
        else:
            fail = "The testbench %s fails." % tb
        return ("src/%s.vhd holds %s and src/%s.vhd its testbench. %s You "
                "cannot read the source files, only analyse and simulate "
                "them. Specification of the design: %s Find the root cause: "
                "the earliest simulated time at which a signal inside the "
                "instance dut holds a wrong value (not the time the "
                "testbench notices the problem), and which signal it is. "
                "Finish with exactly one line in this form: "
                "RESULT: <time in ns> ns <signal path>, for example "
                "RESULT: 40 ns /dut/count"
                % (design, what, tb, fail, spec_of(design)))
    if bug.endswith("silent"):
        fail = ("Its testbench %s runs without any complaint, yet the "
                "outputs are wrong." % tb)
    else:
        fail = "The testbench %s fails." % tb
    return ("src/ holds %s and its testbench. %s The comment at the top of "
            "src/%s.vhd is the specification. Find the root cause: the "
            "earliest simulated time at which a signal inside the instance "
            "dut holds a wrong value (not the time the testbench notices "
            "the problem), and which signal it is. Simulate with nvc; do not "
            "edit any file. Finish with exactly one line in this form: "
            "RESULT: <time in ns> ns <signal path>, for example "
            "RESULT: 40 ns /dut/count" % (what, fail, design))


def norm_path(p):
    p = p.strip().strip("`").rstrip("/").lower()
    p = re.sub(r"\[.*?\]", "", p).replace(".", "/").replace(":", "/")
    p = p.lstrip("/")
    if p.startswith("dut/"):
        p = p[4:]
    return p


def judge(answer, expected, bug=None):
    """(passed, details): time and signal must match; for the SoC bugs the
    file must too, and the line is reported"""
    m = re.search(r"RESULT:\s*([0-9.]+)\s*ns\s+(\S+)(?:\s+(\S+))?\s*$",
                  answer.strip(), re.I | re.M)
    if not m:
        return False, {"why": "no RESULT line"}
    when = float(m.group(1)) * 1e6
    path = norm_path(m.group(2))
    t, sigs = expected
    d = {"time_ok": abs(when - t) <= 1,
         "near_time": abs(when - t) <= 10e6 + 1,
         "signal_ok": path in [norm_path(x) for x in sigs]}
    if bug in SOC_BUGS and len(SOC_BUGS[bug]) > 2 and not d["signal_ok"]:
        alt = [norm_path(x) for x in SOC_BUGS[bug][2]]
        if path in alt and t - 2e9 <= when <= t:
            d["signal_ok"] = d["time_ok"] = d["near_time"] = True
    if bug in SOC_BUGS:
        fname, lo, hi = soc_bug_line(bug)
        loc = m.group(3) or ""
        lm = re.match(r"(?:.*/)?([\w.]+?):(\d+)", loc)
        d["file_ok"] = bool(lm) and lm.group(1).lower() == fname
        d["line_ok"] = d["file_ok"] and lo - 3 <= int(lm.group(2)) <= hi + 3
    ok = d["time_ok"] and d["signal_ok"]
    d["why"] = "ok" if ok else "answered %s ns %s %s, expected %g ns %s" % (
        m.group(1), m.group(2), m.group(3) or "", t / 1e6, sigs)
    return ok, d


async def run_one(nvc, model, bug, expected, tools, label, blind=False,
                  disallowed=(), max_turns=40, budget=1.0):
    from claude_agent_sdk import (query, ClaudeAgentOptions, AssistantMessage,
                                  ResultMessage, ToolUseBlock)
    ws, design, tb = make_workspace(bug, True)
    prompt = prompt_for(bug, tb, blind)
    options = ClaudeAgentOptions(
        model=model, cwd=ws, cli_path=shutil.which("claude"),
        system_prompt={"type": "preset", "preset": "claude_code"},
        setting_sources=[], tools=tools,
        allowed_tools=["mcp__nvc"] + tools,
        disallowed_tools=["mcp__nvc__" + x for x in disallowed],
        mcp_servers={"nvc": {"type": "stdio", "command": nvc,
                             "args": ["--mcp", "--root", ws],
                             "env": {"NVC_LIBPATH":
                                     nvc_env(nvc)["NVC_LIBPATH"]}}},
        max_turns=max_turns, max_budget_usd=budget, stderr=lambda line: None)
    calls, result = [], None
    async for m in query(prompt=prompt, options=options):
        if isinstance(m, AssistantMessage):
            for b in m.content:
                if isinstance(b, ToolUseBlock):
                    calls.append(b.name.replace("mcp__nvc__", "nvc:"))
        elif isinstance(m, ResultMessage):
            result = m
    shutil.rmtree(ws, ignore_errors=True)
    text = (result.result or "") if result else ""
    ok, det = judge(text, expected, bug)
    row = {"bug": bug, "label": label, "model": model, "passed": ok,
           "why": det.pop("why"), **det, "calls": len(calls),
           "turns": result.num_turns if result else 0,
           "cost_usd": round((result.total_cost_usd or 0) if result else 0, 4),
           "seconds": round(result.duration_ms / 1000, 1) if result else 0,
           "sequence": " ".join(calls)}
    print(json.dumps(row), flush=True)
    return row


async def cmd_run(args):
    rows = []
    cache = {}
    for bug in selected(args):
        t, _, _ = truth(args.nvc, bug)
        cache[bug] = t
        for trial in range(args.trials):
            rows.append(await run_one(
                args.nvc, args.model, bug, t,
                [] if args.blind else ["Read", "Glob", "Grep"], args.label,
                args.blind, args.disable or [], args.max_turns,
                args.budget))
    n = len(rows)
    print("\n==== %s, model %s ====" % (args.label, args.model))
    for bug in selected(args):
        r = [x for x in rows if x["bug"] == bug]
        if r:
            extra = ""
            if bug in SOC_BUGS:
                extra = "  file %d/%d  line %d/%d  near %d/%d" % (
                    sum(x.get("file_ok", 0) for x in r), len(r),
                    sum(x.get("line_ok", 0) for x in r), len(r),
                    sum(x.get("near_time", 0) and x.get("signal_ok", 0)
                        for x in r), len(r))
            print("%-17s %d/%d  calls %.1f  cost $%.4f%s" % (
                bug, sum(x["passed"] for x in r), len(r),
                sum(x["calls"] for x in r) / len(r),
                sum(x["cost_usd"] for x in r) / len(r), extra))
    print("passed %d of %d, calls %.1f, cost $%.4f" % (
        sum(x["passed"] for x in rows), n,
        sum(x["calls"] for x in rows) / max(n, 1),
        sum(x["cost_usd"] for x in rows)))
    if args.out:
        with open(args.out, "w") as f:
            json.dump(rows, f, indent=1)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nvc", required=True)
    ap.add_argument("cmd", choices=["check", "run"])
    ap.add_argument("--model", default="claude-haiku-5-5")
    ap.add_argument("--trials", type=int, default=1)
    ap.add_argument("--scenario", action="append")
    ap.add_argument("--family", choices=["all", "basic", "soc"],
                    default="all")
    ap.add_argument("--max-turns", type=int, default=40)
    ap.add_argument("--budget", type=float, default=1.0,
                    help="maximum cost of one agent run in dollars")
    ap.add_argument("--label", default="baseline")
    ap.add_argument("--blind", action="store_true",
                    help="no file tools, specification in the prompt")
    ap.add_argument("--disable", action="append",
                    help="MCP tool to hide, such as nvc_wave_table")
    ap.add_argument("--out")
    a = ap.parse_args()
    a.nvc = os.path.abspath(a.nvc)
    if a.cmd == "check":
        return cmd_check(a)
    return asyncio.run(cmd_run(a))


if __name__ == "__main__":
    sys.exit(main())
