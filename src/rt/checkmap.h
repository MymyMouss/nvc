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

#ifndef _RT_CHECKMAP_H
#define _RT_CHECKMAP_H

#include "prim.h"

// The code of one unit as the checkpoint code sees it
typedef struct {
   unsigned        n;
   const uint64_t *hash;    // Of each instruction, branch target left out
   const uint32_t *label;   // Branch target, or JIT_NO_LABEL
   const uint8_t  *kind;    // JIT_FLOW_*
   const uint8_t  *exec;    // Old code only: which instructions had run
   bool            unsupported;
} code_view_t;

// Decide whether the mapping from the old code to the new one, which says
// for the first instruction of an old block the first instruction of the
// new block that replaces it (or -1), is one under which the new code does
// exactly what the old did in every block that had run.  The mapping need
// not be trusted: anything the check cannot confirm is rejected.  Returns
// NULL if confirmed or the reason.
const char *checkmap_confirm(const code_view_t *old, const code_view_t *new,
                             const int32_t *image);

// A mapping to try: follows the control flow of the old code that ran in
// step with the new.  Fills image (one entry per old instruction).
void checkmap_propose(const code_view_t *old, const code_view_t *new,
                      int32_t *image);

#endif   // _RT_CHECKMAP_H
