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
# Each scenario gets a fresh workspace.  The outcome is checked against
# what nvc itself reports, never by trusting the agent's account.  The
# prompts do not name any tool: choosing them is part of the test.

import argparse
import asyncio
import json
import os
import re
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

# The counter is expected to stay below five: it does not, at 65 ns
CHECKED_TESTBENCH = """\
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity checked_tb is
end entity;

architecture test of checked_tb is
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
    wait;
  end process;
  process (clk)
  begin
    if rising_edge(clk) and rst = '0' then
      assert q /= 5 report "counter reached five" severity error;
    end if;
  end process;
end architecture;
"""

# Runs for about half a minute of real time, which no single call may wait for
LONG_TESTBENCH = """\
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity long_tb is
end entity;

architecture test of long_tb is
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
    wait for 200 ms;
    report "long run complete" severity note;
    std.env.finish;
  end process;
end architecture;
"""

FINISH = "Finish with exactly one line: RESULT: PASS or RESULT: FAIL."
FILE_TOOLS = ["Read", "Edit", "Glob"]

SCENARIOS = [
    dict(name="fix-error", files="buggy", tools=FILE_TOOLS, check="compiles",
         prompt="The directory src/ contains a small VHDL design: a counter "
         "and its testbench. Check whether the design compiles with nvc. If "
         "it does not, explain the problem, fix the source, and confirm "
         "that everything now compiles. " + FINISH),
    dict(name="order-and-std", files="good", tools=FILE_TOOLS, check="library",
         prompt="Using the VHDL-2008 standard, compile src/counter_tb.vhd "
         "and src/counter.vhd with nvc. Tell me whether it worked and, if "
         "not, why. " + FINISH),
    dict(name="wrong-path-no-file-tools", files="good", tools=[],
         check="library",
         prompt="Analyse the file counter.vhd with nvc and tell me the "
         "outcome. You only have the nvc tool. " + FINISH),
    dict(name="discover-units", files="good", tools=[],
         check=r"RESULT:.*counter_tb.*|RESULT:.*counter.*counter_tb",
         both=("counter", "counter_tb"),
         prompt="I do not know what is in this workspace. Find out which "
         "VHDL entities it defines. Finish with exactly one line in this "
         "form: RESULT: <entity names separated by commas>"),
    dict(name="trace-lookback", files="checked", tools=[],
         check=r"RESULT:\s*(2\s*,\s*)?3\s*,\s*4\s*,\s*5\s*$",
         must_call="nvc_wave_start",
         prompt="src/ holds a counter and a testbench called checked_tb whose "
         "assertion fails at some point. Simulate the testbench to that "
         "failure, running the simulation only once and never restarting "
         "it, then tell me the values the counter output q took during the "
         "30 ns before the failure, oldest first, in decimal. Finish with "
         "exactly one line in this form: RESULT: <values separated by "
         "commas>"),
    dict(name="debug-assertion", files="checked", tools=[],
         check=r"RESULT:\s*65\s*ns\s+5\s*$",
         prompt="src/ holds a counter and a testbench called checked_tb. "
         "Simulate the testbench and find out at what simulated time its "
         "assertion fails, and the value, in decimal, of the register cnt "
         "inside the counter instance at that moment. Finish with exactly "
         "one line in this form: RESULT: <time in ns> <value>, for example "
         "RESULT: 40 ns 3"),
    dict(name="long-run", files="long", tools=[],
         check=r"RESULT:.*long run complete", must_call="nvc_sim_wait",
         prompt="src/ holds a counter and a testbench called long_tb that "
         "takes about half a minute of real time to finish. Run it to its "
         "end and tell me the message it prints when it completes. Finish "
         "with exactly one line in this form: RESULT: <message>"),
    dict(name="rewind", files="longfail", tools=[],
         check=r"RESULT:\s*15\s*$", must_call="nvc_sim_restore",
         prompt="src/ holds a counter and a testbench called long_tb that "
         "takes about half a minute of real time and then fails. First run "
         "it to the failure just as it is, with default settings and "
         "without recording any signal: I only know afterwards what I want "
         "to look at. Then tell me the value, in decimal, of the register "
         "cnt inside the counter instance 15 ns before the time of the "
         "failure. Running again from the start costs another half minute: "
         "avoid that if the tools allow. Finish with exactly one line in "
         "this form: RESULT: <value>"),
    dict(name="wave-find", files="checked", tools=[],
         check=r"RESULT:\s*55\s*ns\s*$", must_call="nvc_wave_start",
         prompt="src/ holds a counter and a testbench called checked_tb. "
         "Simulate the testbench for 200 ns while recording a waveform file "
         "of only the signals inside the counter instance, then use that "
         "file to find the first simulated time at which the register cnt "
         "inside the counter becomes 5. Finish with exactly one line in "
         "this form: RESULT: <time in ns> ns"),
]


def short(x, n=400):
    s = x if isinstance(x, str) else json.dumps(x, ensure_ascii=False)
    return s if len(s) <= n else s[:n] + "...[+%d]" % (len(s) - n)


def nvc_env(nvc):
    env = dict(os.environ)
    env["NVC_LIBPATH"] = os.path.join(os.path.dirname(nvc), "..", "lib")
    return env


def make_workspace(kind):
    ws = tempfile.mkdtemp(prefix="nvc-agent-eval-")
    os.makedirs(os.path.join(ws, "src"))

    def put(name, text):
        with open(os.path.join(ws, "src", name), "w") as f:
            f.write(text)

    put("counter.vhd", COUNTER % ("count" if kind == "buggy" else "cnt"))
    if kind == "checked":
        put("checked_tb.vhd", CHECKED_TESTBENCH)
    elif kind == "long":
        put("long_tb.vhd", LONG_TESTBENCH)
    elif kind == "longfail":
        put("long_tb.vhd", LONG_TESTBENCH.replace(
            'report "long run complete" severity note;',
            'assert false report "late failure" severity failure;'))
    else:
        put("counter_tb.vhd", TESTBENCH)
    return ws


def design_compiles(nvc, ws):
    # Independent check: analyse the files in dependency order
    r = subprocess.run([nvc, "-a", "src/counter.vhd", "src/counter_tb.vhd"],
                       cwd=ws, env=nvc_env(nvc), capture_output=True)
    return r.returncode == 0


async def run(nvc, model, sc):
    name, prompt = sc["name"], sc["prompt"]
    ws = make_workspace(sc["files"])

    options = ClaudeAgentOptions(
        model=model, cwd=ws, cli_path=shutil.which("claude"),
        system_prompt={"type": "preset", "preset": "claude_code"},
        setting_sources=[],
        tools=sc["tools"],                       # never Bash
        allowed_tools=["mcp__nvc"] + sc["tools"],
        mcp_servers={"nvc": {"type": "stdio", "command": nvc,
                             "args": ["--mcp", "--root", ws],
                             "env": {"NVC_LIBPATH":
                                     nvc_env(nvc)["NVC_LIBPATH"]}}},
        max_turns=20, max_budget_usd=1.0, stderr=lambda line: None)

    print("\n######## %s\nPROMPT: %s" % (name, prompt))
    names, calls, mcp_errors, result = {}, [], 0, None
    async for m in query(prompt=prompt, options=options):
        if isinstance(m, AssistantMessage):
            for b in m.content:
                if isinstance(b, ToolUseBlock):
                    names[b.id] = b.name
                    calls.append(b.name.replace("mcp__nvc__", "nvc:"))
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

    text = (result.result or "").strip()
    last = text.splitlines()[-1] if text else ""

    check = sc["check"]
    if check == "compiles":
        ok = last.endswith("RESULT: PASS") and design_compiles(nvc, ws)
    elif check == "library":
        ok = last.endswith("RESULT: PASS") and os.path.isdir(
            os.path.join(ws, "work"))
    else:
        ok = re.search(check, last, re.I) is not None
        for word in sc.get("both", ()):
            ok = ok and re.search(r"\b%s\b" % word, last, re.I) is not None

    # Some scenarios are only meaningful if a particular tool was used
    must = sc.get("must_call")
    if must and not any(c.endswith(must) for c in calls):
        print("  the scenario needed %s, which was never called" % must)
        ok = False
    shutil.rmtree(ws, ignore_errors=True)

    print("  final line: %s" % short(last, 200))
    return {"scenario": name, "passed": bool(ok), "turns": result.num_turns,
            "calls": len(calls), "mcp_errors": mcp_errors,
            "sequence": " ".join(calls),
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
    rows = [await run(nvc, args.model, sc) for sc in SCENARIOS
            if args.scenario in (None, sc["name"])]

    print("\n==== SUMMARY (model %s) ====" % args.model)
    for r in rows:
        print(json.dumps(r))
    print("passed %d of %d, total cost: $%.4f"
          % (sum(r["passed"] for r in rows), len(rows),
             sum(r["cost_usd"] for r in rows)))
    return 0 if rows and all(r["passed"] for r in rows) else 1


sys.exit(asyncio.run(main()))
