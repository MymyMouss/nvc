# Saving and restoring a simulation: design note

Status: implemented for the subset below (Phase 2 of `CHECKPOINT-PLAN.md`).
`nvc -r --checkpoint=FILE@TIME` and `nvc -r --restore=FILE`.  Source:
`src/rt/checkpoint.c`, `src/rt/checkpoint.h`; support in `src/rt/model.c`
(`src/rt/model-priv.h` exposes the model internals), `src/rt/fileio.c`,
`src/jit/jit-irgen.c` (frame layout descriptors).  Validation:
`test/checkpoint/harness.py`, `test/checkpoint/inventory.py`,
`test/regress/ckpt1.sh`, `test/regress/ckpt2.sh`, `test/test_mcp.c`.

## 1. Principle

A checkpoint is a typed description of the dynamic state of a model, written
as values identified by name (signal path, process name, variable name,
trigger and file by name and kind), never by address.  A restore elaborates
and resets the same design as usual and then overwrites the dynamic state
from the file.  The objects that exist after reset (signals, processes,
drivers, triggers, frames) are matched by name; the nexus layout, which can
differ from the one after reset because nexuses are split lazily at run
time, is recreated with the model's own split code.

The restore is verified: the restored model is serialised again with the same
function that wrote the file, and the result must be byte for byte the file's
contents.  If it is not, the restore fails with the section and offset of the
first difference.  So a restore is either exact for everything the format
covers, or refused with a reason.  Anything outside the format that the
simulation depends on is the risk, and is listed in section 8.

## 2. The quiescent point

A model can be saved when it is between time steps.  In terms of `rt_model_t`
(`src/rt/model-priv.h`):

* `next_is_delta` is false: the cycle that just ran had no delta cycle to
  follow (`model_cycle` has run the `END_TIME_STEP` callbacks and the
  postponed processes).  `model_at_step_end()` tests this.
* `procq`, `next_procq`, `driverq`, `next_driverq`, `postponedq`,
  `triggerq`, `inactiveq`, `next_inactiveq`, `nonblockq` and `reschedq` are
  empty, and so are `driving_heap` and `effective_heap`.
* No process, trigger or transfer has `wakeable.pending` set, no nexus has
  `NET_F_PENDING`, no driver has `fastqueued` or `sigqueued`.
* Every key in `eventq_heap` is greater than `now`.
* `force_stop` is false and the JIT has no exit status.

`checkpoint_save` checks all of these and refuses otherwise, naming the one
that failed.  The state is then entirely: the future (`eventq_heap`), the
waiting registrations (nexus and trigger `pending` lists, which processes are
delayed), the drivers with their pending transactions, the signal values, the
variable frames and the resume state of every process, the event bookkeeping
(`last_event`, `active_delta`, `event_delta`, `trigger_epoch`, `iteration`),
the files and a few globals.

How it is reached: `--checkpoint=FILE@TIME` registers an `END_TIME_STEP`
phase callback (these run once, so it registers itself again until
`now >= TIME`) and saves there.  The MCP worker, which stops between cycles
and not between time steps, calls `model_step` until `model_at_step_end()`.
The checkpoint is therefore taken at the end of the first time step at or
after TIME and the delta count in the file is that of the last cycle.

## 3. What is saved and how it is restored

All sections are in the order of the file.  Names are written once to a
table and referred to by index, so the file contains strings and numbers only.

| Section | Content | Identified by | Restore |
|---|---|---|---|
| `META` | `now`, `iteration`, `trigger_epoch`, assertion counts per severity and enable mask | | set after everything else |
| `PKGS` | packages whose frames exist (they are initialised lazily) | unit name | `jit_link` each, so their frames and signals exist before matching |
| `FILE` | each open file: predefined (`STD_INPUT`, `STD_OUTPUT`) or user, mode, name, position, size; output files with their contents, input files with a hash | kind, mode, name | see 3.1 |
| `OBJS` | heap arrays and objects that frames point into: byte extents, with the header pointer of an access to an array zeroed | order of first reference in the canonical walk | allocated on the heap and rooted until linked |
| `TRIG` | each trigger: kind, argument count, function name, waiting list | creator process name plus creation count (`rt_trigger_t.owner`, `id`) | triggers are made by process reset; matched and their lists replaced |
| `LAYT` | each signal: size, nexus partition (offset, width) | signal path (scope path and identifier) | `model_split_nexus` for every saved boundary, then the result is compared with the saved partition |
| `SIGS` | per signal: flags, whether its events are cached, the value area (effective, last, driving or initial); per nexus: flags, `active_delta`, `event_delta`, `last_event`, rank, sources (driver: process name, current value and when, future transactions; port: input signal and offset), waiting list in order with empty slots | signal path, nexus offset, source index | values written in place; transactions rebuilt with the model's allocators; drivers made after reset are added |
| `PROC` | per process: delayed, state number, frame variables, and for a process suspended in procedures the frames of the chain | process name | frame variables written by name; chain frames recreated in a private TLAB |
| `FRMS` | frames of instances and packages | scope or unit name | variables written by name |
| `HEAP` | `eventq_heap` in array order: key, kind, target | process name; driver by signal, nexus offset and source index | `heap_append_raw`, which keeps the exact order |

The heap array order is saved as it is because nodes with equal keys are not
ordered by anything but history, and the order in which simultaneous events
are processed can change what is printed.  Waiting lists keep their empty
slots for the same reason: a new registration fills the first empty slot.
Value change callbacks added by tools (the waveform dumper) are left out of
the lists and do not move the other entries.

After the sections are applied: processes' `pending` flags and the deferred
queues from the reset are cleared, `now`, `iteration` and `trigger_epoch` are
set, the signals whose events are cached are put on `eventsigs`, and the
assertion counters are restored.

### 3.1 Files

Output files (write, append, read-write) are saved with their contents, so a
restore recreates the file as it was at the checkpoint whatever the original
run did to it afterwards, and the position.  Input files must be unchanged:
the saved hash is compared.  Standard output needs nothing.  Standard input
is refused if anything has been read from it (a peek counts: `endfile`).  A
file handle in a frame is replaced by the number of the file in the table,
and bound to a fresh handle on restore; a file the reset already opened
(declared in an architecture) is reused and repositioned.  Names are those
the design used, so the restore must run from the same directory (checked
when the design has user files open).

### 3.2 Packages, heap data, access values

Packages are initialised on first use, so the set to initialise is saved.
Frame variables that point at memory are classified by where the pointer
goes: into a frame (identified by frame name and offset), into constant data
of a compiled unit (unit name and offset), or into the heap or a process's
private TLAB (a copy of the referenced bytes is saved once per object, with
the references into it as offsets; two variables that alias the same array
still alias).  Only arrays and objects whose elements hold no pointers are
copied.  An access value to an array has a header with a pointer to the body;
the body can start later than the header (`READ` from a `line` moves it) so
the offset is saved.  Anything else is refused.

## 4. Frame layout descriptor

The JIT lays out a frame in `irgen_context_struct` (`src/jit/jit-irgen.c`).
For each non-temporary variable it now also records a typed descriptor,
`jit_vtype_t` (`src/jit/jit.h`): kind (integer, offset, real, vec2/4, array
with element and count, record with fields and offsets, unconstrained array
with element and dimensions, pointer, access, signal, context, file, trigger,
resolution), size and alignment.  `link_tab_t` (name and offset) became
`jit_var_t` (name, offset, type); `jit_get_framelay` returns the table with
the unit kind and header size.  Opaque pointees (protected types) have no
descriptor and are refused.

Two changes to frame contents make the layout self-describing for suspended
procedures: a procedure frame has an extra word after the state number that
holds the JIT handle of its unit (the only link from a frame to its code,
since a suspended procedure is found through a pointer in its caller), and
its pointer to a nested suspended procedure is cleared when the frame is made
(it was only written when one suspended, so it could hold garbage).

Variable names are used as keys.  Names made up by the compiler with a global
counter (`right24`, `RAM.ename3`) differ between processes with different
lowering order, so a lower-case final component is compared without its
digits and repeated names get an ordinal.  A hash of the frame layout (names,
offsets, types) is saved with every frame and a restore is refused if it
differs.

## 5. Identity of the design and the program

The file starts with: the nvc version string, a hash of the nvc executable,
a hash of every file in the work library, and a hash of the model just after
reset (signal paths and sizes, process names, the layouts of instance and
process frames).  The model hash is taken after reset on both sides.  A
mismatch is reported as "different build of nvc", "the work library has
changed" or "the elaborated design is not the one the checkpoint was written
from".  Libraries other than work (IEEE, STD) are not hashed individually:
they are covered by the executable hash only if built together.  A checksum
over the payload detects truncation and damage.

## 6. What is refused, and how it is detected

Detection is by inspection of the actual state, not by assumption, and the
message names the object.  At save:

* not at a quiescent point (section 2), with the reason;
* coverage enabled, `--shuffle`, PSL properties in any scope, Verilog scopes;
* a forcing or deposit source on a signal, a signal waited on with the
  `'active` attribute, a timeout callback in the event queue (added by a
  plugin or debugger);
* a process whose frame pointer to a nested procedure is not a frame of a
  procedure, or which is suspended in more than 64 levels;
* a frame variable that is: a file handle that is not open, an access value
  to an object with pointers in it or to something that was not allocated by
  `new`, a pointer or uarray data pointer that is in no frame, constant pool
  or heap object, a heap array whose elements hold pointers;
* a signal path or process name that is not unique;
* standard input that has been read, a file that is not a regular file or is
  over 256 MB;
* VHPI and VPI plugins are refused on the command line.

At restore: wrong magic, version, checksum, build, library or design; a
signal, process, trigger, source, frame or variable in the file that is not
in the model or differs in kind, size or layout; an input file that has
changed; and any difference found by the final comparison.

## 7. Inventory of the regression suite (baseline, before the code)

Measured with `NVC_CKPT_INVENTORY=file build/bin/run_regr` and
`test/checkpoint/inventory.py file --vhdl`: each `nvc -r` writes, at the end
of the run, one line per frame variable of every process, instance and
package with its type class and where its pointers go.  1231 runs of VHDL
designs (1496 including the Verilog ones; some tests run `nvc -r` several
times).

| | variables | runs |
|---|---|---|
| plain scalars, arrays and records of them | 7496 | 788 |
| references (signal, context, trigger) | 5299 | 769 |
| resolution values | 1397 | 251 |
| file | 384 | 192 |
| unconstrained array | 2266 | 252 |
| pointer | 370 | 86 |
| access | 137 | 58 |

Runs in which every frame variable is plain or a reference: 674 / 1231.
Runs in which every frame variable can be saved by value at the end of the
run (null, into a frame or into constant data for pointers; open files and
heap data excluded): 863 / 1231.  Blocking reasons for frame variables
(variables, runs): array data on the heap 1813, 148; open file 360, 182;
access to a heap object 96, 38; pointer 94, 53 (coverage counters);
pointer to the heap 56, 22.  Features outside the frames (runs): PSL 30,
force or deposit 27, `'active` 37, signal transfers 57, coverage 55,
processes suspended in a procedure at the end of the run 3, shuffle 1.  Runs
with neither a frame blocker nor a feature blocker: 761 / 1231.  Verilog
scopes: 265 of 1496.

This ordered the work: the files and the heap arrays (`line`, `string`
constants) came first because even `ieee.numeric_std` pulls in
`std.textio` and its two predefined files, then processes suspended in a
procedure (which testbenches do all the time), then access values.  The
inventory code now reports heap arrays, access values and files as saved by
copy, so rerunning it shows what is left (section 9).

## 8. Known gaps and risks

* The checkpoint covers the model and its frames.  Other process-wide state
  that a design could depend on is not saved: the random number generator
  used by `--shuffle` (refused), the state of foreign (C) functions and of
  VHPI/VPI plugins (refused with plugins), the order of JIT handles and
  compiled code (not observable).  Heap memory is saved only for the
  objects reachable from frame variables in the shapes described.
* Reset-time output is printed again by a restored run.
* A restore requires the same build of nvc (hash of the executable) and the
  same work library; this is stricter than strictly needed so that frame
  layouts and block numbers (the resume state of a process is a block
  number) cannot disagree.  Phase 3 will have to replace the block number
  with something stable across code changes.
* LLVM builds are not tested (the build here has no LLVM).  Frame layouts
  come from the same IR generator, so the descriptors should be correct,
  but compiled code that holds values in registers across a wait would not
  be covered by what is checked here.
* Files: output file contents are held in memory twice while saving.
* The final comparison uses the same code to read the state as to write
  it, so a state that the format does not cover and that differs between
  the two runs is not noticed.  The harness (next section) compares what the
  simulation does afterwards instead.

## 9. Validation

`test/checkpoint/harness.py` (documented at its top): for a design it runs
straight to the end with a waveform dump; picks event times at chosen
positions in the run; for each, runs with `--checkpoint=F@T --stop-time=T`
(whose output is exactly what the straight run printed up to T), restores
in a new process with a waveform dump and compares what the restored run
printed on stdout and stderr (reset-time diagnostics are separated out),
its exit status, and its waveform from T: the value of every signal at T
and every later change.  Results are `identical`, `different`, `refused`
(the message is kept) and `n/a`.  It ran on:

* `test/mcp/bench`: the ten small designs and the 23-file SoC;
* every VHDL and mixed test of `test/regress/testlist.txt` except those
  that need VHPI, plugins, Tcl, coverage, a work library option, or a
  waveform or gtkw option, or are `slow` in the interpreter.

Last run on the final tree: bench, 11 designs at 7 positions each, 77
checks, 77 accepted, 77 identical.  Regression suite at 3 positions per
design (0.1, 0.5, 0.9): 1521 designs, 4171 checks, 2833 n/a (879 designs
never advance simulated time and the rest are excluded kinds), 1338
applicable, 792 accepted and identical, 546 refused, 0 different.  Refusals:
PSL, Verilog, `'active`, forcing, heap arrays holding pointers, a few
access values to linked objects.  SoC checkpoint at 200 us: 225,600 bytes,
save 2-3 ms, restore 25 ms (identity hashing and verification included).
