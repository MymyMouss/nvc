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

#ifndef _MCP_CHILD_H
#define _MCP_CHILD_H

#include "mcp/mcp-server.h"

//
// Runs nvc as an isolated child process
//
// The server never loads a design itself: nvc calls exit() on fatal
// errors and a crash must not take the protocol channel with it.
// Analysis and elaboration therefore run in a short lived worker whose
// standard output and error are captured, and whose structured
// diagnostics come back on a separate pipe (see --diag-json).
//

typedef struct {
   const char        *cwd;
   const char *const *argv;     // NULL terminated, argv[0] is the executable
   unsigned           wall_ms;
   size_t             max_output;
   size_t             max_diag;
} mcp_child_spec_t;

typedef struct {
   bool    spawn_failed;
   bool    timed_out;
   bool    cancelled;
   bool    exited;           // Terminated normally with exit_status
   int     exit_status;
   int     term_signal;      // Non-zero if killed by a signal
   char   *output;           // Combined standard output and error
   size_t  output_len;
   bool    output_truncated;
   char   *diag;             // Newline delimited JSON diagnostics
   size_t  diag_len;
   bool    diag_truncated;
} mcp_child_result_t;

void mcp_child_run(mcp_call_t *call, const mcp_child_spec_t *spec,
                   mcp_child_result_t *res);
void mcp_child_free(mcp_child_result_t *res);

// Installs the consumer used by the worker side of --diag-json
void mcp_diag_install(int fd);

#endif   // _MCP_CHILD_H
