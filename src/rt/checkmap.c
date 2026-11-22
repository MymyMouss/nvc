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

// Equivalence of changed code over what has run.  See section 11 of
// CHECKPOINT-DESIGN.md for the argument that makes checkmap_confirm
// sufficient.  Everything above checkmap_propose is the part to trust.

#include "util.h"
#include "jit/jit.h"
#include "rt/checkmap.h"

#include <stdlib.h>
#include <string.h>

// Is instruction i the first of a block?  The first instruction, every
// branch target and every instruction after one that can leave are.
static bool is_leader(const code_view_t *c, const bool *target, unsigned i)
{
   return i == 0 || target[i] || c->kind[i - 1] != JIT_FLOW_NEXT;
}

static unsigned block_end(const code_view_t *c, const bool *target,
                          unsigned start)
{
   unsigned i = start + 1;
   while (i < c->n && !is_leader(c, target, i))
      i++;
   return i;
}

static bool *find_targets(const code_view_t *c)
{
   bool *target = xcalloc(c->n + 1);
   for (unsigned i = 0; i < c->n; i++) {
      if (c->label[i] != JIT_NO_LABEL && c->label[i] < c->n)
         target[c->label[i]] = true;
   }
   return target;
}

// Whether the first instruction of the block at start ran
#define RAN(c, i) ((c)->exec[i] != 0)

const char *checkmap_confirm(const code_view_t *old, const code_view_t *new,
                             const int32_t *image)
{
   if (old->unsupported || new->unsupported)
      return "control flow that is not understood";
   else if (old->exec == NULL)
      return "the code that ran is not known";
   else if (old->n == 0 || new->n == 0)
      return "no code";

   // Labels that point outside the code could not be followed
   for (unsigned i = 0; i < old->n; i++) {
      if (old->label[i] != JIT_NO_LABEL && old->label[i] >= old->n)
         return "branch outside the code";
   }
   for (unsigned i = 0; i < new->n; i++) {
      if (new->label[i] != JIT_NO_LABEL && new->label[i] >= new->n)
         return "branch outside the code";
   }

   bool *otarget = find_targets(old), *ntarget = find_targets(new);
   bool *taken = xcalloc(new->n + 1);   // New blocks already the image of one
   const char *why = NULL;

   // The first instruction is the entry of both
   if (old->exec[0] && image[0] != 0)
      why = "the entry is not mapped to the entry";

   for (unsigned s = 0; s < old->n && why == NULL; s++) {
      if (!is_leader(old, otarget, s) || !RAN(old, s))
         continue;   // Blocks that never started may be anything

      const int32_t ns = image[s];
      if (ns < 0 || (unsigned)ns >= new->n || !is_leader(new, ntarget, ns)) {
         why = "a block that ran has no image that is a block";
         break;
      }
      else if (taken[ns]) {
         why = "two blocks that ran have the same image";
         break;
      }

      taken[ns] = true;

      // Same instructions, as a whole block so nothing after a stop in the
      // middle is assumed not to matter
      const unsigned e = block_end(old, otarget, s);
      const unsigned ne = block_end(new, ntarget, ns);
      if (e - s != ne - ns) {
         why = "a block that ran has a different length";
         break;
      }

      for (unsigned k = 0; k < e - s; k++) {
         if (old->hash[s + k] != new->hash[ns + k]
             || old->kind[s + k] != new->kind[ns + k]
             || (old->label[s + k] == JIT_NO_LABEL)
                != (new->label[ns + k] == JIT_NO_LABEL)) {
            why = "a block that ran has different instructions";
            break;
         }
      }

      if (why != NULL)
         break;

      // Where a branch goes, if it went there at all
      for (unsigned k = s; k < e; k++) {
         const unsigned t = old->label[k];
         if (t == JIT_NO_LABEL || !RAN(old, t))
            continue;

         if (image[t] < 0 || (uint32_t)image[t] != new->label[ns + (k - s)]) {
            why = "a branch that may have been taken goes somewhere else";
            break;
         }
      }

      if (why != NULL)
         break;

      // Falling through to the next block
      const unsigned last = e - 1;
      if (old->kind[last] != JIT_FLOW_JUMP && old->kind[last] != JIT_FLOW_END
          && e < old->n && RAN(old, e)) {
         if (ne >= new->n || image[e] != (int32_t)ne) {
            why = "falling through goes somewhere else";
            break;
         }
      }
   }

   free(otarget);
   free(ntarget);
   free(taken);
   return why;
}

////////////////////////////////////////////////////////////////////////////////
// The proposal: anything here may be wrong without consequence

void checkmap_propose(const code_view_t *old, const code_view_t *new,
                      int32_t *image)
{
   for (unsigned i = 0; i < old->n; i++)
      image[i] = -1;

   if (old->n == 0 || new->n == 0 || old->exec == NULL
       || old->unsupported || new->unsupported)
      return;

   bool *otarget = find_targets(old), *ntarget = find_targets(new);
   unsigned *work = xmalloc_array(old->n + 1, sizeof(unsigned));
   unsigned nwork = 0;

   image[0] = 0;
   work[nwork++] = 0;

   while (nwork > 0) {
      const unsigned s = work[--nwork];
      const unsigned ns = image[s];
      const unsigned e = block_end(old, otarget, s);
      const unsigned ne = block_end(new, ntarget, ns);
      if (e - s != ne - ns)
         continue;

      // Follow each way out of the block in step in the two versions
      for (unsigned k = s; k < e; k++) {
         const unsigned t = old->label[k];
         const unsigned nt = new->label[ns + (k - s)];
         if (t != JIT_NO_LABEL && nt != JIT_NO_LABEL && t < old->n
             && nt < new->n && RAN(old, t) && image[t] < 0) {
            image[t] = nt;
            work[nwork++] = t;
         }
      }

      if (old->kind[e - 1] != JIT_FLOW_JUMP
          && old->kind[e - 1] != JIT_FLOW_END
          && e < old->n && ne < new->n && RAN(old, e) && image[e] < 0) {
         image[e] = ne;
         work[nwork++] = e;
      }
   }

   free(otarget);
   free(ntarget);
   free(work);
}
