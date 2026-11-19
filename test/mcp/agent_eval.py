#!/usr/bin/env python3
#
# Evaluate the nvc MCP server with a real agent (Claude Agent SDK).
#
# This is not part of "make check": it calls a model, so it costs credits
# (a few thousandths of a dollar per scenario with a small model) and
# needs network access.  Run it after changing a tool description, a
# schema or an error message, and read the printed transcripts: what the
# agent does is the result, the pass/fail column only summarises it.
#
#   pip install claude-agent-sdk      # needs the claude CLI, logged in
#   python3 test/mcp/agent_eval.py --nvc build/bin/nvc
#
# Each scenario gets a fresh workspace.  The outcome is checked by running
# nvc directly on the workspace, never by trusting the agent's report.

import argparse
import asyncio
import json
import os
import shutil
import subprocess
import sys
import tempfile

from claude_agent_sdk import (query, ClaudeAgentOptions, AssistantMessage,
                              UserMessage, ResultMessage, TextBlock,
                              ToolUseBlock, ToolResultBlock)

COUNTER = """\
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity counter is
  port (clk : in std_logic; rst : in std_logic; q : out unsigned(3 downto 0));
end entity;

architecture rtl of counter is
  signal cnt : unsigned(3 downto 0);
begin
  process (clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        cnt <= (others => '0');
      else
        cnt <= cnt + 1;
      end if;
    end if;
  end process;

  q <= %s;
end architecture;
"""

TESTBENCH = """\
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity counter_tb is
end entity;

architecture test of counter_tb is
  signal clk : std_logic := '0';
  signal rst : std_logic := '1';
  signal q   : unsigned(3 downto 0);
begin
  dut : entity work.counter port map (clk => clk, rst => rst, q => q);
  clk <= not clk after 5 ns;
  process
  begin
    wait for 12 ns;
    rst <= '0';
    wait for 100 ns;
    assert q /= 0 report "counter did not count" severity failure;
    wait;
  end process;
end architecture;
"""

FINISH = "Finish with exactly one line: RESULT: PASS or RESULT: FAIL."

# name, prompt, uses file tools, design has a bug, files in dependency order
SCENARIOS = [
    ("fix-error",
     "The directory src/ contains a small VHDL design: a counter and its "
     "testbench. Check whether the design compiles with nvc. If it does "
     "not, explain the problem, fix the source, and confirm that "
     "everything now compiles. " + FINISH,
     True, True),
    ("order-and-std",
     "Using the VHDL-2008 standard, compile src/counter_tb.vhd and "
     "src/counter.vhd with nvc. Tell me whether it worked and, if not, "
     "why. " + FINISH,
     True, False),
    ("wrong-path-no-file-tools",
     "Analyse the file counter.vhd with nvc and tell me the outcome. You "
     "only have the nvc tool. " + FINISH,
     False, False),
]


def short(x, n=400):
    s = x if isinstance(x, str) else json.dumps(x, ensure_ascii=False)
    return s if len(s) <= n else s[:n] + "...[+%d]" % (len(s) - n)


def nvc_env(nvc):
    env = dict(os.environ)
    env["NVC_LIBPATH"] = os.path.join(os.path.dirname(nvc), "..", "lib")
    return env


def make_workspace(buggy):
    ws = tempfile.mkdtemp(prefix="nvc-agent-eval-")
    os.makedirs(os.path.join(ws, "src"))
    with open(os.path.join(ws, "src", "counter.vhd"), "w") as f:
        f.write(COUNTER % ("count" if buggy else "cnt"))
    with open(os.path.join(ws, "src", "counter_tb.vhd"), "w") as f:
        f.write(TESTBENCH)
    return ws


def design_compiles(nvc, ws):
    # Independent check: analyse the files in dependency order
    r = subprocess.run([nvc, "-a", "src/counter.vhd", "src/counter_tb.vhd"],
                       cwd=ws, env=nvc_env(nvc), capture_output=True)
    return r.returncode == 0


async def run(nvc, model, name, prompt, file_tools, buggy):
    ws = make_workspace(buggy)
    allowed = ["mcp__nvc__nvc_analyse"]
    if file_tools:
        allowed += ["Read", "Edit", "Glob"]

    options = ClaudeAgentOptions(
        model=model, cwd=ws, cli_path=shutil.which("claude"),
        system_prompt={"type": "preset", "preset": "claude_code"},
        setting_sources=[],
        tools=["Read", "Edit", "Glob"] if file_tools else [],   # no Bash
        allowed_tools=allowed,
        mcp_servers={"nvc": {"type": "stdio", "command": nvc,
                             "args": ["--mcp", "--root", ws],
                             "env": {"NVC_LIBPATH":
                                     nvc_env(nvc)["NVC_LIBPATH"]}}},
        max_turns=14, max_budget_usd=1.0, stderr=lambda line: None)

    print("\n######## %s\nPROMPT: %s" % (name, prompt))
    names, mcp_calls, mcp_errors, result = {}, 0, 0, None
    async for m in query(prompt=prompt, options=options):
        if isinstance(m, AssistantMessage):
            for b in m.content:
                if isinstance(b, ToolUseBlock):
                    names[b.id] = b.name
                    mcp_calls += b.name.startswith("mcp__nvc__")
                    print("  -> %s %s" % (b.name, short(b.input, 300)))
                elif isinstance(b, TextBlock) and b.text.strip():
                    print("  [say] %s" % short(b.text, 500))
        elif isinstance(m, UserMessage) and isinstance(m.content, list):
            for b in m.content:
                if isinstance(b, ToolResultBlock):
                    if b.is_error and names.get(b.tool_use_id, "").startswith(
                            "mcp__nvc__"):
                        mcp_errors += 1
                    print("  <- error=%s %s" % (bool(b.is_error),
                                                short(b.content)))
        elif isinstance(m, ResultMessage):
            result = m

    said_pass = (result.result or "").strip().endswith("RESULT: PASS")
    if buggy:
        verified = design_compiles(nvc, ws)   # the agent had to repair it
    else:
        verified = os.path.isdir(os.path.join(ws, "work"))
    shutil.rmtree(ws, ignore_errors=True)

    return {"scenario": name, "passed": said_pass and verified,
            "said_pass": said_pass, "verified": verified,
            "turns": result.num_turns, "mcp_calls": mcp_calls,
            "mcp_errors": mcp_errors,
            "seconds": round(result.duration_ms / 1000, 1),
            "cost_usd": round(result.total_cost_usd or 0, 5)}


async def main():
    ap = argparse.ArgumentParser(
        description="Evaluate the nvc MCP server with a Claude agent")
    ap.add_argument("--nvc", required=True, help="path to the nvc binary")
    ap.add_argument("--model", default="claude-haiku-5-5")
    ap.add_argument("--scenario", help="run only this scenario")
    args = ap.parse_args()

    nvc = os.path.abspath(args.nvc)
    rows = [await run(nvc, args.model, *s) for s in SCENARIOS
            if args.scenario in (None, s[0])]

    print("\n==== SUMMARY (model %s) ====" % args.model)
    for r in rows:
        print(json.dumps(r))
    print("total cost: $%.4f" % sum(r["cost_usd"] for r in rows))
    return 0 if rows and all(r["passed"] for r in rows) else 1


sys.exit(asyncio.run(main()))
