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
#include "mcp/mcp-child.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define DIAG_MIN_FD 10

typedef struct {
   char   *data;
   size_t  len;
   size_t  alloc;
   size_t  max;
   bool    truncated;
} capture_t;

static void capture_append(capture_t *c, const char *buf, size_t n)
{
   // Keep draining the pipe after the limit so the child never blocks
   if (c->len + n > c->max) {
      c->truncated = true;
      n = c->max > c->len ? c->max - c->len : 0;
   }

   if (n == 0)
      return;

   if (c->len + n + 1 > c->alloc) {
      c->alloc = MAX(c->alloc * 2, c->len + n + 1);
      c->data = xrealloc(c->data, c->alloc);
   }

   memcpy(c->data + c->len, buf, n);
   c->len += n;
   c->data[c->len] = '\0';
}

static void close_if_open(int *fd)
{
   if (*fd >= 0) {
      close(*fd);
      *fd = -1;
   }
}

static void child_exec(const mcp_child_spec_t *spec, char **argv, int out_wr,
                       int diag_fd)
{
   // Own process group so the whole tree can be killed
   setpgid(0, 0);

   const int devnull = open("/dev/null", O_RDONLY);
   if (devnull >= 0)
      dup2(devnull, STDIN_FILENO);

   dup2(out_wr, STDOUT_FILENO);
   dup2(out_wr, STDERR_FILENO);

   const long maxfd = MIN(sysconf(_SC_OPEN_MAX), 4096);
   for (int fd = STDERR_FILENO + 1; fd < maxfd; fd++) {
      if (fd != diag_fd)
         close(fd);
   }

   if (spec->cwd != NULL && chdir(spec->cwd) < 0)
      _exit(126);

   const rlim_t cpu = spec->wall_ms / 1000 + 5;
   struct rlimit rl = { cpu, cpu };
   setrlimit(RLIMIT_CPU, &rl);

   rl = (struct rlimit){ 0, 0 };
   setrlimit(RLIMIT_CORE, &rl);

   rl = (struct rlimit){ 512u << 20, 512u << 20 };
   setrlimit(RLIMIT_FSIZE, &rl);

   execv(argv[0], argv);
   _exit(127);
}

void mcp_child_run(mcp_call_t *call, const mcp_child_spec_t *spec,
                   mcp_child_result_t *res)
{
   memset(res, 0, sizeof(*res));

   int out_pipe[2] = { -1, -1 }, diag_pipe[2] = { -1, -1 };

   if (pipe(out_pipe) < 0 || pipe(diag_pipe) < 0) {
      res->spawn_failed = true;
      close_if_open(&out_pipe[0]);
      close_if_open(&out_pipe[1]);
      close_if_open(&diag_pipe[0]);
      close_if_open(&diag_pipe[1]);
      return;
   }

   // The diagnostics descriptor must not collide with the descriptors
   // the child redirects to 0-2
   const int diag_wr = fcntl(diag_pipe[1], F_DUPFD, DIAG_MIN_FD);
   if (diag_wr < 0) {
      res->spawn_failed = true;
      close_if_open(&out_pipe[0]);
      close_if_open(&out_pipe[1]);
      close_if_open(&diag_pipe[0]);
      close_if_open(&diag_pipe[1]);
      return;
   }

   close(diag_pipe[1]);
   diag_pipe[1] = -1;

   // argv[0], then the descriptor option, then the caller's arguments
   size_t nargs = 0;
   while (spec->argv[nargs] != NULL)
      nargs++;

   char **argv = xmalloc_array(nargs + 2, sizeof(char *));
   argv[0] = (char *)spec->argv[0];

   char opt[32];
   checked_sprintf(opt, sizeof(opt), "--diag-json=%d", diag_wr);
   argv[1] = opt;

   for (size_t i = 1; i < nargs; i++)
      argv[i + 1] = (char *)spec->argv[i];
   argv[nargs + 1] = NULL;

   const uint64_t start_us = get_timestamp_us();

   const pid_t pid = fork();
   if (pid < 0) {
      res->spawn_failed = true;
      close_if_open(&out_pipe[0]);
      close_if_open(&out_pipe[1]);
      close_if_open(&diag_pipe[0]);
      close(diag_wr);
      free(argv);
      return;
   }
   else if (pid == 0)
      child_exec(spec, argv, out_pipe[1], diag_wr);

   free(argv);

   close(out_pipe[1]);
   close(diag_wr);

   capture_t out = { .max = spec->max_output };
   capture_t diag = { .max = spec->max_diag };

   int out_fd = out_pipe[0], diag_fd = diag_pipe[0];
   bool killed = false;

   while (out_fd >= 0 || diag_fd >= 0) {
      const int64_t elapsed_ms = (get_timestamp_us() - start_us) / 1000;

      if (!killed && mcp_call_cancelled(call)) {
         res->cancelled = true;
         killed = true;
         kill(-pid, SIGKILL);
      }
      else if (!killed && elapsed_ms >= spec->wall_ms) {
         res->timed_out = true;
         killed = true;
         kill(-pid, SIGKILL);
      }

      int fds[2];
      size_t nfds = 0, which[2];
      if (out_fd >= 0) {
         which[nfds] = 0;
         fds[nfds++] = out_fd;
      }
      if (diag_fd >= 0) {
         which[nfds] = 1;
         fds[nfds++] = diag_fd;
      }

      int timeout = killed ? 1000 : (int)(spec->wall_ms - elapsed_ms);
      if (timeout < 0)
         timeout = 0;

      bool ready[2] = { false, false };
      mcp_call_wait(call, fds, nfds, ready, timeout);

      for (size_t i = 0; i < nfds; i++) {
         if (!ready[i])
            continue;

         int *fd = which[i] == 0 ? &out_fd : &diag_fd;
         capture_t *cap = which[i] == 0 ? &out : &diag;

         char buf[8192];
         const ssize_t n = read(*fd, buf, sizeof(buf));
         if (n > 0)
            capture_append(cap, buf, n);
         else if (n == 0 || (errno != EINTR && errno != EAGAIN))
            close_if_open(fd);
      }
   }

   int status = 0;
   while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
      ;

   if (WIFEXITED(status)) {
      res->exited = true;
      res->exit_status = WEXITSTATUS(status);
   }
   else if (WIFSIGNALED(status))
      res->term_signal = WTERMSIG(status);

   // 127 is the status of a failed exec
   if (res->exited && res->exit_status == 127 && out.len == 0
       && diag.len == 0)
      res->spawn_failed = true;

   res->output = out.data;
   res->output_len = out.len;
   res->output_truncated = out.truncated;
   res->diag = diag.data;
   res->diag_len = diag.len;
   res->diag_truncated = diag.truncated;
}

void mcp_child_free(mcp_child_result_t *res)
{
   free(res->output);
   free(res->diag);
   memset(res, 0, sizeof(*res));
}
