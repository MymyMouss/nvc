//
//  Copyright (C) 2026  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#ifndef _RT_CHECKPOINT_H
#define _RT_CHECKPOINT_H

#include "prim.h"

#include <stdio.h>

typedef struct _ckpt_id ckpt_id_t;

// Identity of the design and of this build of nvc, which must be the
// same when a checkpoint is restored.  Call this just after model_reset
// as it describes the state of a freshly reset model.
ckpt_id_t *checkpoint_identify(rt_model_t *m, jit_t *jit);
void checkpoint_free_id(ckpt_id_t *id);

// Save the state of a model that is at a quiescent point (the end of a
// time step) to a file.  Returns NULL on success or a malloc'd message
// naming the object that cannot be saved and the reason.  Nothing is
// written on failure and the model is not changed.
char *checkpoint_save(rt_model_t *m, jit_t *jit, const ckpt_id_t *id,
                      const char *path);

// What a restore into a changed design can say about the result.  The
// verdict is "exact" only if the restored simulation is provably the one
// the changed design would have had from time zero: every unit whose code
// changed had not run any of the changed part before the checkpoint.
typedef struct {
   bool      tracked;         // The checkpoint knows which code had run
   bool      changed_design;  // The library or the design hash differed
   bool      layout_changed;  // Variables or signals were added or dropped
   bool      exact;
   unsigned  nchanged;
   char    **changed;         // Names of the units whose code changed
   bool     *executed;        // ... and whether changed code had run that
                              // could not be shown to be equivalent
   bool     *by_mapping;      // Changed code that had run, shown equivalent
   bool     *prefix_ran;      // What the rule without mappings would say
   bool      exact_prefix;    // ... the verdict by that rule
   bool      verified;        // The restored state was compared with the file
   bool      own_state;       // Restored as the state of this very design
   unsigned  nresumed;
   char    **resumed;         // Suspended processes resumed at the same wait
                              // number of changed code that had run
   unsigned  nnotes;
   char    **notes;           // Why the verdict is not exact, and more
} ckpt_report_t;

void checkpoint_report_free(ckpt_report_t *rep);

// Verdict as a JSON object
char *checkpoint_report_json(const ckpt_report_t *rep);

// Restore a checkpoint into a model of the same design that has just been
// reset and has not yet run.  With allow_changes the work library may
// differ from the one the checkpoint came from and the report says what
// that means.  Returns NULL on success or a malloc'd message; after a
// failure the model must be discarded.
char *checkpoint_restore(rt_model_t *m, jit_t *jit, const char *path,
                         bool allow_changes, ckpt_report_t *rep);

// Arrange for a checkpoint to be written the first time a time step at or
// after the given time ends.  The result is reported through diagnostics
// and checkpoint_finish.
void checkpoint_schedule(rt_model_t *m, jit_t *jit, const char *path,
                         uint64_t when);

// Format a simulation time in the largest unit that divides it exactly
void checkpoint_format_time(uint64_t t, char *buf, size_t len);

// Write a checkpoint into DIR every `every` femtoseconds of simulated time
// (at the end of the first time step at or after each multiple), named by
// the time step it was saved at.  At most `max` are kept: when there would
// be more every second one is deleted and the interval doubles, so a long
// run keeps a bounded number spread over its length.
void checkpoint_schedule_every(rt_model_t *m, jit_t *jit, const char *dir,
                               uint64_t every, unsigned max);

// What can be read from a checkpoint file without restoring it
typedef struct {
   uint64_t  time_fs;
   char    **inputs;      // Files the design had open for reading
   unsigned  ninputs;
} ckpt_info_t;

char *checkpoint_describe(const char *path, ckpt_info_t *info);
void checkpoint_info_free(ckpt_info_t *info);

// Compare the saved state of two checkpoints, ignoring what identifies the
// build and which code had run.  Returns NULL if they are the same or a
// malloc'd description of the first difference: the section and the signal,
// process or frame that holds it.
char *checkpoint_compare(const char *a, const char *b);

// Call when the simulation ends: reports a checkpoint that was never
// reached and returns true if the checkpoint failed in any way.
bool checkpoint_finish(rt_model_t *m);

// Description of every frame variable in the design and whether it can be
// saved: one line per variable.  Used to measure what is supported.
void checkpoint_inventory(rt_model_t *m, jit_t *jit, FILE *out);

#endif   // _RT_CHECKPOINT_H
