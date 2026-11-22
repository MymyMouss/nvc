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

#include "util.h"
#include "mcp/mcp.h"
#include "mcp/mcp-server.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __MINGW32__

int mcp_run(const char *root, const char *exe_hint,
            const char *const *global_argv, int global_argc)
{
   fatal("the MCP server is not supported on this platform");
}

#else

static int proto_fd = -1;

static void write_line(void *ctx, const char *line, size_t len)
{
   char *buf = xmalloc(len + 1);
   memcpy(buf, line, len);
   buf[len] = '\n';

   size_t done = 0;
   while (done < len + 1) {
      const ssize_t n = write(proto_fd, buf + done, len + 1 - done);
      if (n < 0 && errno == EINTR)
         continue;
      else if (n <= 0) {
         // The client has gone away
         free(buf);
         exit(EXIT_SUCCESS);
      }

      done += n;
   }

   free(buf);
}

int mcp_run(const char *root, const char *exe_hint,
            const char *const *global_argv, int global_argc)
{
   char *real_root = realpath(root ?: ".", NULL);
   if (real_root == NULL)
      fatal_errno("cannot resolve workspace root '%s'", root ?: ".");

   char exe_buf[PATH_MAX];
   const ssize_t exe_len = readlink("/proc/self/exe", exe_buf,
                                    sizeof(exe_buf) - 1);

   char *exe;
   if (exe_len > 0) {
      exe_buf[exe_len] = '\0';
      exe = xstrdup(exe_buf);
   }
   else if ((exe = realpath(exe_hint, NULL)) == NULL)
      exe = xstrdup(exe_hint);

   // Standard output carries the protocol and nothing else: keep a
   // private copy and point descriptor 1 at standard error so anything
   // nvc prints by accident cannot corrupt the message stream
   proto_fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
   if (proto_fd < 0)
      fatal_errno("cannot duplicate standard output");

   if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
      fatal_errno("cannot redirect standard output");

   signal(SIGPIPE, SIG_IGN);

   const mcp_options_t opts = {
      .write       = write_line,
      .in_fd       = STDIN_FILENO,
      .exe         = exe,
      .root        = real_root,
      .global_argv = global_argv,
      .global_argc = global_argc,
   };

   mcp_server_t *srv = mcp_server_new(&opts);
   mcp_server_run(srv);
   mcp_server_free(srv);

   free(exe);
   free(real_root);

   return EXIT_SUCCESS;
}

#endif   // __MINGW32__
