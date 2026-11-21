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

// Restore a checkpoint into a model of the same design that has just been
// reset and has not yet run.  Returns NULL on success or a malloc'd
// message; after a failure the model must be discarded.
char *checkpoint_restore(rt_model_t *m, jit_t *jit, const char *path);

// Arrange for a checkpoint to be written the first time a time step at or
// after the given time ends.  The result is reported through diagnostics
// and checkpoint_finish.
void checkpoint_schedule(rt_model_t *m, jit_t *jit, const char *path,
                         uint64_t when);

// Format a simulation time in the largest unit that divides it exactly
void checkpoint_format_time(uint64_t t, char *buf, size_t len);

// Call when the simulation ends: reports a checkpoint that was never
// reached and returns true if the checkpoint failed in any way.
bool checkpoint_finish(rt_model_t *m);

// Description of every frame variable in the design and whether it can be
// saved: one line per variable.  Used to measure what is supported.
void checkpoint_inventory(rt_model_t *m, jit_t *jit, FILE *out);

#endif   // _RT_CHECKPOINT_H
