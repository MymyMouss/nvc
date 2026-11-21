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

## 10. Phase 3: restoring into a changed design

`nvc -r --restore=FILE --allow-changes [--verdict=FILE]`.  Without the
option a changed work library is refused and the message names the option.
The nvc executable must still be the same.

### 10.1 What is compared: the unit

The unit of comparison is a compiled JIT function, found by name: a process,
an instance, a package, a subprogram.  Its fingerprint is not taken from the
source or the tree but from the code generated for it:

* a hash of each instruction (opcode, size, condition, registers, immediate
  operands, displacement only in addresses, labels as instruction numbers),
  with callees replaced by their names, because handle numbers depend on the
  order of compilation, and with source locations, loci and `J_DEBUG`
  ignored, because they only change messages;
* a hash of the unit's constant pool, which is zero-filled now so that
  padding does not carry stale memory (this was the cause of spurious
  differences);
* whether every wait of a process resumes at its start (`restart_top`).

Because it is per function, editing one process leaves the fingerprints of
the others as they were, and moving unrelated code changes only line numbers,
which are ignored.  The file stores the hashes of every unit that had been
compiled when it was written, in a block after the state that is not part of
the state checksum or of the verification.

### 10.2 What replaces the block number

Nothing replaces it in general.  The resume state of a process is the block
number it will resume at.  It is kept as it is when the process code is
unchanged, or changed only after every instruction that had run (10.3):
then the jump table at the start of the function is among the unchanged
instructions, so the number means the same thing.  When the changed process
has run changed code, the number is not trusted: the process is restored
only if both versions always resume at their start (a sensitivity list or a
single wait at the end), in which case the state number of the new code's
wait is used, and otherwise the restore is refused.  A procedure frame is
restored only if its unit is unchanged or changed in code that had not run.
Mapping the waits of a changed process onto each other was not attempted.

### 10.3 When changed code counts as not run

The interpreter marks each instruction when it is fetched (only when
`--checkpoint` or `--restore` is given, or the MCP worker is asked to
`track_code`).  The marks of every unit are saved.  For a unit whose code
differs, let p be the length of the common prefix of the instruction hashes.
The unit counts as changed-but-not-run if the saved marks show that nothing
at or after instruction p had run; everything before p is the same code and
did the same thing.  A different constant pool counts as a difference at
instruction 0.  A unit that is in the checkpoint but not in the design, or
that cannot be compiled, counts as changed, and as run if any instruction
had.  A unit that had not been compiled when the checkpoint was written
cannot have run so changes to it are free.  Calls are accounted for by the
callee's own marks, so a function already called is not harmless.

Because the declarations, initial values, signal and port declarations,
generics, concurrent structure and everything evaluated at elaboration or
reset live in the instance, process and package initialisation code, which
always has run, any change to them is a change to code that had run.

After a restore the marks of unchanged units are carried over and every
changed unit is marked as entirely run, so a later checkpoint of the restored
simulation cannot under-report.  A checkpoint that did not track code, or
that was made with a tiered (compiled) JIT, says so and every change is then
approximate.

### 10.4 The verdict

`exact` only if all of: the checkpoint tracked the code (or nothing
changed), no unit that changed had run the changed part, no variable,
signal or frame layout differs, and the elaborated design (signals,
processes, instance and process frame layouts) hashes the same.  Otherwise
`approximate`.  The verdict is printed as a note (exact) or a warning
(approximate, saying to validate by running the fixed design from zero), is
written as JSON by `--verdict=FILE`, and is returned by the MCP tool.
"Approximate" may be reported for a restore that is in fact identical; the
tests assert only that "exact" is never wrong.

Known reasons for over-reporting: a change anywhere before code that runs
every cycle (a clocked process: the code after the change has run), a
changed string or aggregate constant in the unit, and any change in
initialisation.  So exact results are mostly late phases of testbenches and
checkers.

### 10.5 Matching rules

* Variables of a frame by name where the type hash is equal; others keep
  their initial value (added) or are dropped (removed or changed type).
* Signals by path.  A different size refuses.  A signal that is new keeps
  its initial value and must have no driver or reader yet, otherwise the
  restore is refused; a signal that is gone refuses.
* Processes, triggers, drivers and port connections must be the same set:
  an added or removed process, or a driver that appears or disappears,
  refuses the restore (a pending transaction or a registration has no
  meaning without its owner).
* Packages by name; a package gone refuses.

### 10.6 What is no longer verified

The byte-for-byte comparison of section 1 still runs when nothing about
the layout of state differs (the common case of a fix inside code), and
there it also covers every restored variable of changed units.  When
variables or signals were added or dropped, or a process resumed from its
start instead of its saved point, the comparison is not applicable and is
not done: what was matched by name is not re-read.  This is reported in the
verdict reasons.

### 10.7 Measured

See the final report: the table of verdicts by outcome from
`test/checkpoint/harness.py fix`, and the cost of code tracking, which
was 0.57 s to 0.77 s (35%) on the SoC testbench with the debug interpreter.

### 10.8 Gaps

* The hash is 32 bits per instruction: a collision could lengthen the common
  prefix wrongly (probability about 2^-32 per compared instruction).
* Tracking relies on the interpreter; native tiers (LLVM) are untested and a
  checkpoint made with them is marked as not tracked.
* Messages of the output before the checkpoint were printed by the old code
  and are not part of the comparison.

## 11. Exact verdicts over the code that ran (basic block mappings)

Section 10.3 declared a change harmless only if nothing at or after its first
instruction had run.  That fails the typical RTL case: a clocked process runs
its code after the changed branch on every cycle.  The rule is now
(`src/rt/checkmap.c`, about 120 lines for the checker):

A unit whose code changed is *equivalent over what ran* if the prefix rule
holds, or if the checker confirms a mapping from the basic blocks of the old
code that ran to blocks of the new code.  A mapping that the checker does not
confirm counts as "changed code had run" as before.

### 11.1 The condition checked

Blocks are the straight runs of instructions between leaders: the first
instruction, every branch target, every instruction after a jump, a
conditional branch (`J_JUMP` with a condition, `MACRO_CASE`), a return or a
trap.  A block has *run* if its first instruction was fetched.  The mapping
gives, for the first instruction of an old block, the first instruction of a
new block.  It is confirmed iff:

1. neither unit has control flow the checker does not understand (a label
   operand on anything but a jump or case, two labels, `MACRO_REEXEC`), the
   old marks are known, and every label points inside the code;
2. if the entry block ran, it maps to the entry block;
3. every block that ran maps to the start of a new block, and no two share
   an image;
4. a block that ran and its image have the same length and the same
   instructions: 64-bit hashes of opcode, size, condition, result register,
   both operands (registers, immediates, displacements in addresses, exits,
   callee names; not source locations, loci, debug instructions) and the
   same flow kind, with a label present in both or neither;
5. for every label in such a block whose target block ran, the target's image
   is exactly the label of the corresponding instruction of the image;
6. if the block can fall through and the next old block ran, the image of
   the next old block is the block that follows the image.

Blocks that never ran may be different or absent.

### 11.2 Why that is enough

Claim: if every unit is unchanged or confirmed, and the constant pool, frame
layouts, variable names and types, signals, processes and the post-reset
design are identical (all of which are still checked as strictly as before),
then the new design run from zero reaches the state the old one had at the
checkpoint, and so the restored simulation is the new design at that time.

By induction on the steps of the old run.  Its configuration is the registers
and flags of each active function, the frames and heap, the signals and every
queue of the model.  Initially the configurations are equal (same design,
same reset).  Suppose they are equal and the old run is about to execute
instruction i of unit U, which is in a block that ran.  By 2 to 4 the new run is
at the corresponding instruction of the image of that block, whose
instruction is the same operation on the same registers and immediates.  A
step depends only on the instruction and the configuration, not on its
position, so the result is the same configuration, except for where control
goes next.  Inside a block control goes to the next instruction in both.  At
the end of a block: for a jump or a taken branch whose target block ran, 5 says
the new instruction goes to the image of the target; for a branch not taken or
a fall-through to a block that ran, 6 says the new run falls into the image; and
for a branch whose target never ran the old run did not take it, the condition
is the same in the new run (same instruction, same configuration) so the new
run does not take it either.  A call goes to the callee of the same name,
which by assumption is itself unchanged or confirmed, and returns to the
next instruction; a wait is a return and the state number it stored is an
immediate in a block that ran, so it is the same in both and means the same
in the new code, where the table that dispatches on it begins at the same
entry.  So both runs perform the same steps up to the checkpoint.  The new
code that was not run is not involved.

A process saved suspended is therefore restored with its saved state number
as it is.  This replaces the refusals of section 10.2 when the mapping is
confirmed; without a confirmation they stand.

### 11.3 Assumptions

* Marks are complete: every execution of every instruction before the
  checkpoint was through the interpreter, from the start of the process (or
  carried by a restore: units of a restored run that changed are marked
  fully run).  Compiled tiers are not covered and make the checkpoint
  untracked.
* A step is a function of the instruction and the configuration.  Position
  independence of the interpreter's instructions was checked by reading it:
  `anchor->irpos` and locations only affect diagnostics, which are
  re-derived from the new code after the restore.
* Hash collisions: 64 bits per instruction; a collision could equate two
  different instructions.
* Content that is compared elsewhere is complete: the constant pool by a
  hash of all of it, frame layouts by a hash of names, offsets and types.
  A change in any of those is approximate, as before; in particular a new
  string literal in an unrun branch still makes the unit approximate
  because the constant pool changed.
* Registers live across blocks are covered by the induction: the register
  numbers are part of the instruction, and the register file is part of
  the configuration.
* The checkpoint code compared the machine state of the model for the units
  it saves; state outside it (see section 8) is as at Phase 2.

### 11.4 Where it is, and the proposal

The proposal (`checkmap_propose`) follows the control flow of the old code
that ran in step with the new, from the entry.  It can be arbitrarily wrong:
the checker alone decides.  The checkpoint holds, per instruction, the 64-bit
hash without the label, the label, the flow kind and the mark, so contents
are checked by hash and targets are checked exactly (64 bits per instruction,
rather than the instruction stream, because the stream contains addresses
that differ between processes and would have to be normalised anyway; the
size grows from 4 to 13 bytes per instruction, about 0.8 MB for the SoC).
`test/test_checkmap.c` feeds the checker wrong mappings (entry elsewhere,
missing, shared or mid-block images, swapped blocks, a changed block that ran,
a changed jump target, a block inserted into a fall-through, unknown marks)
and checks that each is rejected.

### 11.5 Oracle and measurements

`harness.py fix` and `mutate` save, for each exact verdict, the fixed design
run from zero to the same time and compare its state section by section
(everything except the identity of the build and the execution marks) with
the state saved right after the restore (`--resave`).  Waveform dumping must
be on in both runs: it changes delta cycle numbers, which are part of the
state.  See the final report for counts.

## 12. Confirmation by segments

An approximate restore says that the fixed design continues from a state the
original run reached; it does not say that the fixed design, run from time
zero, would have reached it.  Confirmation checks that, for every checkpoint
of an earlier run, in parallel instead of by running the whole thing again.

### 12.1 Argument

Let `C0` be the initial state, `C1 ... Cn` the files written by the original
run with `--checkpoint=DIR@every=T` at times `T1 < ... < Tn` (the end of the
first time step at or after each multiple), and `F` the fixed design.  Segment
`i` restores `C(i-1)` into `F` (for `i = 1` there is nothing to restore: `F`
starts from time zero, which covers the initialisation of the new code),
runs to the time step `Ti` and saves its state `S(i)`.  If `S(i)` equals `Ci`
for every `i`, then by induction the state of `F` run from zero at `Ti` is
`Ci`: the state of `F` at `T(i-1)` is `C(i-1)` by hypothesis, a restore of
`C(i-1)` into `F` is `F` at `T(i-1)` provided the restore is faithful, and
the run is deterministic, so what follows is `S(i)`.

The step that needs care is "provided the restore is faithful".  It is not
assumed: each segment restores with `--allow-changes`, and the restore checks
itself by saving the restored state again and comparing it with the file
(`state_verified` in the verdict).  A segment whose restore could not be
verified (for example the saved process was in code that is not equivalent,
section 10) is incomplete and does not count; so is every later segment that
follows it in the chain, because the induction needs every link.  The confirmation never says `confirmed` unless every
segment was run, was verified and agreed.  "Different" is reported for the
first segment that disagrees: all segments before it are confirmed, so the
fixed design reproduces the original up to the previous checkpoint and the
first real difference lies in the interval of that segment.  Nothing checks that `C1 ... Cn` came
from one run of one design, but each `Ci` is checked against what `F` does
from `C(i-1)`, so a file from another run shows up as a difference.

### 12.2 Decisions

* **Comparison.**  The payload of a file (everything but the header, which
  identifies the build, and the trailer of execution marks) is compared
  byte for byte: the name table is built in a
  fixed order, so equal states have equal bytes.  When they differ
  `checkpoint_compare` walks the sections to name the first one that differs
  and the record (signal, process, frame, heap object) in it; this is only
  an explanation, the verdict is the byte comparison.  A section-aware
  comparison that ignored some differences would be a way to be wrong, so it
  is not used for the decision.
* **Differences are repeated before they are believed.**  A segment whose
  state differs from `Ci` is run a second time from the same checkpoint.  If
  the two runs end in different states the segment is incomplete ("not
  reproducible"), never different: bytes that are not part of what the design
  computes can vary from run to run.  The second run costs one segment.
* **Frames start zeroed.**  The frame of a process or procedure was
  allocated without being cleared, so a variable not yet assigned (the
  bounds and index of a `for` loop not yet entered) held whatever the memory
  held before.  Those bytes are dead for the design but they are saved, and
  they differed from run to run: on the SoC bench, replaying a segment of
  the *same* design gave a state that differed from the original's in some
  segments, reported as `different` or as "not reproducible".  The frame is
  now cleared when it is allocated (`irgen_context_struct`), which makes the
  saved state a function of what the design computed.  With this the same
  design confirms on every segment at every interval tried (250 segments).
  The repeat of a differing segment is kept as a guard.
* **Alignment of times.**  The segment runs to exactly the time step the
  original file was saved at (`Ti` is read from the file), not to a multiple
  of the interval, and saves at the end of the first time step at or after
  it: the same rule as the original, so the two are saved at the same point
  of the same delta sequence.
* **Waveform dumping** changes the delta cycle numbers, which are part of the
  state.  A checkpoint records whether dumping was on (`META`) and a
  checkpoint written with it, or a segment that starts from one, is
  incomplete.  The segments themselves run without dumping.
* **Segments in separate directories.**  Each segment runs in
  `DIR/seg-<time>`, so files the design writes by relative name do not
  collide between parallel segments, and the files the design read (the
  union of the `FILE` sections of the checkpoints) are made available there
  by name with symbolic links.  The cwd check of a restore is bypassed
  (`NVC_CKPT_ANYDIR`) since segments run elsewhere by design.  Limits: a
  design that writes a file by absolute path is not isolated; a file that was
  opened and closed again between two checkpoints is not in any `FILE`
  section and is not staged; output files are only compared as far as they
  are part of the saved state.
* **Interrupting and resuming.**  Each settled segment is appended to
  `confirm-progress.txt` with a key: a 64-bit hash of this program, the top
  level, the global options, the contents of the work library and of every
  library directory on the path, the input files, and the two checkpoint
  files at the ends of the segment.  A saved result is used only if the key
  is the same.  The key was added after a test showed that a second run for
  a changed design reused the results of the first (found by
  `test/regress/ckpt4.sh`).
* **Budget.**  `--confirm-budget` and `--confirm-max` stop starting segments
  and report incomplete with the list of segments that were not run, and the
  prefix that was confirmed (`confirmed_up_to_fs`).
* **Disk and memory.**  Files are about 0.6 MB each on the SoC bench (25 MB
  for 41 files), and the number is bounded by thinning (`max`, default 64):
  at the limit every second file is deleted and the interval doubles, so a
  long run keeps at most `max` files spread over its length at the cost of
  coarser segments.  A confirmation holds one restored model per running
  segment: about the memory of the simulation times `--jobs`.
* **Cost of writing.**  Code tracking (needed so that the files can later give
  exact verdicts) costs about a third on the interpreter; each file costs a
  few milliseconds on the SoC bench (see the report).

### 12.3 Assumptions

* The simulation is deterministic given the state and the files it reads:
  no `now`-independent wall clock values, no randomness not derived from the
  state, no plusargs that differ between the runs (the options of the
  confirming run are those given to it: they must be those of the original).
* A run of `F` from `C(i-1)` to `Ti` needs the same input files as the
  original; those that changed are the user's responsibility and the
  comparison will show it as a difference if it matters.
* The verification of a restore compares the model after the restore with
  the file; state that is not saved (section 8) is not checked.

### 12.4 Measurements (final tree)

* **Agreement with a reference.**  The reference for a pair of designs is the
  changed design run from zero to exactly the time of each original
  checkpoint (`--checkpoint=FILE@TIME`, one run per checkpoint), compared
  with the original's file; it does not use restore or segments.
  `harness.py fix --confirm` on the 62 pairs (test/mcp/bench bugs, own and
  `x_*` pairs): confirmed 2 (reference same), different 57 (reference
  differs), incomplete 3 (reference same; the reasons were not tabulated).
  `harness.py mutate --confirm` on 846 mutants (swapped operators, changed
  constants and conditions, deleted statements, changed wait durations):
  confirmed 179 (reference same), different 542 (reference differs),
  incomplete 56 (54 where the reference is the same), not applicable 69.
  **Confirmed where the reference differs: 0.  Different where the
  reference is the same: 0.  Different in an earlier segment than the
  reference: 0** (2 before frames were cleared, see 12.2).
* **Cost of writing.**  Code tracking (needed so that the files can later give
  exact verdicts) costs about a third on the interpreter; each file costs a
  few milliseconds on the SoC bench (see the report).

### 12.3 Assumptions

* The simulation is deterministic given the state and the files it reads:
  no `now`-independent wall clock values, no randomness not derived from the
  state, no plusargs that differ between the runs (the options of the
  confirming run are those given to it: they must be those of the original).
* A run of `F` from `C(i-1)` to `Ti` needs the same input files as the
  original; those that changed are the user's responsibility and the
  comparison will show it as a difference if it matters.
* The verification of a restore compares the model after the restore with
  the file; state that is not saved (section 8) is not checked.

### 12.4 Measurements (final tree)

* **Agreement with a reference.**  The reference for a pair of designs is the
  changed design run from zero to exactly the time of each original
  checkpoint (`--checkpoint=FILE@TIME`, one run per checkpoint), compared
  with the original's file; it does not use restore or segments.
  `harness.py fix --confirm` on the 62 pairs (test/mcp/bench bugs, own and
  `x_*` pairs): confirmed 2 (reference same), different 55, incomplete 5
  (3 where the reference is the same; the reasons were not tabulated).  `harness.py mutate --confirm` on 846 mutants (swapped
  operators, changed constants and conditions, deleted statements, changed
  wait durations): confirmed 173 (reference same), different 540 (reference
  differs), incomplete 65 (63 where the reference is the same), not
  applicable 68.  **Confirmed where the reference differs: 0.  Different
  where the reference is the same: 0.**  Different in an earlier segment than
  the reference: 2 of the 62 pairs, both on the SoC bench (the limit of
  12.2).
* **Cost of writing.**  SoC bench (0.4 ms simulated, about 0.6 s): code
  tracking about +30%; one file about 0.6 MB and 5 ms; every 10 us 41 files,
  25 MB, 1.0 s; every 1 us thinned to 52 files, 32 MB, 1.3 s.
* **Wall time**, a design of 5 ms simulated time that takes 24 s on four
  cores (10 files of 20 KB): original plain 24.3 s, with the files 29.5 s,
  fixed design rerun from zero 25.9 s, confirmation with 1, 2, 4 and 8 jobs
  32.5, 16.2, 9.4 and 9.9 s (CPU 30 to 32 s; four cores, so 8 jobs do not
  help).  The confirmation does the work of one run, plus one extra
  segment startup per file, spread over the workers.
