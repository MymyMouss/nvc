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

#ifndef _MCP_SERVER_H
#define _MCP_SERVER_H

#include "mcp/mcp-json.h"

// Tool schemas are written as JSON text: the macro quotes it.  Avoid
// apostrophes in descriptions.
#define MCP_JSON(...) #__VA_ARGS__

//
// Model Context Protocol server
//
// The server speaks JSON-RPC 2.0 over newline-delimited messages.  Both
// the stateless 2026-07-28 revision and the earlier handshake based
// revisions (2025-11-25 and 2025-06-18) are supported.  See
// MCP-ARCHITECTURE.md for the design.
//

typedef struct _mcp_server mcp_server_t;
typedef struct _mcp_call   mcp_call_t;

// Called once for each outgoing message, without the trailing newline
typedef void (*mcp_write_fn_t)(void *ctx, const char *line, size_t len);

typedef struct {
   mcp_write_fn_t     write;
   void              *write_ctx;
   int                in_fd;          // Negative when driven by handle_line
   const char        *exe;            // Executable used to start workers
   const char        *root;           // Canonical workspace root
   const char *const *global_argv;    // Global options forwarded to workers
   int                global_argc;
} mcp_options_t;

mcp_server_t *mcp_server_new(const mcp_options_t *opts);
void mcp_server_free(mcp_server_t *srv);

// Process one message synchronously
void mcp_server_handle_line(mcp_server_t *srv, const char *line, size_t len);

// Read messages from in_fd until end of file
void mcp_server_run(mcp_server_t *srv);

//
// Interface for tool implementations
//

// Returns true on success with the structured result written to out or
// false after calling mcp_call_error
typedef bool (*mcp_tool_fn_t)(mcp_call_t *call, const json_t *args,
                              json_writer_t *out);

typedef struct {
   const char    *name;
   const char    *title;
   const char    *description;
   const char    *input_schema;    // JSON Schema as JSON text
   const char    *output_schema;   // JSON Schema as JSON text
   const char    *example;         // Example arguments as JSON text
   bool           read_only;
   bool           destructive;
   bool           idempotent;
   mcp_tool_fn_t  fn;
} mcp_tool_t;

const mcp_tool_t *mcp_tool_table(size_t *count);

void mcp_call_error(mcp_call_t *call, const char *fmt, ...)
   __attribute__((format(printf, 2, 3)));

const char *mcp_call_root(mcp_call_t *call);
const char *mcp_call_exe(mcp_call_t *call);
const char *const *mcp_call_global_argv(mcp_call_t *call, int *argc);

// True when the client cancelled this request
bool mcp_call_cancelled(mcp_call_t *call);

// Report progress of a long call.  Does nothing unless the client asked
// for progress notifications with a progress token.  The value must
// increase between calls.
bool mcp_call_wants_progress(mcp_call_t *call);
void mcp_call_progress(mcp_call_t *call, int64_t progress, const char *message);

// Live simulations owned by this server
typedef struct _mcp_sims mcp_sims_t;
mcp_sims_t *mcp_call_sims(mcp_call_t *call);

// Tool definitions that live in other files
const mcp_tool_t *mcp_sim_tools(size_t *count);
mcp_sims_t *mcp_sims_new(void);
void mcp_sims_free(mcp_sims_t *sims);
void mcp_sims_reap(mcp_sims_t *sims);

// Wait up to timeout_ms (negative for no limit) for any of the file
// descriptors to become readable while still servicing the client's
// input, which may cancel the call.  ready[i] is set for each
// descriptor that is readable or hung up.
void mcp_call_wait(mcp_call_t *call, const int *fds, size_t nfds,
                   bool *ready, int timeout_ms);

#endif   // _MCP_SERVER_H
