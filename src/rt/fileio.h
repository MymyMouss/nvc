//
//  Copyright (C) 2025  Nick Gasson
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

#ifndef _RT_FILEIO_H
#define _RT_FILEIO_H

#include "prim.h"

#include <stdio.h>

typedef enum {
   FILE_READ = 0,
   FILE_WRITE = 1,
   FILE_APPEND = 2,
   FILE_READ_WRITE = 3,
} file_mode_t;

typedef uint32_t file_handle_t;

bool file_mode(file_handle_t fh, file_mode_t *mode);
bool file_logical_name(file_handle_t fh, const char **name);
file_handle_t file_open(const char *name, file_mode_t mode);
bool file_close(file_handle_t fh);
FILE *file_stream(file_handle_t fh);

// Support for forked copies of the simulation (see mcp-worker.c).  Save
// the position of every open file before forking, and in the copy reopen
// them all.  The latter returns NULL or a message for the caller to free.
void file_save_positions(void);
char *file_reopen_all(void);
unsigned file_count_open(void);

// Description of an open file for a checkpoint.  Files written by the
// design are saved with their contents so that restoring does not depend
// on what the original run did to them afterwards; input files must be
// unchanged.
typedef struct {
   bool         predef;     // STD_INPUT or STD_OUTPUT
   file_mode_t  mode;
   char        *name;
   int64_t      size;
   int64_t      pos;
   uint64_t     hash;       // Of the contents, input files only
   uint8_t     *data;       // Contents of output files
} file_ckpt_t;

unsigned file_enum_open(file_handle_t *out, unsigned max);
char *file_ckpt_describe(file_handle_t fh, file_ckpt_t *out);
char *file_ckpt_apply(const file_ckpt_t *in, file_handle_t cur,
                      file_handle_t *out);
void file_ckpt_free(file_ckpt_t *f);

#endif  // _RT_FILEIO_H
