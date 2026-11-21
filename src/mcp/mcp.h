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

#ifndef _MCP_H
#define _MCP_H

#include "prim.h"

// Run the MCP server on standard input and output until the client
// closes its end.  The root directory bounds every file the tools may
// access and defaults to the current directory.  The global options are
// passed on to each worker process.
int mcp_run(const char *root, const char *exe_hint,
            const char *const *global_argv, int global_argc);

// Worker side: report diagnostics as JSON lines on the given descriptor
void mcp_diag_install(int fd);

// Format a time in femtoseconds with the largest exact unit
void mcp_format_time(int64_t fs, char *buf, size_t len);

// Worker side: serve a loaded simulation on the control socket until the
// supervisor closes it.  See mcp-worker.c.
int mcp_worker_main(rt_model_t *m, tree_t top, jit_t *jit, int ctl_fd,
                    const char *files_dir, const char *restore_file,
                    bool allow_changes, const char *every_dir,
                    uint64_t every_fs);

#endif   // _MCP_H
