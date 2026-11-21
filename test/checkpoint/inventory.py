#!/usr/bin/env python3
#
# Aggregate the frame inventory written by `nvc -r` when the environment
# variable NVC_CKPT_INVENTORY names a file (see checkpoint_inventory in
# src/rt/checkpoint.c).  Usage:
#
#   NVC_CKPT_INVENTORY=/tmp/inv.txt build/bin/run_regr
#   test/checkpoint/inventory.py /tmp/inv.txt [--vhdl]
#
# A run is "frame-clean" if every variable of every process, instance and
# package frame is of a plain type, a reference to a signal/context/trigger,
# or a pointer-like type whose value at the end of the run is null or points
# at constant data or into the frame itself.  The blocking reasons are
# counted per run (a run counts once per reason) and per variable.

import sys
from collections import Counter, defaultdict

runs = []
cur = None
for line in open(sys.argv[1]):
    f = line.rstrip("\n").split("\t")
    if f[0] == "BEGIN":
        cur = {"top": f[1], "feat": {}, "vars": [], "frames": 0}
        runs.append(cur)
    elif cur is None:
        continue
    elif f[0] == "FEAT":
        cur["feat"][f[1]] = int(f[2])
    elif f[0] == "FRAME":
        cur["frames"] += 1
    elif f[0] == "VAR":
        cur["vars"].append(f[1:])

if len(sys.argv) > 2 and sys.argv[2] == "--vhdl":
    runs = [r for r in runs if r["feat"].get("verilog_scopes", 0) == 0]
    print("(VHDL-only runs)")

nruns = len(runs)
print("runs:", nruns)

# Static classes: every variable by the most demanding leaf type
cls_vars = Counter()
cls_runs = defaultdict(set)
reason_vars = Counter()
reason_runs = defaultdict(set)
clean = 0
clean_static = 0
for i, r in enumerate(runs):
    bad = False
    bad_static = False
    for (kind, unit, var, ty, cls, run) in r["vars"]:
        cls_vars[cls] += 1
        cls_runs[cls].add(i)
        if cls in ("uarray", "pointer", "access", "file", "resolution"):
            bad_static = True
        if run != "ok":
            bad = True
            reason_vars[run] += 1
            reason_runs[run].add(i)
    clean += not bad
    clean_static += not bad_static

print("\nvariables by static class (count, runs containing one):")
for c in ("plain", "ref", "resolution", "file", "uarray", "pointer", "access"):
    print("  %-12s %8d vars %5d runs" % (c, cls_vars[c], len(cls_runs[c])))

print("\nruns where every frame variable is plain/ref only: %d / %d"
      % (clean_static, nruns))
print("runs where every frame variable can be saved by value at the end "
      "of the run: %d / %d" % (clean, nruns))

print("\nblocking runtime reasons for frame variables (vars, runs):")
for k, v in reason_vars.most_common():
    print("  %-28s %7d %5d" % (k, v, len(reason_runs[k])))

# Non-frame blockers, from the feature counters
print("\nnon-frame features (runs with count > 0):")
feats = Counter()
for r in runs:
    for k, v in r["feat"].items():
        if v > 0:
            feats[k] += 1
for k in ("verilog_scopes", "psl_properties", "pseudo_sources_force_deposit",
          "active_outputs", "signal_transfers", "processes_suspended_in_procedure",
          "processes_with_private_tlab", "coverage", "shuffle", "triggers",
          "multi_source_nexuses", "watches_registered"):
    print("  %-34s %5d" % (k, feats[k]))

blocked = 0
for r in runs:
    f = r["feat"]
    if any(f.get(k, 0) for k in ("verilog_scopes", "psl_properties",
                                 "pseudo_sources_force_deposit",
                                 "active_outputs", "signal_transfers",
                                 "coverage", "shuffle")):
        blocked += 1
print("\nruns with at least one non-frame blocker "
      "(verilog, psl, force/deposit, 'active, transfers, coverage): "
      "%d / %d" % (blocked, nruns))

both = 0
for i, r in enumerate(runs):
    f = r["feat"]
    nb = any(f.get(k, 0) for k in ("verilog_scopes", "psl_properties",
                                   "pseudo_sources_force_deposit",
                                   "active_outputs", "signal_transfers",
                                   "coverage", "shuffle"))
    bad = any(v[5] != "ok" for v in r["vars"])
    both += (not nb and not bad)
print("runs with neither frame nor non-frame blockers: %d / %d" % (both, nruns))
