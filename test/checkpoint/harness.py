#!/usr/bin/env python3
#
# Validation harness for nvc checkpoints (nvc -r --checkpoint / --restore).
#
# For each design it
#
#   1. runs straight to the end with a waveform dump,
#   2. picks a time T near the middle of the run that is an event time in
#      that dump, and runs again with `--checkpoint=F@T --stop-time=T`.  The
#      output of this run is exactly what the straight run printed up to T,
#   3. restores F in a new process (`--restore=F`) with a waveform dump and
#      lets it run to the end,
#   4. compares what the restored run printed (stdout and stderr, separately)
#      with what the straight run printed after the output of step 2, the
#      exit status, and the waveform from T on: the value of every signal at
#      T and all later changes, taken from the FST dumps with build/bin/fstdump.
#
# A design is "refused" if nvc declined to write the checkpoint (the message
# is kept), "identical" if everything agrees, and "different" otherwise.
# "n/a" means the test cannot be run this way (expected elaboration failure,
# plugins, shell or Tcl driven, no simulated time elapses ...).
#
# Usage (from the build directory, after `make`):
#
#   ../test/checkpoint/harness.py regress [NAME ...]    # default: all
#   ../test/checkpoint/harness.py bench                 # test/mcp/bench
#   ../test/checkpoint/harness.py design --top T --stop 5us a.vhd b.vhd
#
# Common options: --at Q1,Q2,.. (positions of the checkpoint as fractions
# of the event times of the run, default 0.5), --nvc PATH (default bin/nvc), --lib DIR (default lib),
# --json FILE (full results), --keep (keep the temporary directories),
# --timeout SECONDS (per nvc command, default 120).
#
# Exit status is 1 if any design is "different".

import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


class Design:
    def __init__(self, name, files, top, std="1993", stop=None, gens=(),
                 env=None, aflags=(), rflags=(), heap=None, olevel=0,
                 expect_fail=False):
        self.name = name
        self.files = list(files)
        self.top = top
        self.std = std
        self.stop = stop
        self.gens = list(gens)
        self.env = dict(env or {})
        self.aflags = list(aflags)
        self.rflags = list(rflags)
        self.heap = heap
        self.olevel = olevel
        self.expect_fail = expect_fail


class NA(Exception):
    pass


def run(cmd, cwd, env, timeout):
    t0 = time.time()
    try:
        p = subprocess.run(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, b"", b"", time.time() - t0
    return p.returncode, p.stdout, p.stderr, time.time() - t0


# --------------------------------------------------------------------------
# Test lists

def regress_designs(names):
    path = os.path.join(ROOT, "test", "regress", "testlist.txt")
    designs = []
    skipped = []
    for line in open(path):
        line = line.split("#")[0].strip()
        if not line:
            continue
        name, _, opts = line.partition(" ")
        opts = opts.strip().split(",") if opts.strip() else []
        if names and name not in names:
            continue

        d = Design(name, [], name)
        why = None
        for o in opts:
            if o in ("2000", "2002", "2008", "2019"):
                d.std = o
            elif o == "fail":
                d.expect_fail = True
            elif o.startswith("stop="):
                d.stop = o[5:]
            elif o in ("vhpi", "shell", "tcl", "slow", "gtkw", "wave",
                       "shuffle", "!windows", "per-file") \
                    or o.startswith(("plugin=", "cover", "work=", "export")):
                if o not in ("!windows",):
                    why = o
            elif o in ("mixed", "verilog"):
                pass
            elif o == "psl":
                d.aflags.append("--psl")
            elif o == "relaxed":
                d.aflags.append("--relaxed")
            elif o.startswith("relax="):
                d.aflags.append("--relax=" + o[6:])
            elif o.startswith("define="):
                d.aflags.append("--define=" + o[7:])
            elif o == "no-collapse":
                d.aflags.append("--no-collapse")
            elif o.startswith("H="):
                d.heap = o[2:]
            elif o.startswith("O"):
                d.olevel = int(o[1:])
            elif o.startswith("g") and "=" in o:
                k, v = o[1:].split("=", 1)
                d.gens.append((k, v))
            elif o.startswith("$") and "=" in o:
                k, v = o[1:].split("=", 1)
                d.env[k] = v
            elif o.startswith("+"):
                d.rflags.append(o)
            elif o.startswith("seed=") or o.startswith("dump-arrays") \
                    or o.startswith("exit="):
                pass
            elif o in ("normal", "gold"):
                pass
            else:
                why = "option " + o

        reg = os.path.join(ROOT, "test", "regress")
        if os.path.exists(os.path.join(reg, name + ".vhd")) \
                and "verilog" not in opts:
            d.files.append(os.path.join(reg, name + ".vhd"))
        for ext in (".sv", ".v"):
            if ("mixed" in opts or "verilog" in opts) \
                    and os.path.exists(os.path.join(reg, name + ext)):
                d.files.append(os.path.join(reg, name + ext))
                break

        if why is None and not d.files:
            why = "no source file"
        d.skip = why
        designs.append(d)
    return designs


BENCH = [
    # design file, testbench file, top, stop time
    ("ctrl", "ctrl_tb", "ctrl_tb", "5us"),
    ("fifo", "fifo_tb", "fifo_tb", "5us"),
    ("uart_tx", "uart_tx_tb", "uart_tx_tb", "5us"),
    ("video_sync", "video_sync_tb", "video_sync_tb", "5us"),
    ("pipe", "pipe_tb", "pipe_tb", "5us"),
    ("dma", "dma_tb", "dma_tb", "40us"),
    ("arb", "arb_tb", "arb_tb", "3us"),
    ("uart_rx", "uart_rx_tb", "uart_rx_tb", "15us"),
    ("crc_tx", "crc_tx_tb", "crc_tx_tb", "100us"),
    ("timer", "timer_tb", "timer_tb", "300us"),
]

SOC_FILES = ["pkg_soc", "uart_rx", "uart_tx", "sync_fifo", "crc8_unit",
             "watchdog", "cmd_parser", "reg_bank", "irq_ctrl", "timer",
             "video_timing", "pattern_gen", "filter3", "gain_offset",
             "threshold_stats", "rr_arbiter", "rsp_builder", "tx_path",
             "ctrl_path", "video_pipe", "soc_top", "tb_pkg", "tb_soc"]


def bench_designs():
    b = os.path.join(ROOT, "test", "mcp", "bench")
    out = []
    for dut, tb, top, stop in BENCH:
        d = Design("bench_" + dut,
                   [os.path.join(b, dut + ".vhd"), os.path.join(b, tb + ".vhd")],
                   top, std="2008", stop=stop)
        d.skip = None
        out.append(d)
    soc = Design("bench_soc",
                 [os.path.join(b, "soc", f + ".vhd") for f in SOC_FILES],
                 "tb_soc", std="2008", stop="3ms")
    soc.skip = None
    out.append(soc)
    return out


# --------------------------------------------------------------------------
# Waveforms

def parse_dump(text):
    """Return {time: {name: value}} from fstdump output."""
    out = collections.OrderedDict()
    for line in text.splitlines():
        if not line.startswith("#"):
            continue
        parts = line.split(" ", 2)
        if len(parts) != 3:
            continue
        t = int(parts[0][1:])
        out.setdefault(t, {})[parts[1]] = parts[2]
    return out


def waveform_diff(straight, restored, t):
    """Compare from time t.  Returns a list of difference descriptions."""
    diffs = []

    # Value of every signal at t in the straight run
    state = {}
    for time_, ch in straight.items():
        if time_ <= t:
            state.update(ch)

    snap = restored.get(t, {})
    for name, val in state.items():
        if name not in snap:
            diffs.append("restored dump has no value for %s at %d" % (name, t))
        elif snap[name] != val:
            diffs.append("%s at %d: straight %s, restored %s"
                         % (name, t, val, snap[name]))
    for name in snap:
        if name not in state:
            diffs.append("restored dump has extra signal %s" % name)

    later_s = {k: v for k, v in straight.items() if k > t}
    later_r = {k: v for k, v in restored.items() if k > t}
    for time_ in sorted(set(later_s) | set(later_r)):
        a, b = later_s.get(time_, {}), later_r.get(time_, {})
        if a != b:
            for name in sorted(set(a) | set(b)):
                if a.get(name) != b.get(name):
                    diffs.append("%s at %d: straight %s, restored %s"
                                 % (name, time_, a.get(name), b.get(name)))
                    break
            else:
                continue
        if len(diffs) > 5:
            break
    return diffs


# --------------------------------------------------------------------------
# One design

UNITS = {b"fs": 1, b"ps": 1000, b"ns": 1000000, b"us": 1000000000,
         b"ms": 1000000000000, b"sec": 1000000000000000}
CYCLE_RE = re.compile(rb"^TRACE (\d+)(fs|ps|ns|us|ms|sec)\+0: begin cycle",
                      re.M)


def trace_times(err):
    return [int(m.group(1)) * UNITS[m.group(2)] for m in CYCLE_RE.finditer(err)]


NOTE_RE = re.compile(rb"^\*\* Note: [^\n]*checkpoint [^\n]* (?:written|restored) at "
                     rb"[^\n]*\n|^\*\* Note: restored checkpoint [^\n]*\n", re.M)


WAVE_NOTE_RE = re.compile(rb"^\*\* Note: arrays of composite types[^\n]*\n", re.M)


BLOCK_RE = re.compile(rb"^(?=\*\* (?:Note|Warning|Error|Fatal|Failure)\b)", re.M)
STAMP_RE = re.compile(rb"^\*\* \w+: (\(init\)|\d+(?:fs|ps|ns|us|ms|sec)\+\d+): ")


ADDR_RE = re.compile(rb"0x[0-9a-f]{8,16}")


def norm(b):
    # Notes that depend only on how the run was invoked, and heap addresses
    # which differ between any two runs
    return ADDR_RE.sub(b"0xADDR", WAVE_NOTE_RE.sub(b"", NOTE_RE.sub(b"", b)))


def blocks(b):
    """Split diagnostics into blocks: (is_init, text).  Blocks that carry
    no simulation time are tool messages (waveform dumper warnings) and are
    dropped.  Anything before the first block is kept as one block."""
    out = []
    for part in BLOCK_RE.split(b):
        if not part:
            continue
        m = STAMP_RE.match(part)
        if m is None:
            if part.startswith(b"** "):
                continue
            out.append((False, part))
        else:
            out.append((m.group(1) == b"(init)", part))
    return out


def stderr_diff(s_err, a_err, r_err):
    """The restored run repeats the reset, so the '(init)' diagnostics
    appear at its start; the rest must equal what the straight run printed
    after the checkpoint."""
    sb, ab, rb = blocks(s_err), blocks(a_err), blocks(r_err)
    s_init = [t for i, t in sb if i]
    r_init = [t for i, t in rb if i]
    s_rest = [t for i, t in sb if not i]
    a_rest = [t for i, t in ab if not i]
    r_rest = [t for i, t in rb if not i]
    if r_init != s_init:
        return "diagnostics during reset differ"
    if s_rest[:len(a_rest)] != a_rest:
        return "output before the checkpoint is not a prefix"
    if r_rest != s_rest[len(a_rest):]:
        return ("stderr differs: restored %r, expected %r"
                % (b"".join(r_rest)[:300], b"".join(s_rest[len(a_rest):])[:300]))
    return None


def check(d, nvc, lib, fstdump, keep, timeout, quantile=0.5):
    work = tempfile.mkdtemp(prefix="ckpt-")
    env = dict(os.environ)
    env["NVC_LIBPATH"] = lib
    env["NVC_IMP_LIB"] = lib
    env["LANG"] = "en_US.UTF-8"
    env.update(d.env)
    res = {"name": d.name, "status": None, "detail": ""}

    try:
        base = [nvc, "--std=" + d.std]
        if d.heap:
            base.append("-H" + d.heap)

        rc, out, err, _ = run(base + ["-a"] + d.files + d.aflags
                              + ["-e", d.top, "-O%d" % d.olevel]
                              + ["-g%s=%s" % g for g in d.gens], work, env,
                              timeout)
        if rc != 0:
            raise NA("analysis or elaboration failed" if not d.expect_fail
                     else "expected failure before run")

        def runcmd(extra, stop=None):
            cmd = base + ["-r"]
            if stop:
                cmd.append("--stop-time=" + stop)
            cmd += extra + d.rflags + [d.top]
            return run(cmd, work, env, timeout)

        # 1. straight run
        s_rc, s_out, s_err, s_time = runcmd(["--wave=straight.fst"], d.stop)
        if s_rc is None:
            raise NA("straight run timed out")
        s_err = norm(s_err)
        s_out = norm(s_out)
        if not os.path.exists(os.path.join(work, "straight.fst")):
            raise NA("no waveform produced")

        dump = subprocess.run([fstdump, "straight.fst"], cwd=work,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL).stdout.decode(
                                  "utf-8", "replace")
        straight = parse_dump(dump)
        times = sorted(t for t in straight if t > 0)
        if len(times) < 2:
            # Few signal events: take the times of the time steps from the
            # kernel trace instead
            t_rc, t_out, t_err, _ = runcmd(["--trace"], d.stop)
            times = sorted(set(trace_times(t_err)) - {0})
        if len(times) < 2:
            raise NA("no simulated time elapses")
        tmid = times[int((len(times) - 1) * quantile)]
        res["time_fs"] = tmid
        res["straight_s"] = round(s_time, 3)

        # 2. checkpoint run, stopping at the checkpoint
        ck = os.path.join(work, "state.ckpt")
        a_rc, a_out, a_err, _ = runcmd(
            ["--checkpoint=%s@%dfs" % (ck, tmid), "--wave=a.fst"], "%dfs" % tmid)
        if a_rc is None:
            raise NA("checkpoint run timed out")

        if not os.path.exists(ck):
            m = re.search(rb"cannot write checkpoint [^\n]*? at [^:]*: ([^\n]*)",
                          a_err)
            res["status"] = "refused"
            res["detail"] = (m.group(1).decode() if m
                             else a_err.decode("utf-8", "replace")[-200:].strip())
            return res

        res["ckpt_bytes"] = os.path.getsize(ck)
        a_err = norm(a_err)
        a_out = norm(a_out)

        if not s_out.startswith(a_out):
            res["status"] = "different"
            res["detail"] = ("the output of the run that wrote the "
                             "checkpoint is not a prefix of the straight run")
            return res

        # 3. restore in a new process
        r_rc, r_out, r_err, r_time = runcmd(
            ["--restore=" + ck, "--wave=restored.fst"], d.stop)
        r_err = norm(r_err)
        r_out = norm(r_out)
        res["restore_s"] = round(r_time, 3) if r_rc is not None else None
        if r_rc is None:
            res["status"] = "different"
            res["detail"] = "restored run timed out"
            return res

        problems = []
        if r_rc != s_rc:
            problems.append("exit status %s, straight %s" % (r_rc, s_rc))
        if r_out != s_out[len(a_out):]:
            problems.append("stdout differs")
        why = stderr_diff(s_err, a_err, r_err)
        if why:
            problems.append(why)

        if not os.path.exists(os.path.join(work, "restored.fst")):
            problems.append("no waveform from the restored run")
        else:
            rdump = subprocess.run([fstdump, "restored.fst"], cwd=work,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL).stdout.decode(
                                       "utf-8", "replace")
            problems += waveform_diff(straight, parse_dump(rdump), tmid)

        if problems:
            res["status"] = "different"
            res["detail"] = "; ".join(problems[:4])
        else:
            res["status"] = "identical"
        return res

    except NA as e:
        res["status"] = "n/a"
        res["detail"] = str(e)
        return res
    finally:
        if keep:
            res["dir"] = work
        else:
            shutil.rmtree(work, ignore_errors=True)


# --------------------------------------------------------------------------
# Restoring into changed code

WRITTEN_RE = re.compile(rb"checkpoint \S+ written at (\d+)(fs|ps|ns|us|ms|sec)")


def elaborate(d, base, work, env, timeout):
    rc, out, err, _ = run(base + ["-a"] + d.files + d.aflags
                          + ["-e", d.top, "-O%d" % d.olevel]
                          + ["-g%s=%s" % g for g in d.gens], work, env, timeout)
    return rc == 0


def payload_of(path):
    """The state part of a checkpoint file: not the identity of the build or
    design, nor the record of which code had run"""
    import struct
    data = open(path, "rb").read()
    if data[:8] != b"NVCCKPT1":
        return None
    off = 12
    vlen = struct.unpack_from("<I", data, off)[0]
    off += 4 + vlen + 24
    length = struct.unpack_from("<Q", data, off)[0]
    off += 16
    return data[off:off + length]


def check_fix(before, after, nvc, lib, fstdump, keep, timeout, quantiles=(0.5,)):
    """Checkpoint `before` at each position, restore into `after` and compare
    with `after` run from time zero.  The verdict is the one nvc printed.
    For an exact verdict the state of the restored simulation is also
    compared with the state `after` has at that time when run from zero."""
    work = tempfile.mkdtemp(prefix="ckfix-")
    wb, wa = os.path.join(work, "b"), os.path.join(work, "a")
    os.makedirs(wb)
    os.makedirs(wa)
    env = dict(os.environ)
    env["NVC_LIBPATH"] = lib
    env["NVC_IMP_LIB"] = lib
    env["LANG"] = "en_US.UTF-8"
    results = []

    def na(why):
        return [{"name": after.name, "status": "n/a", "detail": why,
                 "verdict": None}]

    try:
        base = [nvc, "--std=" + after.std]

        def runin(d, dirname, extra, stop):
            cmd = base + ["-r"]
            if stop:
                cmd.append("--stop-time=" + stop)
            cmd += extra + [d.top]
            return run(cmd, dirname, env, timeout)

        if not elaborate(before, base, wb, env, timeout) \
                or not elaborate(after, base, wa, env, timeout):
            return na("analysis or elaboration failed")

        b_rc, b_out, b_err, _ = runin(before, wb, ["--wave=b.fst"], before.stop)
        if b_rc is None or not os.path.exists(os.path.join(wb, "b.fst")):
            return na("the design before the fix did not run")
        dump = subprocess.run([fstdump, "b.fst"], cwd=wb, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL).stdout.decode(
                                  "utf-8", "replace")
        times = sorted(t for t in parse_dump(dump) if t > 0)
        if len(times) < 2:
            return na("no simulated time elapses")

        # The fixed design from time zero is the reference
        s_rc, s_out, s_err, _ = runin(after, wa, ["--wave=s.fst"], after.stop)
        if s_rc is None or not os.path.exists(os.path.join(wa, "s.fst")):
            return na("the design after the fix did not run")
        s_err, s_out = norm(s_err), norm(s_out)
        sdump = subprocess.run([fstdump, "s.fst"], cwd=wa, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL).stdout.decode(
                                   "utf-8", "replace")
        straight = parse_dump(sdump)

        for q in quantiles:
            res = {"name": "%s@%g" % (after.name, q), "status": None,
                   "detail": "", "verdict": None, "state": None}
            results.append(res)
            t = times[int((len(times) - 1) * q)]
            res["time_fs"] = t

            ck = os.path.join(wb, "state.ckpt")
            if os.path.exists(ck):
                os.unlink(ck)
            a_rc, a_out, a_err, _ = runin(
                before, wb, ["--checkpoint=%s@%dfs" % (ck, t), "--wave=c.fst"],
                "%dfs" % t)
            m = WRITTEN_RE.search(a_err)
            if not os.path.exists(ck) or m is None:
                res["status"] = "refused"
                mm = re.search(rb"cannot write checkpoint [^\n]*? at [^:]*: ([^\n]*)",
                               a_err)
                res["detail"] = mm.group(1).decode() if mm else "no checkpoint"
                continue
            tc = int(m.group(1)) * UNITS[m.group(2)]

            p_rc, p_out, p_err, _ = runin(after, wa, ["--wave=p.fst"],
                                          "%dfs" % tc)
            p_err, p_out = norm(p_err), norm(p_out)

            verdict = os.path.join(wa, "verdict.json")
            resave = os.path.join(wa, "resave.ckpt")
            for f in (verdict, resave):
                if os.path.exists(f):
                    os.unlink(f)
            r_rc, r_out, r_err, _ = runin(
                after, wa, ["--restore=" + ck, "--allow-changes",
                            "--verdict=" + verdict, "--resave=" + resave,
                            "--wave=r.fst"], after.stop)
            if r_rc is None:
                res["status"] = "different"
                res["detail"] = "restored run timed out"
                continue
            if b"cannot restore checkpoint" in r_err:
                mm = re.search(rb"cannot restore checkpoint: ([^\n]*)", r_err)
                res["status"] = "refused"
                res["detail"] = mm.group(1).decode() if mm else "?"
                continue

            if not os.path.exists(verdict):
                res["status"] = "error"
                res["detail"] = ("no verdict: " + r_err.decode(
                    "utf-8", "replace")[-200:]).strip()
                continue

            v = json.load(open(verdict))
            res["verdict"] = v["verdict"]
            res["verdict_old"] = v.get("verdict_without_mappings")
            res["changed"] = len(v["changed_units"])
            res["ran"] = sum(1 for u in v["changed_units"]
                             if u["executed_before_checkpoint"])
            res["mapped"] = sum(1 for u in v["changed_units"]
                                if u.get("equivalent_by_mapping"))

            r_err, r_out = norm(r_err), norm(r_out)
            problems = []
            if r_rc != s_rc:
                problems.append("exit status %s, reference %s" % (r_rc, s_rc))
            if not s_out.startswith(p_out):
                problems.append("reference output is not a prefix")
            elif r_out != s_out[len(p_out):]:
                problems.append("stdout differs")
            why = stderr_diff(s_err, p_err, r_err)
            if why:
                problems.append(why)
            if not os.path.exists(os.path.join(wa, "r.fst")):
                problems.append("no waveform from the restored run")
            else:
                rdump = subprocess.run([fstdump, "r.fst"], cwd=wa,
                                       stdout=subprocess.PIPE,
                                       stderr=subprocess.DEVNULL).stdout.decode(
                                           "utf-8", "replace")
                problems += waveform_diff(straight, parse_dump(rdump), tc)

            # The claim behind "exact" is equality of state at the
            # checkpoint: the fixed design run from zero to the same time
            # saves what the restored simulation saves right after restoring
            if v["verdict"] == "exact":
                ref = os.path.join(wa, "ref.ckpt")
                if os.path.exists(ref):
                    os.unlink(ref)
                # Waveform dumping is on in the other runs and it can change
                # delta cycle numbers, so it is on here too
                runin(after, wa, ["--checkpoint=%s@%dfs" % (ref, tc),
                                  "--wave=ref.fst"], "%dfs" % tc)
                if not os.path.exists(ref) or not os.path.exists(resave):
                    res["state"] = "unavailable"
                else:
                    a1, a2 = payload_of(ref), payload_of(resave)
                    res["state"] = "same" if a1 == a2 and a1 else "differs"
                    if res["state"] == "differs":
                        problems.append("state at the checkpoint differs from "
                                        "the fixed design run from zero")

            res["status"] = "different" if problems else "identical"
            res["detail"] = "; ".join(problems[:3])
        return results
    finally:
        if keep:
            for r in results:
                r["dir"] = work
        else:
            shutil.rmtree(work, ignore_errors=True)


def ck_files(d):
    out = {}
    if os.path.isdir(d):
        for f in sorted(os.listdir(d)):
            m = re.match(r"ck-(\d+)\.ckpt$", f)
            if m:
                out[int(m.group(1))] = os.path.join(d, f)
    return out


def check_confirm(before, after, nvc, lib, fstdump, timeout, jobs=2):
    """Confirmation by segments against the truth: the changed design run
    from zero with checkpoints at the same interval, compared with those of
    the original run file by file"""
    work = tempfile.mkdtemp(prefix="ckconf-")
    wb, wa = os.path.join(work, "b"), os.path.join(work, "a")
    os.makedirs(wb)
    os.makedirs(wa)
    env = dict(os.environ)
    env["NVC_LIBPATH"] = lib
    env["NVC_IMP_LIB"] = lib
    res = {"name": after.name, "confirm": None, "truth": None, "ok": True,
           "detail": ""}
    try:
        base = [nvc, "--std=" + after.std]
        if not elaborate(before, base, wb, env, timeout) \
                or not elaborate(after, base, wa, env, timeout):
            res["confirm"] = "n/a"
            return res

        b_rc, _, _, _ = run(base + ["-r", "--wave=b.fst"] +
                            (["--stop-time=" + before.stop] if before.stop
                             else []) + [before.top], wb, env, timeout)
        dump = subprocess.run([fstdump, "b.fst"], cwd=wb, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL).stdout.decode(
                                  "utf-8", "replace") \
            if os.path.exists(os.path.join(wb, "b.fst")) else ""
        times = sorted(t for t in parse_dump(dump) if t > 0)
        if len(times) < 8:
            res["confirm"] = "n/a"
            return res
        every = max(times[-1] // 8 // 1000000 * 1000000, 1000000)

        stop = (["--stop-time=" + before.stop] if before.stop else [])
        # The original run may record waveforms: the saved state must not
        # depend on it
        wv = ["--wave=orig.fst"] if os.environ.get("HARNESS_CONFIRM_WAVES") \
            else []
        run(base + ["-r"] + stop + wv
            + ["--checkpoint=%s@every=%dfs,max=100000"
               % (os.path.join(work, "ckb"), every), before.top],
            wb, env, timeout)
        fb = ck_files(os.path.join(work, "ckb"))
        if len(fb) < 2:
            res["confirm"] = "n/a"
            return res

        # Truth: the first checkpoint of the original that the changed design
        # does not reproduce when run from zero to exactly that time (one run
        # per checkpoint, so a changed design with events at other times is
        # still compared at the right one)
        first = None
        for i, (t, f) in enumerate(sorted(fb.items())):
            one = os.path.join(work, "one%d.ckpt" % i)
            run(base + ["-r", "--stop-time=%dfs" % t,
                        "--checkpoint=%s@%dfs" % (one, t), after.top],
                wa, env, timeout)
            if not os.path.exists(one) or payload_of(f) != payload_of(one):
                first = i
                break
        res["truth"] = "same" if first is None else "differs@%d" % (first + 1)

        cr = os.path.join(work, "res.json")
        rc, out, err, secs = run(
            base + ["-r", "--confirm=" + os.path.join(work, "ckb"),
                    "--jobs=%d" % jobs, "--confirm-result=" + cr, after.top],
            wa, env, timeout)
        if not os.path.exists(cr):
            res["confirm"] = "error"
            res["ok"] = False
            res["detail"] = err.decode("utf-8", "replace")[-200:]
            return res
        r = json.load(open(cr))
        res["confirm"] = r["result"]
        res["wall_s"] = r["wall_s"]
        if r["result"] == "incomplete":
            res["why"] = next((x["detail"] for x in r["segment_results"]
                               if x["state"] not in ("same", "different")),
                              "") or "budget or not run"
        if r["result"] == "confirmed" and first is not None:
            res["ok"] = False
            res["detail"] = "confirmed but the reference differs at checkpoint %d" \
                % (first + 1)
        elif r["result"] == "different":
            seg = r["first_difference"]["segment"]
            if first is None:
                res["ok"] = False
                res["detail"] = "different but the reference is the same"
            elif seg != first + 1:
                res["ok"] = False
                res["detail"] = "different in segment %d, reference first " \
                    "differs at checkpoint %d" % (seg, first + 1)
        elif r["result"] == "incomplete" and first is None:
            res["detail"] = "not confirmed although the reference is the same"
        return res
    finally:
        if os.environ.get("CONFIRM_KEEP") and not res["ok"]:
            print("kept", work, after.name, flush=True)
        else:
            shutil.rmtree(work, ignore_errors=True)


CONF_LINE_RE = re.compile(rb"^confirm: [^\n]*\n", re.M)


def check_rc(before, after, nvc, lib, fstdump, timeout, waves=False):
    """The strong oracle for restoring from the last confirmed checkpoint:
    the original run writes checkpoints, the changed design is restored from
    the last one that the confirmation proves to be its own state and run on,
    and what it does from there on is compared with the changed design run
    from zero: output, waveform and the state at a late time"""
    work = tempfile.mkdtemp(prefix="ckrc-")
    wb, wa = os.path.join(work, "b"), os.path.join(work, "a")
    os.makedirs(wb)
    os.makedirs(wa)
    env = dict(os.environ)
    env["NVC_LIBPATH"] = lib
    env["NVC_IMP_LIB"] = lib
    res = {"name": after.name, "status": None, "detail": "", "k": None,
           "n": None, "frac": None}
    try:
        base = [nvc, "--std=" + after.std]
        if not elaborate(before, base, wb, env, timeout) \
                or not elaborate(after, base, wa, env, timeout):
            res["status"] = "n/a"
            return res
        stop = (["--stop-time=" + before.stop] if before.stop else [])

        rc, _, _, _ = run(base + ["-r", "--wave=b.fst"] + stop + [before.top],
                          wb, env, timeout)
        if not os.path.exists(os.path.join(wb, "b.fst")):
            res["status"] = "n/a"
            return res
        times = sorted(t for t in parse_dump(subprocess.run(
            [fstdump, "b.fst"], cwd=wb, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL).stdout.decode("utf-8", "replace"))
            if t > 0)
        if len(times) < 8:
            res["status"] = "n/a"
            return res
        every = max(times[-1] // 8 // 1000000 * 1000000, 1000000)
        ck = os.path.join(work, "ckb")
        wv = ["--wave=orig.fst"] if waves else []
        run(base + ["-r"] + stop + wv + ["--checkpoint=%s@every=%dfs,max=100000"
                                         % (ck, every), before.top],
            wb, env, timeout)
        if len(ck_files(ck)) < 2:
            res["status"] = "n/a"
            return res

        # The changed design from zero, with waveform, output and a late state
        late = times[-1]
        rc_s, s_out, s_err, _ = run(
            base + ["-r", "--wave=ref.fst", "--checkpoint=ref.ckpt@%dfs" % late]
            + stop + [after.top], wa, env, timeout)
        if rc_s is None or not os.path.exists(os.path.join(wa, "ref.fst")):
            res["status"] = "n/a"
            return res
        sdump = parse_dump(subprocess.run(
            [fstdump, "ref.fst"], cwd=wa, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL).stdout.decode("utf-8", "replace"))

        vj = os.path.join(work, "v.json")
        def restored(with_ckpt):
            return run(
                base + ["-r", "--wave=res.fst", "--restore-confirmed=" + ck,
                        "--confirmed-or-zero", "--verdict=" + vj]
                + (["--checkpoint=res.ckpt@%dfs" % late] if with_ckpt else [])
                + stop + [after.top], wa, env, timeout)

        rc_r, r_out, r_err, secs = restored(True)
        if rc_r not in (None, rc_s) and os.path.exists(vj) \
                and json.load(open(vj))["confirmation"]["checkpoint_fs"] >= late:
            # Restored at or after the time of the late state
            rc_r, r_out, r_err, secs = restored(False)
        r_err = CONF_LINE_RE.sub(b"", r_err)
        if rc_r is None:
            res["status"] = "different"
            res["detail"] = "restored run timed out"
            return res

        t_rest = 0
        res["k"] = 0
        if os.path.exists(vj):
            v = json.load(open(vj))
            if v.get("verdict") != "exact" or not v.get("state_verified"):
                res["status"] = "different"
                res["detail"] = "verdict %s" % v.get("verdict")
                return res
            c = v["confirmation"]
            t_rest = c["checkpoint_fs"]
            res["k"], res["n"] = c["segments_confirmed"], c["segments"]
        res["frac"] = t_rest / float(late) if late else 0.0

        # What the changed design printed before that time
        a_err = b""
        if t_rest > 0:
            _, _, a_err, _ = run(base + ["-r", "--stop-time=%dfs" % t_rest,
                                         after.top], wa, env, timeout)
            _, a_out, _, _ = run(base + ["-r", "--stop-time=%dfs" % t_rest,
                                         after.top], wa, env, timeout)
        else:
            a_out = b""

        problems = []
        if rc_r != rc_s:
            problems.append("exit status %s, reference %s" % (rc_r, rc_s))
        if norm(r_out) != norm(s_out)[len(norm(a_out)):]:
            problems.append("stdout differs")
        why = stderr_diff(norm(s_err), norm(a_err), norm(r_err))
        if why:
            problems.append(why)
        if t_rest > 0 or os.path.exists(os.path.join(wa, "res.fst")):
            rdump = parse_dump(subprocess.run(
                [fstdump, "res.fst"], cwd=wa, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL).stdout.decode("utf-8", "replace"))
            problems += waveform_diff(sdump, rdump, t_rest)
        rf, sf = os.path.join(wa, "res.ckpt"), os.path.join(wa, "ref.ckpt")
        if os.path.exists(sf) and t_rest < late:
            if not os.path.exists(rf):
                problems.append("no state saved at the late time")
            elif payload_of(rf) != payload_of(sf):
                problems.append("state at the late time differs")
        res["status"] = "different" if problems else "identical"
        res["detail"] = "; ".join(problems[:3])
        return res
    finally:
        if os.environ.get("CONFIRM_KEEP") and res["status"] == "different":
            print("kept", work, after.name, flush=True)
        else:
            shutil.rmtree(work, ignore_errors=True)


def rc_summary(results):
    print("\nrestore from the last confirmed checkpoint, against the changed "
          "design run from zero:")
    cnt = collections.Counter(r["status"] for r in results)
    print("  identical %d  different %d  n/a %d" % (cnt["identical"],
          cnt["different"], cnt["n/a"]))
    ok = [r for r in results if r["status"] != "n/a"]
    zero = [r for r in ok if not r["k"]]
    print("  restarted from zero: %d; from a later checkpoint: %d"
          % (len(zero), len(ok) - len(zero)))
    bins = collections.Counter()
    for r in ok:
        if r["k"]:
            bins[min(int(r["frac"] * 10), 9)] += 1
    print("  how far into the run (tenths of the run): " + ", ".join(
        "%d-%d%%: %d" % (b * 10, b * 10 + 10, bins[b]) for b in sorted(bins)))
    if ok:
        print("  mean fraction saved: %.2f" % (sum(r["frac"] or 0 for r in ok)
                                              / len(ok)))
    bad = [r for r in results if r["status"] == "different"]
    for r in bad[:10]:
        print("RESTORE-CONFIRMED DIFFERS %s: %s" % (r["name"], r["detail"]))
    return len(bad)


def confirm_summary(results):
    table = collections.Counter((r["confirm"], r["truth"] and
                                 ("same" if r["truth"] == "same" else "differs"))
                                for r in results)
    print("\nconfirmation by segments against the reference:")
    for (c, t), n in sorted(table.items(), key=str):
        print("  %-11s reference %-8s %d" % (c, t, n))
    why = collections.Counter(re.sub(r"/\S+: |:\S+ |[0-9]+", "", r["why"])[:150]
                              for r in results if r.get("why"))
    for w, n in why.most_common():
        print("  incomplete because: %3d  %s" % (n, w))
    bad = [r for r in results if not r["ok"]]
    # A difference found earlier than the reference has it is the known
    # limit of comparing bytes (design note 12.2): conservative, not wrong
    early = [r for r in bad if r["detail"].startswith("different in segment")]
    wrong = [r for r in bad if r not in early]
    for r in wrong[:10]:
        print("CONFIRM DISAGREES %s: %s" % (r["name"], r["detail"]))
    for r in early[:10]:
        print("confirm reports a difference earlier than the reference "
              "%s: %s" % (r["name"], r["detail"]))
    print("confirmation disagreements with the reference: %d "
          "(+ %d differences reported earlier than the reference)"
          % (len(wrong), len(early)))
    return len(wrong)


def write_sources(root, files):
    os.makedirs(root, exist_ok=True)
    out = []
    for name, text in files:
        path = os.path.join(root, name + ".vhd")
        with open(path, "w") as f:
            f.write(text)
        out.append(path)
    return out


def bench_pairs(tmp):
    """(before, after) for every bug of test/mcp/bench/bench.py"""
    sys.path.insert(0, os.path.join(ROOT, "test", "mcp", "bench"))
    import bench
    pairs = []
    for bug in list(bench.BUGS) + list(bench.SOC_BUGS):
        d = []
        for buggy in (True, False):
            root = os.path.join(tmp, bug, "before" if buggy else "after")
            if bug in bench.SOC_BUGS:
                files = write_sources(root, bench.soc_sources(bug, buggy))
                top, stop = "tb_soc", bench.SOC_STOP
            else:
                design, src, tb, tbsrc = bench.sources(bug, buggy)
                files = write_sources(root, [(design, src), (tb, tbsrc)])
                top, stop = tb, bench.BUGS[bug][3]
            dd = Design(bug, files, top, std="2008", stop=stop)
            d.append(dd)
        pairs.append((d[0], d[1]))
    return pairs


def own_pairs():
    """Pairs in test/checkpoint/pairs/NAME/{before,after}.  Each directory
    holds the files in alphabetical order and a `pair.txt` giving the top
    unit, the stop time and the standard on separate `key = value` lines."""
    base = os.path.join(HERE, "pairs")
    pairs = []
    if not os.path.isdir(base):
        return pairs
    for name in sorted(os.listdir(base)):
        cfg = {"std": "2008", "stop": None}
        for line in open(os.path.join(base, name, "pair.txt")):
            if "=" in line:
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
        ds = []
        for side in ("before", "after"):
            dirn = os.path.join(base, name, side)
            files = [os.path.join(dirn, f) for f in sorted(os.listdir(dirn))
                     if f.endswith(".vhd")]
            ds.append(Design(name, files, cfg["top"], std=cfg["std"],
                             stop=cfg["stop"]))
        pairs.append((ds[0], ds[1]))
    return pairs


def tally(results, table):
    for r in results:
        table[(r["verdict"] or r["status"], r["status"], r.get("state"),
               r.get("verdict_old"))] += 1


def print_table(table):
    print("\nverdict x result (restore from the design before the change):")
    rows = collections.Counter()
    for (verdict, status, state, old), n in table.items():
        rows[(verdict, status)] += n
    for verdict in ("exact", "approximate", "refused", "n/a"):
        row = {k[1]: v for k, v in rows.items() if k[0] == verdict}
        if row:
            print("  %-12s %s" % (verdict, ", ".join(
                "%s %d" % (k, v) for k, v in sorted(row.items()))))
    states = collections.Counter()
    for (verdict, status, state, old), n in table.items():
        if verdict == "exact":
            states[state] += n
    print("exact: state at the checkpoint compared with the fixed design "
          "from zero: %s" % (dict(states) or "none"))
    new = sum(n for (v, st, sta, old), n in table.items() if v == "exact")
    prev = sum(n for (v, st, sta, old), n in table.items()
               if old == "exact" and v is not None)
    both = sum(n for (v, st, sta, old), n in table.items()
               if v == "exact" and old == "exact")
    print("exact verdicts: %d with mappings, %d by the rule without them "
          "(%d by both)" % (new, prev, both))
    appr_id = sum(n for (v, st, sta, old), n in table.items()
                  if v == "approximate" and st == "identical")
    appr = sum(n for (v, st, sta, old), n in table.items()
               if v == "approximate")
    print("approximate but identical afterwards: %d of %d approximate"
          % (appr_id, appr))
    bad = sum(n for (v, st, sta, old), n in table.items()
              if v == "exact" and (st == "different" or sta == "differs"))
    print("exact and different (output, waves or state): %d" % bad)
    unavail = sum(n for (v, st, sta, old), n in table.items()
                  if v == "exact" and sta == "unavailable")
    print("exact with no state comparison possible: %d" % unavail)
    return bad


# --------------------------------------------------------------------------
# Mutants

COND_RE = re.compile(r"^\s*(if|elsif|when|while|assert|exit when|next when)\b|\bwhen\b.*\belse\b")
ASSIGN_RE = re.compile(r"^(\s*)[A-Za-z_][\w.()'\s,]*?\s*(<=|:=)[^;]*;\s*$")
SKIP_RE = re.compile(r"^\s*(--|library|use|entity|architecture|end|package|"
                     r"generic|port|constant|type|subtype|function|procedure|"
                     r"signal|variable|component|begin|is\b)", re.I)
NUM_RE = re.compile(r"(?<![\w.'#\"])(\d+)(?![\w.#\"'])(?!\s*(fs|ps|ns|us|ms|sec)\b)")


def line_mutations(line):
    """All the single changes that can be made to a line, as (name, text)"""
    out = []
    if SKIP_RE.match(line):
        return out
    m = ASSIGN_RE.match(line)
    if m and "<=" not in line.split(":=")[0].replace("<=", "<=", 1) or m:
        out.append(("delete", m.group(1) + "null;" if m else line))
        out.append(("duplicate", line + "\n" + line))
    cond = bool(COND_RE.match(line))
    for name, old, new in (("plus", " + ", " - "), ("minus", " - ", " + "),
                           ("times", " * ", " + "), ("and", " and ", " or "),
                           ("or", " or ", " and ")):
        if old in line:
            out.append((name, line.replace(old, new, 1)))
    if cond:
        for name, old, new in (("eq", " = ", " /= "), ("neq", " /= ", " = "),
                               ("lt", " < ", " > "), ("gt", " > ", " < "),
                               ("le", " <= ", " >= "), ("ge", " >= ", " <= ")):
            if old in line:
                out.append((name, line.replace(old, new, 1)))
    for mm in NUM_RE.finditer(line):
        v = int(mm.group(1))
        out.append(("const", line[:mm.start()] + str(v + 1) + line[mm.end():]))
        break
    mm = re.search(r"wait for (\d+) (ns|us)", line)
    if mm:
        out.append(("wait", line[:mm.start(1)] + str(int(mm.group(1)) * 2 + 1)
                    + line[mm.end(1):]))
    return out


def mutants(files, rng, count):
    """(description, {file: new text}) for count single mutations"""
    sites = []
    texts = {f: open(f).read().split("\n") for f in files}
    for f, lines in texts.items():
        for i, line in enumerate(lines):
            for name, new in line_mutations(line):
                sites.append((f, i, name, new))
    rng.shuffle(sites)
    return [("%s:%d:%s" % (os.path.basename(f), i + 1, name),
             {f: "\n".join(texts[f][:i] + new.split("\n")
                           + texts[f][i + 1:])})
            for f, i, name, new in sites[:count]]


def mutation_bases():
    b = os.path.join(ROOT, "test", "mcp", "bench")
    bases = []
    for dut, tb, top, stop in BENCH:
        bases.append(("m_" + dut, [os.path.join(b, dut + ".vhd"),
                                   os.path.join(b, tb + ".vhd")], top, stop, 1))
    bases.append(("m_soc", [os.path.join(b, "soc", f + ".vhd")
                            for f in SOC_FILES], "tb_soc", "3ms", 0.25))
    xb = os.path.join(HERE, "pairs", "x_c0", "before", "t.vhd")
    bases.append(("m_x", [xb], "t", "1200ns", 2))
    return bases


def mutation_task(task):
    (name, files, top, stop, desc, repl, nvc, lib, fstdump, timeout,
     quantiles, root, confirm) = task
    tmp = tempfile.mkdtemp(prefix="ckmut-")
    try:
        after_files = []
        for f in files:
            text = repl.get(f)
            if text is None:
                after_files.append(f)
                continue
            dst = os.path.join(tmp, os.path.basename(os.path.dirname(f))
                               + "_" + os.path.basename(f))
            open(dst, "w").write(text)
            after_files.append(dst)
        before = Design(name, files, top, std="2008", stop=stop)
        after = Design(name + "/" + desc, after_files, top, std="2008",
                       stop=stop)
        res = check_fix(before, after, nvc, lib, fstdump, False, timeout,
                        quantiles)
        for r in res:
            r["name"] = "%s/%s%s" % (name, desc, r["name"][r["name"].rfind("@"):])
        if confirm:
            c = check_confirm(before, after, nvc, lib, fstdump, timeout, 2)
            c["name"] = "%s/%s" % (name, desc)
            res.append({"name": c["name"], "status": "confirm", "confirm": c,
                        "verdict": None})
        if os.environ.get("HARNESS_RC"):
            c = check_rc(before, after, nvc, lib, fstdump, timeout,
                         bool(os.environ.get("HARNESS_CONFIRM_WAVES")))
            c["name"] = "%s/%s" % (name, desc)
            res.append({"name": c["name"], "status": "rc", "rc": c,
                        "verdict": None})
        return res
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def mutate_main(args, nvc, lib, fstdump):
    import random
    import multiprocessing
    rng = random.Random(args.seed)
    qs = [float(x) for x in args.at.split(",")]
    tasks = []
    for name, files, top, stop, weight in mutation_bases():
        for desc, repl in mutants(files, rng, int(args.count * weight)):
            tasks.append((name, files, top, stop, desc, repl, nvc, lib,
                          fstdump, args.timeout, qs, ROOT, args.confirm))

    table = collections.Counter()
    results = []
    cresults = []
    rcresults = []
    with multiprocessing.Pool(args.jobs) as pool:
        for res in pool.imap_unordered(mutation_task, tasks):
            for r in res:
                if r["status"] == "confirm":
                    cresults.append(r["confirm"])
                    continue
                if r["status"] == "rc":
                    rcresults.append(r["rc"])
                    continue
                results.append(r)
                tally([r], table)
                if r["status"] == "different" and r["verdict"] == "exact":
                    print("EXACT BUT DIFFERENT %s: %s" % (r["name"],
                                                          r["detail"][:200]),
                          flush=True)
    print("mutants tried: %d, checks: %d" % (len(tasks), len(results)))
    bad = print_table(table)
    if args.confirm:
        bad += confirm_summary(cresults)
    if args.restore_confirmed:
        bad += rc_summary(rcresults)
    if args.json:
        json.dump(results, open(args.json, "w"), indent=1)
    return 1 if bad else 0


def fix_main(args, nvc, lib, fstdump):
    tmp = tempfile.mkdtemp(prefix="ckpairs-")
    pairs = own_pairs()
    if args.pairs in ("bench", "all"):
        pairs = bench_pairs(tmp) + pairs
    if args.names:
        pairs = [p for p in pairs if p[0].name in args.names]

    qs = [float(x) for x in args.at.split(",")]
    table = collections.Counter()
    results = []
    for before, after in pairs:
        for r in check_fix(before, after, nvc, lib, fstdump, args.keep,
                           args.timeout, qs):
            results.append(r)
            tally([r], table)
            if args.verbose or r["status"] == "different":
                print("%-28s %-12s %-10s %s" % (r["name"], r["verdict"] or "-",
                                                r["status"], r["detail"][:140]),
                      flush=True)

    bad = print_table(table)
    if args.confirm:
        cres = [check_confirm(b, a, nvc, lib, fstdump, args.timeout)
                for b, a in pairs]
        bad += confirm_summary(cres)
    if args.restore_confirmed:
        rres = [check_rc(b, a, nvc, lib, fstdump, args.timeout,
                         bool(os.environ.get("HARNESS_CONFIRM_WAVES")))
                for b, a in pairs]
        bad += rc_summary(rres)
    if args.json:
        json.dump(results, open(args.json, "w"), indent=1)
    return 1 if bad else 0


def waves_check(d, nvc, lib, fstdump, keep, timeout):
    """Periodic checkpoints of the same run without a dumper, with an FST
    dump and with a VCD dump must have byte-equal states"""
    work = tempfile.mkdtemp(prefix="ckwave-")
    env = dict(os.environ)
    env["NVC_LIBPATH"] = lib
    env["NVC_IMP_LIB"] = lib
    env["LANG"] = "en_US.UTF-8"
    env.update(d.env)
    res = {"name": d.name, "status": None, "detail": ""}
    try:
        base = [nvc, "--std=" + d.std]
        if d.heap:
            base.append("-H" + d.heap)
        rc, _, _, _ = run(base + ["-a"] + d.files + d.aflags
                          + ["-e", d.top, "-O%d" % d.olevel]
                          + ["-g%s=%s" % g for g in d.gens], work, env, timeout)
        if rc != 0:
            raise NA("analysis or elaboration failed")

        def runcmd(extra):
            cmd = base + ["-r"] + (["--stop-time=" + d.stop] if d.stop else [])
            return run(cmd + extra + d.rflags + [d.top], work, env, timeout)

        rc, _, _, _ = runcmd(["--wave=t.fst"])
        if rc is None or not os.path.exists(os.path.join(work, "t.fst")):
            raise NA("no waveform produced")
        dump = subprocess.run([fstdump, "t.fst"], cwd=work,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL).stdout.decode(
                                  "utf-8", "replace")
        times = sorted(t for t in parse_dump(dump) if t > 0)
        if len(times) < 2:
            raise NA("no simulated time elapses")
        every = max(times[-1] // 6, 1)

        dirs = {}
        for tag, extra in (("plain", []), ("fst", ["--wave=a.fst"]),
                           ("vcd", ["--wave=a.vcd", "--format=vcd"])):
            dd = os.path.join(work, "ck-" + tag)
            rc, _, err, _ = runcmd(["--checkpoint=%s@every=%dfs,max=100000"
                                    % (dd, every)] + extra)
            if rc is None:
                raise NA("run timed out")
            dirs[tag] = ck_files(dd)
        if not dirs["plain"]:
            m = re.search(rb"cannot write checkpoint [^\n]*? at [^:]*: ([^\n]*)",
                          err)
            res["status"] = "refused"
            res["detail"] = (m.group(1).decode() if m
                             else err.decode("utf-8", "replace")[-200:].strip())
            return res

        problems = []
        for tag in ("fst", "vcd"):
            if sorted(dirs[tag]) != sorted(dirs["plain"]):
                problems.append("%s: checkpoints at other times" % tag)
                continue
            for t, f in sorted(dirs["plain"].items()):
                if payload_of(f) != payload_of(dirs[tag][t]):
                    problems.append("%s: state differs at %d" % (tag, t))
                    break
        res["status"] = "different" if problems else "identical"
        res["detail"] = "; ".join(problems)
        res["files"] = len(dirs["plain"])
        return res
    except NA as e:
        res["status"] = "n/a"
        res["detail"] = str(e)
        return res
    finally:
        if keep:
            res["dir"] = work
        else:
            shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="nvc checkpoint validation harness (see the comment at the top of the file)")
    ap.add_argument("mode", choices=["regress", "bench", "design", "fix", "mutate", "waves"])
    ap.add_argument("--pairs", default="all", choices=["bench", "own", "all"],
                    help="for the fix mode: which pairs of designs")
    ap.add_argument("names", nargs="*")
    ap.add_argument("--nvc", default="bin/nvc")
    ap.add_argument("--lib", default="lib")
    ap.add_argument("--fstdump", default=None)
    ap.add_argument("--json")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--at", default="0.5",
                    help="comma separated positions in the run, as fractions "
                    "of its event times, at which to checkpoint (default 0.5)")
    ap.add_argument("--confirm", action="store_true",
                    help="fix and mutate modes: also run the confirmation by "
                    "segments and compare it with the reference")
    ap.add_argument("--restore-confirmed", action="store_true",
                    help="fix and mutate modes: restore the changed design "
                    "from the last confirmed checkpoint and compare the rest "
                    "of the run with the changed design run from zero")
    ap.add_argument("--confirm-waves", action="store_true",
                    help="with --confirm: the original run records waveforms")
    ap.add_argument("--count", type=int, default=40,
                    help="mutate mode: mutants per base design")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--top")
    ap.add_argument("--stop")
    ap.add_argument("--std", default="2008")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.confirm_waves:
        os.environ["HARNESS_CONFIRM_WAVES"] = "1"
    if args.restore_confirmed:
        os.environ["HARNESS_RC"] = "1"

    nvc = os.path.abspath(args.nvc)
    lib = os.path.abspath(args.lib)
    fstdump = args.fstdump or os.path.join(os.path.dirname(nvc), "fstdump")

    if args.mode == "fix":
        return fix_main(args, nvc, lib, fstdump)
    if args.mode == "mutate":
        return mutate_main(args, nvc, lib, fstdump)

    if args.mode == "waves":
        designs = bench_designs() + regress_designs(set(args.names))
        counts = collections.Counter()
        bad = 0
        for d in designs:
            if getattr(d, "skip", None):
                counts["n/a"] += 1
                continue
            r = waves_check(d, nvc, lib, fstdump, args.keep, args.timeout)
            counts[r["status"]] += 1
            if r["status"] == "different":
                bad += 1
                print("DIFFERENT %s: %s" % (r["name"], r["detail"]), flush=True)
        print("waves: %s" % dict(counts))
        return 1 if bad else 0

    if args.mode == "regress":
        designs = regress_designs(set(args.names))
    elif args.mode == "bench":
        designs = bench_designs()
    else:
        d = Design("design", [os.path.abspath(f) for f in args.names],
                   args.top, std=args.std, stop=args.stop)
        d.skip = None
        designs = [d]

    results = []
    counts = collections.Counter()
    reasons = collections.Counter()
    quantiles = [float(q) for q in args.at.split(",")]
    jobs = []
    for d in designs:
        if getattr(d, "skip", None):
            jobs.append((d, None))
        else:
            jobs += [(d, q) for q in quantiles]

    for d, q in jobs:
        if q is None:
            r = {"name": d.name, "status": "n/a", "detail": d.skip}
        else:
            r = check(d, nvc, lib, fstdump, args.keep, args.timeout, q)
            if len(quantiles) > 1:
                r["name"] = "%s@%g" % (d.name, q)
        results.append(r)
        counts[r["status"]] += 1
        if r["status"] == "refused":
            reasons[re.sub(r"[0-9]+", "N", re.sub(
                r"(process|instance|package|signal|variable|file|trigger) \S+",
                r"\1 X", r["detail"]))[:90]] += 1
        if args.verbose or r["status"] in ("different",):
            print("%-24s %-9s %s" % (r["name"], r["status"],
                                     r["detail"][:150]), flush=True)
        elif r["status"] != "n/a" and args.mode != "regress":
            print("%-24s %-9s %s" % (r["name"], r["status"],
                                     r["detail"][:150]), flush=True)

    total = sum(counts.values())
    applicable = total - counts["n/a"]
    print("\nchecks: %d  n/a: %d  applicable: %d" % (total, counts["n/a"],
                                                    applicable))
    acc = counts["identical"] + counts["different"]
    print("accepted (checkpoint written): %d   refused: %d"
          % (acc, counts["refused"]))
    print("  identical: %d   different: %d" % (counts["identical"],
                                               counts["different"]))
    if reasons:
        print("\nrefusal reasons:")
        for k, v in reasons.most_common(15):
            print("  %5d  %s" % (v, k))

    if args.json:
        json.dump(results, open(args.json, "w"), indent=1)

    return 1 if counts["different"] else 0


if __name__ == "__main__":
    sys.exit(main())
