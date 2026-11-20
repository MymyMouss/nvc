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

# name -> (design file, testbench file, top, (old, new) patch on the design)
BUGS = {
    "ctrl_skip": ("ctrl", "ctrl_tb",
                  ("state <= WAIT_ACK;", "state <= FINISH;")),
    "ctrl_stuck": ("ctrl", "ctrl_tb",
                   ("            state <= WAIT_ACK;\n            if ack = '1' then\n"
                    "              ack_seen <= '1';\n            end if;\n",
                    "            state <= WAIT_ACK;\n")),
    "fifo_drop": ("fifo", "fifo_tb",
                  [("count < 4 else '0'", "count <= 4 else '0'"),
                   ("valid_in = '1' and count < 4", "valid_in = '1' and count <= 4")]),
    "fifo_dup": ("fifo", "fifo_tb",
                 ("if pop then\n          rd_p", "if pop and count > 1 then\n          rd_p")),
    "uart_parity": ("uart_tx", "uart_tx_tb",
                    ("data(4) xor data(5) xor data(6) xor data(7)",
                     "data(4) xor data(5) xor data(6)")),
    "video_line": ("video_sync", "video_sync_tb",
                   ("elsif hcount = 15 then", "elsif hcount = 16 then")),
    "pipe_valid": ("pipe", "pipe_tb", ("v2 <= v1;", "v2 <= in_valid;")),
    # Same bug without any checking in the testbench: only the spec
    # tells what is wrong
    "pipe_silent": ("pipe", "pipe_tb_silent", ("v2 <= v1;", "v2 <= in_valid;")),
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
}


def read(name):
    with open(os.path.join(HERE, name + ".vhd")) as f:
        return f.read()


def silent_tb():
    text = read("pipe_tb")
    text = re.sub(r"  --CHECK-BEGIN.*--CHECK-END\n", "", text, flags=re.S)
    return text.replace("entity pipe_tb ", "entity pipe_tb_silent ") \
               .replace("of pipe_tb ", "of pipe_tb_silent ")


def sources(bug, buggy):
    design, tb, patch = BUGS[bug]
    src = read(design)
    if buggy:
        for old, new in (patch if isinstance(patch, list) else [patch]):
            assert old in src, (bug, old)
            src = src.replace(old, new)
    tbsrc = silent_tb() if tb == "pipe_tb_silent" else read(tb)
    return design, src, tb, tbsrc


def make_workspace(bug, buggy):
    ws = tempfile.mkdtemp(prefix="nvc-bench-")
    os.makedirs(os.path.join(ws, "src"))
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
        r = subprocess.run(
            [nvc, "--std=2008", "-a", "src/%s.vhd" % design,
             "src/%s.vhd" % tb, "-e", tb, "-r", "--stop-time=5us", "--wave=" + fst],
            cwd=ws, env=env, capture_output=True, text=True)
        events = []
        if os.path.exists(fst):
            dump = os.path.join(os.path.dirname(nvc), "fstdump")
            out = subprocess.run([dump, fst], capture_output=True, text=True)
            for line in out.stdout.splitlines():
                m = re.match(r"#(\d+) (\S+) (.*)", line)
                if m and ".dut." in m.group(2):
                    path = m.group(2).split(".dut.", 1)[1]
                    path = re.sub(r"\[.*\]$", "", path)
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


def cmd_check(args):
    ok = True
    for bug in BUGS:
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
        print("%-12s good=%d bad=%d first difference %s %s  %s"
              % (bug, gstatus, status, when, t[1] if t else "", flag))
    return 0 if ok else 1


def spec_of(design):
    lines = []
    for line in read(design).splitlines():
        if not line.startswith("--"):
            break
        lines.append(line[2:].strip())
    return " ".join(x for x in lines if x)


def prompt_for(bug, tb, blind=False):
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


def judge(answer, expected):
    m = re.search(r"RESULT:\s*([0-9.]+)\s*ns\s+(\S+)\s*$", answer.strip(),
                  re.I | re.M)
    if not m:
        return False, "no RESULT line"
    when = float(m.group(1)) * 1e6
    leaf = m.group(2).rstrip("/").split("/")[-1].lower()
    leaf = re.sub(r"\[.*\]$", "", leaf)
    t, sigs = expected
    if abs(when - t) > 1 or leaf not in [x.lower() for x in sigs]:
        return False, "answered %s ns %s, expected %g ns %s" % (
            m.group(1), m.group(2), t / 1e6, sigs)
    return True, "ok"


async def run_one(nvc, model, bug, expected, tools, label, blind=False,
                  disallowed=()):
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
        max_turns=40, max_budget_usd=1.0, stderr=lambda line: None)
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
    ok, why = judge(text, expected)
    row = {"bug": bug, "label": label, "model": model, "passed": ok,
           "why": why, "calls": len(calls),
           "turns": result.num_turns if result else 0,
           "cost_usd": round((result.total_cost_usd or 0) if result else 0, 4),
           "seconds": round(result.duration_ms / 1000, 1) if result else 0,
           "sequence": " ".join(calls)}
    print(json.dumps(row), flush=True)
    return row


async def cmd_run(args):
    rows = []
    cache = {}
    for bug in BUGS:
        if args.scenario and bug not in args.scenario:
            continue
        t, _, _ = truth(args.nvc, bug)
        cache[bug] = t
        for trial in range(args.trials):
            rows.append(await run_one(
                args.nvc, args.model, bug, t,
                [] if args.blind else ["Read", "Glob"], args.label,
                args.blind, args.disable or []))
    n = len(rows)
    print("\n==== %s, model %s ====" % (args.label, args.model))
    for bug in BUGS:
        r = [x for x in rows if x["bug"] == bug]
        if r:
            print("%-12s %d/%d  calls %.1f  cost $%.4f" % (
                bug, sum(x["passed"] for x in r), len(r),
                sum(x["calls"] for x in r) / len(r),
                sum(x["cost_usd"] for x in r) / len(r)))
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
