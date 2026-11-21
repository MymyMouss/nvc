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

#ifndef _RT_CONFIRM_H
#define _RT_CONFIRM_H

#include "prim.h"

typedef struct {
   const char  *dir;           // Checkpoints written by --checkpoint=DIR@every=
   const char  *top;
   char *const *child_args;    // Program and global options, null terminated
   unsigned     jobs;
   double       budget_s;      // Wall time allowed, or 0
   unsigned     max_segments;  // Segments to start, or 0
   const char  *result_file;   // JSON result, or NULL for standard output
} confirm_opts_t;

// Run the design as it is now from time zero and from each checkpoint in
// the directory to the next one and compare the states: if all agree the
// design reaches the last one when run from zero.  Segments run in parallel.
// Returns 0 if confirmed, 1 if the states differ and 2 if some segment could
// not be checked or the budget ran out.
int confirm_run(const confirm_opts_t *opts);

#endif   // _RT_CONFIRM_H
