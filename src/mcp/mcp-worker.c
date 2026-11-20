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
#include "common.h"
#include "diag.h"
#include "hash.h"
#include "ident.h"
#include "jit/jit.h"
#include "mcp/mcp.h"
#include "mcp/mcp-json.h"
#include "rt/model.h"
#include "rt/printer.h"
#include "rt/structs.h"
#include "rt/wave.h"
#include "tree.h"
#include "type.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

//
// Worker process for one live simulation
//
// The worker owns the simulation model.  It is started with the design
// on its command line, then answers newline delimited JSON requests on a
// control socket one at a time.  The supervisor in mcp-sim.c turns the
// answers into tool results, so the shape of every response here follows
// the output schema of the corresponding tool.
//

#define MAX_MESSAGES        100
#define MAX_MESSAGE_CHARS   2000
#define MAX_TRACE_LINES     6
#define OUTPUT_LIMIT        16384
#define PROGRESS_PERIOD_US  250000
#define SLICE_TARGET_US     20000
#define FILES_CHECK_US      250000
#define DEFAULT_LOG_MAX     (1ull << 30)
#define DEFAULT_WAVE_MAX    (1ull << 30)
#define MAX_WAVE_MAX        (64ull << 30)
#define MAX_WAVE_PATTERNS   64
#define MAX_REQUEST_BYTES   (1024 * 1024)
#define MAX_LIST_LIMIT      200

#define CLAMP_INT(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

#define MAX_TRACE_SIGNALS    512
#define MAX_TRACE_BYTES      256
#define DEFAULT_TRACE_CHANGES 10000
#define MAX_TRACE_CHANGES    500000
#define TRACE_MEMORY_LIMIT   (64u << 20)

typedef enum { OBJ_REGION, OBJ_SIGNAL } obj_kind_t;
typedef struct _trace trace_t;

typedef struct {
   obj_kind_t    kind;
   ident_t       name;
   ident_t       path;
   int           parent;
   rt_signal_t  *signal;
   print_func_t *printer;
} design_obj_t;

typedef struct {
   rt_model_t   *model;
   tree_t        top;
   int           ctl_fd;
   char         *inbuf;
   size_t        inlen;
   size_t        inalloc;
   bool          eof;

   design_obj_t *objs;
   size_t        nobjs;
   size_t        objalloc;
   hash_t       *by_path;
   printer_t    *printer;

   // Captured standard output and error
   int           out_fd;
   off_t         out_off;

   // Diagnostics emitted while running
   char        **msgs;
   size_t        nmsgs;
   size_t        nmsgs_dropped;
   unsigned      counts[DIAG_FATAL + 1];
   diag_level_t  pause_level;
   bool          pause_requested;

   bool          started;
   bool          ended;
   uint64_t      last_target;

   // The run in progress, advanced one slice at a time between requests
   struct {
      bool      active;
      bool      background;
      bool      progress;
      uint64_t  start_us;
      uint64_t  last_progress_us;
      uint64_t  stop;
      int64_t   max_wall_ms;     // Zero for no limit
      int64_t   start_fs;
      unsigned  slice;
   } run;

   // Files written under files_dir, a path relative to the working directory
   const char   *files_dir;
   jit_t        *jit;
   uint64_t      log_max;
   bool          log_full;
   wave_dumper_t *wave;
   wave_dumper_t **old_waves;
   size_t        nold_waves;
   char         *wave_file;       // Name of the file being written
   char        **wave_done;       // Finished files
   size_t        nwave_done;
   uint64_t      wave_max;
   bool          wave_quota_hit;
   unsigned      wave_seq;
   uint64_t      wave_bytes;
   uint64_t      wave_rate;       // Bytes per second at the last check
   uint64_t      files_check_us;

   const char   *last_reason;    // Why the previous run stopped
   int64_t       last_wall_ms;
   uint64_t      msg_base;       // Sequence number of msgs[0]

   trace_t      *live;             // In-memory recording, if any
   unsigned      next_trace_id;
} worker_t;

static void files_check(worker_t *w, bool force);
static void write_files(worker_t *w, json_writer_t *jw);
static void wave_finish(worker_t *w);
static bool live_pick(worker_t *w, const json_t *include,
                      const json_t *exclude, design_obj_t ***picked,
                      size_t *npicked, text_buf_t *err);
static bool live_start(worker_t *w, design_obj_t **picked, size_t npicked,
                       int64_t max_changes, text_buf_t *err);
static void live_stop(worker_t *w);
static bool live_active(const worker_t *w);
static void live_free(worker_t *w);
static void write_live(worker_t *w, json_writer_t *jw);

////////////////////////////////////////////////////////////////////////////////
// Helpers

void mcp_format_time(int64_t fs, char *buf, size_t len)
{
   static const struct { const char *unit; int64_t scale; } units[] = {
      { "s", 1000000000000000 }, { "ms", 1000000000000 },
      { "us", 1000000000 }, { "ns", 1000000 }, { "ps", 1000 }, { "fs", 1 },
   };

   if (fs == 0) {
      checked_sprintf(buf, len, "0 fs");
      return;
   }

   for (size_t i = 0; i < ARRAY_LEN(units); i++) {
      if (fs % units[i].scale == 0) {
         checked_sprintf(buf, len, "%"PRIi64" %s", fs / units[i].scale,
                         units[i].unit);
         return;
      }
   }
}

static void ctl_send(worker_t *w, text_buf_t *tb)
{
   tb_append(tb, '\n');

   const char *data = tb_get(tb);
   size_t left = tb_len(tb);

   while (left > 0) {
      const ssize_t n = write(w->ctl_fd, data, left);
      if (n < 0 && errno == EINTR)
         continue;
      else if (n <= 0)
         _exit(0);   // The supervisor has gone away

      data += n;
      left -= n;
   }
}

// Returns a newline terminated request without the newline, or NULL if
// no complete request is available within timeout_ms (negative to wait
// forever).  Sets eof when the supervisor closes the socket.
static char *ctl_read_line(worker_t *w, int timeout_ms)
{
   for (;;) {
      char *nl = memchr(w->inbuf, '\n', w->inlen);
      if (nl != NULL) {
         const size_t len = nl - w->inbuf;
         char *line = xmalloc(len + 1);
         memcpy(line, w->inbuf, len);
         line[len] = '\0';

         memmove(w->inbuf, nl + 1, w->inlen - len - 1);
         w->inlen -= len + 1;
         return line;
      }

      if (w->eof)
         return NULL;

      struct pollfd pfd = { .fd = w->ctl_fd, .events = POLLIN };
      const int rc = poll(&pfd, 1, timeout_ms);
      if (rc < 0 && errno == EINTR)
         continue;
      else if (rc <= 0)
         return NULL;

      if (w->inlen + 4096 > w->inalloc) {
         w->inalloc = MAX(w->inalloc * 2, 8192);
         w->inbuf = xrealloc(w->inbuf, w->inalloc);
      }

      const ssize_t n = read(w->ctl_fd, w->inbuf + w->inlen,
                             w->inalloc - w->inlen);
      if (n < 0 && (errno == EINTR || errno == EAGAIN))
         continue;
      else if (n <= 0) {
         w->eof = true;
         continue;
      }

      w->inlen += n;

      if (w->inlen > MAX_REQUEST_BYTES)
         _exit(1);   // Not a well behaved supervisor
   }
}

static void begin_reply(json_writer_t *w, bool ok)
{
   jw_begin_object(w);
   jw_key(w, "ok");
   jw_bool(w, ok);
}

static void reply_error(worker_t *w, const char *fmt, ...)
   __attribute__((format(printf, 2, 3)));

static void reply_error(worker_t *w, const char *fmt, ...)
{
   LOCAL_TEXT_BUF msg = tb_new();

   va_list ap;
   va_start(ap, fmt);
   tb_vprintf(msg, fmt, ap);
   va_end(ap);

   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);

   begin_reply(&jw, false);
   jw_key(&jw, "error");
   jw_string(&jw, tb_get(msg));
   jw_end_object(&jw);

   ctl_send(w, tb);
}

////////////////////////////////////////////////////////////////////////////////
// Design index

static design_obj_t *add_obj(worker_t *w, obj_kind_t kind, ident_t name,
                             ident_t path, int parent)
{
   if (w->nobjs == w->objalloc) {
      w->objalloc = MAX(w->objalloc * 2, 256);
      w->objs = xrealloc_array(w->objs, w->objalloc, sizeof(design_obj_t));
   }

   design_obj_t *o = &(w->objs[w->nobjs]);
   memset(o, 0, sizeof(*o));
   o->kind   = kind;
   o->name   = name;
   o->path   = path;
   o->parent = parent;

   hash_put(w->by_path, path, (void *)(uintptr_t)(w->nobjs + 1));

   w->nobjs++;
   return o;
}

static void index_scope(worker_t *w, rt_scope_t *scope, int parent,
                        text_buf_t *path)
{
   const size_t base = tb_len(path);

   // The path buffer ends with a separator while inside a region: region
   // paths are reported without it, except for the root
   const bool is_root = base == 1;
   ident_t rname = ident_downcase(tree_ident(scope->where));
   ident_t rpath;
   if (is_root)
      rpath = ident_new("/");
   else {
      LOCAL_TEXT_BUF tmp = tb_new();
      tb_catn(tmp, tb_get(path), base - 1);
      rpath = ident_new(tb_get(tmp));
   }

   const int self = w->nobjs;
   add_obj(w, OBJ_REGION, rname, rpath, parent);

   const size_t nsig = scope->signals.count;
   for (size_t i = 0; i < nsig + scope->aliases.count; i++) {
      rt_signal_t *sig;
      ident_t name;
      if (i < nsig) {
         sig = scope->signals.items[i];
         name = ident_downcase(tree_ident(sig->where));
      }
      else {
         rt_alias_t *a = scope->aliases.items[i - nsig];
         sig = a->signal;
         name = ident_downcase(tree_ident(a->where));
      }

      tb_istr(path, name);
      design_obj_t *o = add_obj(w, OBJ_SIGNAL, name, ident_new(tb_get(path)),
                                self);
      o->signal = sig;
      tb_trim(path, base);
   }

   for (size_t i = 0; i < scope->children.count; i++) {
      rt_scope_t *child = scope->children.items[i];
      tb_istr(path, ident_downcase(tree_ident(child->where)));
      tb_append(path, '/');
      index_scope(w, child, self, path);
      tb_trim(path, base);
   }
}

static design_obj_t *find_obj(worker_t *w, const char *path)
{
   LOCAL_TEXT_BUF tb = tb_new();
   tb_cat(tb, path);
   tb_downcase(tb);

   // Regions are accepted with or without a trailing slash
   while (tb_len(tb) > 1 && tb_get(tb)[tb_len(tb) - 1] == '/')
      tb_trim(tb, tb_len(tb) - 1);

   uintptr_t index = (uintptr_t)hash_get(w->by_path, ident_new(tb_get(tb)));
   return index == 0 ? NULL : &(w->objs[index - 1]);
}

static const char *obj_kind_name(const design_obj_t *o)
{
   return o->kind == OBJ_REGION ? "region" : "signal";
}

static bool obj_printer(worker_t *w, design_obj_t *o)
{
   if (o->printer == NULL)
      o->printer = printer_for(w->printer, tree_type(o->signal->where));

   return o->printer != NULL;
}

static void write_obj(worker_t *w, json_writer_t *jw, design_obj_t *o)
{
   jw_begin_object(jw);
   jw_key(jw, "path");
   jw_string(jw, istr(o->path));
   jw_key(jw, "kind");
   jw_string(jw, obj_kind_name(o));

   if (o->kind == OBJ_SIGNAL) {
      jw_key(jw, "type");
      jw_string(jw, type_pp(tree_type(o->signal->where)));
      jw_key(jw, "width");
      jw_int(jw, signal_width(o->signal));
   }

   jw_end_object(jw);
}

// Suggest signals or regions with the same last path component
static void suggest_paths(worker_t *w, const char *path, text_buf_t *out)
{
   const char *slash = strrchr(path, '/');
   const char *leaf = slash ? slash + 1 : path;

   unsigned n = 0;
   for (size_t i = 0; i < w->nobjs && n < 5; i++) {
      if (strcasecmp(istr(w->objs[i].name), leaf) == 0) {
         tb_cat(out, n++ == 0 ? ". Did you mean: " : ", ");
         tb_cat(out, istr(w->objs[i].path));
      }
   }

   if (n > 0)
      tb_append(out, '?');
   else
      tb_cat(out, ". Use nvc_design_search to find the right path");
}

////////////////////////////////////////////////////////////////////////////////
// Diagnostics and output capture

static const char *severity_name(diag_level_t level)
{
   switch (level) {
   case DIAG_DEBUG:   return "debug";
   case DIAG_NOTE:    return "note";
   case DIAG_WARN:    return "warning";
   case DIAG_ERROR:   return "error";
   case DIAG_FAILURE: return "failure";
   case DIAG_FATAL:   return "fatal";
   }

   return "error";
}

static void worker_diag(diag_t *d, void *context)
{
   worker_t *w = context;

   const diag_level_t level = diag_level(d, NULL);
   w->counts[level]++;

   if (level >= w->pause_level)
      w->pause_requested = true;

   if (w->nmsgs >= MAX_MESSAGES) {
      w->nmsgs_dropped++;
      return;
   }

   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);

   jw_begin_object(&jw);

   jw_key(&jw, "severity");
   jw_string(&jw, severity_name(level));

   unsigned delta = 0;
   const int64_t now = model_now(w->model, &delta);
   char timestr[32];
   mcp_format_time(now, timestr, sizeof(timestr));

   jw_key(&jw, "time_fs");
   jw_int(&jw, now);
   jw_key(&jw, "time");
   jw_string(&jw, timestr);
   jw_key(&jw, "delta");
   jw_int(&jw, delta);

   const loc_t *loc = diag_get_loc(d);
   if (loc != NULL && !loc_invalid_p(loc)) {
      const char *file = loc_file_str(loc);
      if (file != NULL) {
         jw_key(&jw, "file");
         jw_string(&jw, file);
      }

      jw_key(&jw, "line");
      jw_int(&jw, loc->first_line);
   }

   const char *text = diag_get_text(d);
   jw_key(&jw, "message");
   jw_stringn(&jw, text, MIN(strlen(text), MAX_MESSAGE_CHARS));

   const int ntrace = MIN(diag_traces(d), MAX_TRACE_LINES);
   if (ntrace > 0) {
      jw_key(&jw, "trace");
      jw_begin_array(&jw);
      for (int i = 0; i < ntrace; i++) {
         const char *line = diag_get_trace(d, i);
         if (line != NULL)
            jw_string(&jw, line);
      }
      jw_end_array(&jw);
   }

   jw_end_object(&jw);

   w->msgs = xrealloc_array(w->msgs, w->nmsgs + 1, sizeof(char *));
   w->msgs[w->nmsgs++] = xstrdup(tb_get(tb));
}

static void reset_messages(worker_t *w)
{
   w->msg_base += w->nmsgs + w->nmsgs_dropped;

   for (size_t i = 0; i < w->nmsgs; i++)
      free(w->msgs[i]);

   free(w->msgs);
   w->msgs = NULL;
   w->nmsgs = w->nmsgs_dropped = 0;
   memset(w->counts, 0, sizeof(w->counts));
   w->pause_requested = false;
}

static void write_messages_since(worker_t *w, json_writer_t *jw,
                                 uint64_t since)
{
   jw_key(jw, "messages");
   jw_begin_array(jw);
   for (size_t i = 0; i < w->nmsgs; i++) {
      if (w->msg_base + i >= since)
         jw_raw(jw, w->msgs[i]);
   }
   jw_end_array(jw);

   jw_key(jw, "next_seq");
   jw_int(jw, w->msg_base + w->nmsgs + w->nmsgs_dropped);

   jw_key(jw, "messages_truncated");
   jw_bool(jw, w->nmsgs_dropped > 0);

   jw_key(jw, "counts");
   jw_begin_object(jw);
   jw_key(jw, "note");
   jw_int(jw, w->counts[DIAG_NOTE]);
   jw_key(jw, "warning");
   jw_int(jw, w->counts[DIAG_WARN]);
   jw_key(jw, "error");
   jw_int(jw, w->counts[DIAG_ERROR]);
   jw_key(jw, "failure");
   jw_int(jw, w->counts[DIAG_FAILURE] + w->counts[DIAG_FATAL]);
   jw_end_object(jw);
}

static void write_messages(worker_t *w, json_writer_t *jw)
{
   write_messages_since(w, jw, 0);
}

static void capture_output(worker_t *w)
{
   // Standard output and error go to a file so what the design prints is
   // returned with the call that produced it and can be read again later
   int fd = -1;

   if (w->files_dir != NULL) {
      char *name = xasprintf("%s/output.log", w->files_dir);
      fd = open(name, O_RDWR | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
      free(name);
   }

#ifdef MFD_CLOEXEC
   if (fd < 0)
      fd = memfd_create("nvc-sim-output", MFD_CLOEXEC);
#endif

   if (fd < 0) {
      FILE *f = tmpfile();
      if (f == NULL)
         return;
      fd = dup(fileno(f));
      fclose(f);
   }

   fflush(stdout);
   fflush(stderr);
   dup2(fd, STDOUT_FILENO);
   dup2(fd, STDERR_FILENO);

   w->out_fd = fd;
}

static void write_output(worker_t *w, json_writer_t *jw)
{
   if (w->out_fd < 0)
      return;

   fflush(stdout);
   fflush(stderr);

   struct stat st;
   if (fstat(w->out_fd, &st) < 0 || st.st_size <= w->out_off)
      return;

   const size_t total = st.st_size - w->out_off;
   const size_t take = MIN(total, OUTPUT_LIMIT);

   char *buf = xmalloc(take);
   const ssize_t n = pread(w->out_fd, buf, take, w->out_off);
   if (n > 0) {
      jw_key(jw, "sim_output");
      jw_stringn(jw, buf, n);

      if (total > take) {
         jw_key(jw, "sim_output_truncated");
         jw_bool(jw, true);
         jw_key(jw, "sim_output_total_bytes");
         jw_int(jw, total);

         if (w->files_dir != NULL) {
            char *name = xasprintf("%s/output.log", w->files_dir);
            jw_key(jw, "sim_output_log");
            jw_string(jw, name);
            free(name);
            jw_key(jw, "sim_output_next_offset");
            jw_int(jw, w->out_off + n);
         }
      }
   }

   free(buf);
   w->out_off = st.st_size;
}

////////////////////////////////////////////////////////////////////////////////
// Operations

static void write_state(worker_t *w, json_writer_t *jw)
{
   unsigned delta = 0;
   const int64_t now = model_now(w->model, &delta);

   char timestr[32];
   mcp_format_time(now, timestr, sizeof(timestr));

   jw_key(jw, "now_fs");
   jw_int(jw, now);
   jw_key(jw, "now");
   jw_string(jw, timestr);
   jw_key(jw, "delta");
   jw_int(jw, delta);

   jw_key(jw, "next_event_fs");
   const int64_t next = model_next_time(w->model);
   if (next == INT64_MAX)
      jw_null(jw);
   else
      jw_int(jw, next);
}

static void send_progress(worker_t *w)
{
   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);
   jw_begin_object(&jw);
   jw_key(&jw, "event");
   jw_string(&jw, "progress");
   jw_key(&jw, "now_fs");
   jw_int(&jw, model_now(w->model, NULL));
   jw_end_object(&jw);
   ctl_send(w, tb);
}

static void run_finish(worker_t *w, const char *reason)
{
   const uint64_t wall_ms = (get_timestamp_us() - w->run.start_us) / 1000;

   // Only a run that reached its target moves the reference point for
   // the next relative duration beyond the time actually simulated
   w->last_target = !strcmp(reason, "time") ? w->run.stop
      : (uint64_t)model_now(w->model, NULL);

   if (!w->ended && (!strcmp(reason, "finished")
                     || !strcmp(reason, "terminated"))) {
      w->ended = true;
      if (!model_is_terminated(w->model))
         model_end(w->model);
   }

   w->run.active = false;
   w->last_reason = reason;
   w->last_wall_ms = wall_ms;

   if (w->run.background)
      return;   // Read later with the status operation

   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);

   begin_reply(&jw, true);
   jw_key(&jw, "result");
   jw_begin_object(&jw);
   jw_key(&jw, "stopped_because");
   jw_string(&jw, reason);
   write_state(w, &jw);
   jw_key(&jw, "wall_ms");
   jw_int(&jw, wall_ms);
   if (model_is_terminated(w->model)) {
      jw_key(&jw, "exit_status");
      jw_int(&jw, model_exit_status(w->model));
   }
   write_messages(w, &jw);
   write_output(w, &jw);

   // Say what to do next: the reasons are easy to misread
   const char *hint = NULL;
   const unsigned bad = w->counts[DIAG_ERROR] + w->counts[DIAG_FAILURE]
      + w->counts[DIAG_FATAL];
   if (!strcmp(reason, "wall_budget")) {
      if (bad > 100 && w->pause_level > DIAG_ERROR)
         hint = "The real time limit was reached and the design reported many "
            "errors during this call (see counts). Use pause_on error to stop "
            "at the first one, or give duration or until to bound the run.";
      else
         hint = "The real time limit was reached with the design still "
            "running. Call nvc_sim_run again to continue, or give duration "
            "or until to stop at a chosen simulated time. For a long run "
            "use background true and follow it with nvc_sim_wait.";
   }
   else if (!strcmp(reason, "severity"))
      hint = "Stopped at the first message of the requested severity: read "
         "messages and signals now, then call nvc_sim_run again to continue.";
   else if (!strcmp(reason, "terminated"))
      hint = "The simulation cannot continue. Signals can still be read; open "
         "a new simulation with nvc_sim_open to run it again.";
   else if (!strcmp(reason, "finished"))
      hint = "No events remain, so the design has finished. Signals can still "
         "be read.";

   if (hint != NULL) {
      jw_key(&jw, "hint");
      jw_string(&jw, hint);
   }

   jw_end_object(&jw);
   jw_end_object(&jw);

   ctl_send(w, tb);
}

// Advance the run by one short slice of work
static void run_step(worker_t *w)
{
   if (model_is_terminated(w->model))
      return run_finish(w, "terminated");
   else if (w->pause_requested)
      return run_finish(w, "severity");

   files_check(w, false);

   const uint64_t now_us = get_timestamp_us();
   if (w->run.max_wall_ms > 0
       && (int64_t)((now_us - w->run.start_us) / 1000) >= w->run.max_wall_ms)
      return run_finish(w, "wall_budget");

   if (w->run.progress && now_us - w->run.last_progress_us
       >= PROGRESS_PERIOD_US) {
      send_progress(w);
      w->run.last_progress_us = now_us;
   }

   const bool done = model_run_cycles(w->model, w->run.stop, w->run.slice,
                                      &w->pause_requested);

   // Aim for slices short enough to notice a cancellation promptly
   const uint64_t took = get_timestamp_us() - now_us;
   if (took < SLICE_TARGET_US / 4 && w->run.slice < (1u << 20))
      w->run.slice *= 2;
   else if (took > SLICE_TARGET_US * 2 && w->run.slice > 1)
      w->run.slice /= 2;

   if (done) {
      if (model_is_terminated(w->model))
         run_finish(w, "terminated");
      else if (model_is_finished(w->model))
         run_finish(w, "finished");
      else
         run_finish(w, "time");
   }
}

static void op_run(worker_t *w, const json_t *req)
{
   if (w->run.active) {
      reply_error(w, "A run is already in progress: wait for it with "
                  "nvc_sim_wait or end it with nvc_sim_stop");
      return;
   }

   int64_t max_wall_ms = 10000, until_fs = -1, duration_fs = -1;
   json_as_int(json_get(req, "max_wall_ms"), &max_wall_ms);
   json_as_int(json_get(req, "until_fs"), &until_fs);
   json_as_int(json_get(req, "duration_fs"), &duration_fs);

   // A duration counts from the end of the previous run, not from the
   // time of the last event, so two runs of 100 ns advance 200 ns
   int64_t stop_fs = until_fs;
   if (duration_fs >= 0) {
      const int64_t base = MAX((int64_t)w->last_target,
                               model_now(w->model, NULL));
      stop_fs = duration_fs > INT64_MAX - base ? INT64_MAX : base + duration_fs;
   }

   bool progress = false, background = false;
   json_as_bool(json_get(req, "progress"), &progress);
   json_as_bool(json_get(req, "background"), &background);

   const char *pause = json_string(json_get(req, "pause_on"));
   w->pause_level = DIAG_FATAL + 1;
   if (pause != NULL) {
      if (!strcmp(pause, "note"))         w->pause_level = DIAG_NOTE;
      else if (!strcmp(pause, "warning")) w->pause_level = DIAG_WARN;
      else if (!strcmp(pause, "error"))   w->pause_level = DIAG_ERROR;
      else if (!strcmp(pause, "failure")) w->pause_level = DIAG_FAILURE;
   }

   reset_messages(w);

   if (!w->started) {
      w->started = true;
      model_start(w->model);
   }

   w->run.active = true;
   w->run.background = background;
   w->run.progress = progress && !background;
   w->run.stop = stop_fs < 0 ? (uint64_t)INT64_MAX : (uint64_t)stop_fs;
   w->run.max_wall_ms = max_wall_ms;
   w->run.start_us = w->run.last_progress_us = get_timestamp_us();
   w->run.start_fs = model_now(w->model, NULL);
   w->run.slice = 16;
   w->last_reason = NULL;

   if (background) {
      LOCAL_TEXT_BUF tb = tb_new();
      json_writer_t jw;
      jw_init(&jw, tb);
      begin_reply(&jw, true);
      jw_key(&jw, "result");
      jw_begin_object(&jw);
      jw_key(&jw, "state");
      jw_string(&jw, "running");
      write_state(w, &jw);
      jw_key(&jw, "hint");
      jw_string(&jw, "The run continues in the background. Call nvc_sim_wait "
                "to wait for it to stop or report a problem, nvc_sim_status "
                "to look without waiting, or nvc_sim_stop to end it.");
      jw_end_object(&jw);
      jw_end_object(&jw);
      ctl_send(w, tb);
   }
}

// Called for a request that arrives while a run is in progress
static void op_stop(worker_t *w)
{
   if (w->run.active)
      run_finish(w, "cancelled");
}

static void op_status(worker_t *w, const json_t *req)
{
   int64_t since = 0;
   json_as_int(json_get(req, "since_seq"), &since);

   const char *state;
   if (w->run.active)
      state = "running";
   else if (model_is_terminated(w->model))
      state = "terminated";
   else if (w->started && model_is_finished(w->model))
      state = "finished";
   else
      state = "idle";

   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);

   begin_reply(&jw, true);
   jw_key(&jw, "result");
   jw_begin_object(&jw);
   jw_key(&jw, "state");
   jw_string(&jw, state);

   if (w->last_reason != NULL) {
      jw_key(&jw, "stopped_because");
      jw_string(&jw, w->last_reason);
   }

   write_state(w, &jw);

   const uint64_t now_us = get_timestamp_us();
   const int64_t now_fs = model_now(w->model, NULL);

   jw_key(&jw, "wall_ms");
   jw_int(&jw, w->run.active ? (now_us - w->run.start_us) / 1000
          : w->last_wall_ms);

   if (w->run.active) {
      const uint64_t wall_us = MAX(now_us - w->run.start_us, 1);
      const int64_t advanced = now_fs - w->run.start_fs;
      const int64_t rate = (int64_t)((double)advanced * 1e6 / wall_us);

      jw_key(&jw, "run_started_at_fs");
      jw_int(&jw, w->run.start_fs);
      jw_key(&jw, "sim_fs_per_second");
      jw_int(&jw, rate);

      if (w->run.stop != (uint64_t)INT64_MAX) {
         jw_key(&jw, "target_fs");
         jw_int(&jw, w->run.stop);

         const int64_t span = (int64_t)w->run.stop - w->run.start_fs;
         if (span > 0) {
            jw_key(&jw, "percent");
            jw_int(&jw, MIN(100, advanced * 100 / span));
         }
         if (rate > 0 && (int64_t)w->run.stop > now_fs) {
            jw_key(&jw, "eta_ms");
            jw_int(&jw, (int64_t)(((double)w->run.stop - now_fs)
                                  * 1000.0 / rate));
         }
      }
   }

   if (model_is_terminated(w->model)) {
      jw_key(&jw, "exit_status");
      jw_int(&jw, model_exit_status(w->model));
   }

   write_messages_since(w, &jw, since);
   write_output(w, &jw);
   write_files(w, &jw);

   jw_end_object(&jw);
   jw_end_object(&jw);
   ctl_send(w, tb);
}

////////////////////////////////////////////////////////////////////////////////
// Files: the output log and waveform dumps
//
// Everything that can grow without bound is written to disk with a size
// limit that is checked while the simulation runs, so a long run cannot
// fill the disk and the result stays readable after the worker has gone.

static uint64_t prefix_bytes(const char *dir, const char *prefix)
{
   DIR *d = opendir(dir);
   if (d == NULL)
      return 0;

   uint64_t total = 0;
   const size_t plen = strlen(prefix);
   struct dirent *e;
   while ((e = readdir(d)) != NULL) {
      if (strncmp(e->d_name, prefix, plen) != 0)
         continue;

      char *path = xasprintf("%s/%s", dir, e->d_name);
      struct stat st;
      if (stat(path, &st) == 0)
         total += st.st_size;
      free(path);
   }

   closedir(d);
   return total;
}

static void wave_finish(worker_t *w)
{
   if (w->wave == NULL)
      return;

   wave_dumper_close(w->wave);

   w->old_waves = xrealloc_array(w->old_waves, w->nold_waves + 1,
                                 sizeof(wave_dumper_t *));
   w->old_waves[w->nold_waves++] = w->wave;
   w->wave = NULL;

   w->wave_done = xrealloc_array(w->wave_done, w->nwave_done + 1,
                                 sizeof(char *));
   w->wave_done[w->nwave_done++] = w->wave_file;
   w->wave_file = NULL;
}

// Called regularly while running: measure the files and enforce limits
static void files_check(worker_t *w, bool force)
{
   const uint64_t now_us = get_timestamp_us();
   if (!force && now_us - w->files_check_us < FILES_CHECK_US)
      return;

   if (w->wave != NULL) {
      char *base = xstrdup(w->wave_file);
      char *slash = strrchr(base, '/');
      *slash = '\0';

      const uint64_t bytes = prefix_bytes(base, slash + 1);
      free(base);

      if (w->files_check_us != 0 && now_us > w->files_check_us && bytes >=
          w->wave_bytes) {
         const double dt = (now_us - w->files_check_us) / 1e6;
         w->wave_rate = (uint64_t)((bytes - w->wave_bytes) / dt);
      }
      w->wave_bytes = bytes;

      if (bytes > w->wave_max) {
         char *file = xstrdup(w->wave_file);
         wave_finish(w);
         w->wave_quota_hit = true;
         notef("waveform dump %s stopped at its size limit of %"PRIu64
               " bytes: the simulation continues without it", file,
               w->wave_max);
         free(file);
      }
   }

   if (w->out_fd >= 0 && !w->log_full) {
      struct stat st;
      if (fstat(w->out_fd, &st) == 0 && (uint64_t)st.st_size > w->log_max) {
         w->log_full = true;
         notef("the output log reached its limit of %"PRIu64" bytes: "
               "further output of the design is discarded", w->log_max);

         const int devnull = open("/dev/null", O_WRONLY);
         if (devnull >= 0) {
            fflush(stdout);
            fflush(stderr);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
         }
      }
   }

   w->files_check_us = now_us;
}

static void write_files(worker_t *w, json_writer_t *jw)
{
   if (w->files_dir == NULL)
      return;

   files_check(w, true);

   jw_key(jw, "files");
   jw_begin_object(jw);

   jw_key(jw, "directory");
   jw_string(jw, w->files_dir);

   if (w->out_fd >= 0) {
      struct stat st;
      jw_key(jw, "output_log");
      jw_begin_object(jw);
      char *log = xasprintf("%s/output.log", w->files_dir);
      jw_key(jw, "file");
      jw_string(jw, log);
      free(log);
      jw_key(jw, "bytes");
      jw_int(jw, fstat(w->out_fd, &st) == 0 ? st.st_size : 0);
      jw_key(jw, "limit_reached");
      jw_bool(jw, w->log_full);
      jw_end_object(jw);
   }

   jw_key(jw, "wave");
   jw_begin_object(jw);
   jw_key(jw, "active");
   jw_bool(jw, w->wave != NULL);
   if (w->wave != NULL) {
      jw_key(jw, "file");
      jw_string(jw, w->wave_file);
      jw_key(jw, "bytes");
      jw_int(jw, w->wave_bytes);
      jw_key(jw, "bytes_per_second");
      jw_int(jw, w->wave_rate);
      jw_key(jw, "max_bytes");
      jw_int(jw, w->wave_max);
   }
   jw_key(jw, "quota_reached");
   jw_bool(jw, w->wave_quota_hit);
   jw_key(jw, "finished");
   jw_begin_array(jw);
   for (size_t i = 0; i < w->nwave_done; i++)
      jw_string(jw, w->wave_done[i]);
   jw_end_array(jw);
   jw_end_object(jw);

   write_live(w, jw);

   jw_end_object(jw);
}

// Turn a design path such as /dut/cnt or /dut/* into the pattern the
// waveform filter matches: the full lower case path with colons
static char *wave_pattern(worker_t *w, const char *path)
{
   if (path[0] == ':')
      return xstrdup(path);

   LOCAL_TEXT_BUF tb = tb_new();
   tb_printf(tb, ":%s", istr(tree_ident(tree_stmt(w->top, 0))));
   for (const char *p = path; *p != '\0'; p++) {
      if (*p == '/') {
         if (p[1] != '\0' || p == path)
            tb_append(tb, ':');
      }
      else
         tb_append(tb, *p);
   }

   tb_downcase(tb);
   return tb_claim(tb);
}

static bool valid_file_name(const char *name)
{
   const size_t len = strlen(name);
   if (len == 0 || len > 64 || name[0] == '.')
      return false;

   for (const char *p = name; *p != '\0'; p++) {
      if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.')
         return false;
   }

   return true;
}

// The include and exclude entries as the patterns the waveform filter
// matches, which are full lower case paths with colons
static char *wave_filter_pattern(worker_t *w, const char *entry)
{
   if (strchr(entry, '/') == NULL && entry[0] != ':') {
      // A name or glob without a path matches in every region
      LOCAL_TEXT_BUF tb = tb_new();
      tb_printf(tb, "*:%s", entry);
      tb_downcase(tb);
      return tb_claim(tb);
   }

   return wave_pattern(w, entry);
}

static void op_wave_start(worker_t *w, const json_t *req)
{
   if (w->files_dir == NULL) {
      reply_error(w, "This simulation has no file directory");
      return;
   }
   else if (w->wave != NULL || live_active(w)) {
      reply_error(w, "A recording is already active: end it with "
                  "nvc_wave_stop, which also lets you start another");
      return;
   }

   const char *to = json_string(json_get(req, "to")) ?: "both";
   const bool want_file = strcmp(to, "memory") != 0;
   const bool want_memory = strcmp(to, "file") != 0;

   const char *name = json_string(json_get(req, "name"));
   char *auto_name = NULL;
   if (name == NULL)
      name = auto_name = xasprintf("wave-%u", w->wave_seq + 1);
   else if (!valid_file_name(name)) {
      reply_error(w, "Invalid name '%s': use letters, digits, - _ and . "
                  "(at most 64 characters, not starting with a dot)", name);
      return;
   }

   int64_t max_bytes = DEFAULT_WAVE_MAX, max_changes = DEFAULT_TRACE_CHANGES;
   json_as_int(json_get(req, "max_bytes"), &max_bytes);
   json_as_int(json_get(req, "max_changes"), &max_changes);
   max_bytes = CLAMP_INT(max_bytes, 1, (int64_t)MAX_WAVE_MAX);

   char *file = xasprintf("%s/%s%s", w->files_dir, name,
                          strstr(name, ".fst") ? "" : ".fst");
   free(auto_name);

   struct stat st;
   if (want_file && stat(file, &st) == 0) {
      reply_error(w, "The file %s already exists: choose another name", file);
      free(file);
      return;
   }

   const json_t *incl = json_get(req, "include");
   const json_t *excl = json_get(req, "exclude");

   // The signals selected, for the in-memory recording and to check the
   // patterns before anything is started
   design_obj_t **picked = NULL;
   size_t npicked = 0;
   LOCAL_TEXT_BUF err = tb_new();
   if (!live_pick(w, incl, excl, &picked, &npicked, err)) {
      reply_error(w, "%s", tb_get(err));
      free(file);
      return;
   }

   if (npicked == 0) {
      reply_error(w, "Every signal selected by include was removed by "
                  "exclude: nothing would be recorded");
      free(picked);
      free(file);
      return;
   }

   char *memory_note = NULL;
   if (want_memory) {
      tb_rewind(err);
      if (!live_start(w, picked, npicked, max_changes, err)) {
         if (!want_file) {
            reply_error(w, "Cannot record in memory: %s", tb_get(err));
            free(picked);
            free(file);
            return;
         }

         // The file alone still works for a large selection
         memory_note = xasprintf("Not recorded in memory: %s. The file "
                                 "has everything, and can be read once "
                                 "finished with nvc_wave_stop", tb_get(err));
      }
   }

   free(picked);

   if (want_file) {
      wave_clear_globs();
      for (int pass = 0; pass < 2; pass++) {
         const json_t *list = pass == 0 ? incl : excl;
         const size_t n = json_is(list, JSON_ARRAY) ? list->array.count : 0;
         for (size_t i = 0; i < n; i++) {
            const char *pat = json_string(json_at(list, i));
            if (pat == NULL)
               continue;

            char *glob = wave_filter_pattern(w, pat);
            if (pass == 0)
               wave_include_glob(glob);
            else
               wave_exclude_glob(glob);
            free(glob);
         }
      }

      w->wave = wave_dumper_new(file, NULL, w->top, WAVE_FORMAT_FST);
      w->wave_file = file;
      w->wave_max = max_bytes;
      w->wave_quota_hit = false;
      w->wave_bytes = w->wave_rate = 0;
      w->wave_seq++;

      // The initial values are those at the current time
      wave_dumper_restart(w->wave, w->model, w->jit);
      wave_clear_globs();
   }
   else
      free(file);

   w->files_check_us = 0;

   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);
   begin_reply(&jw, true);
   jw_key(&jw, "result");
   jw_begin_object(&jw);
   jw_key(&jw, "signals");
   jw_int(&jw, npicked);
   if (w->wave != NULL) {
      jw_key(&jw, "file");
      jw_string(&jw, w->wave_file);
      jw_key(&jw, "max_bytes");
      jw_int(&jw, max_bytes);
   }
   if (memory_note != NULL) {
      jw_key(&jw, "hint");
      jw_string(&jw, memory_note);
      free(memory_note);
   }
   write_state(w, &jw);
   write_files(w, &jw);
   jw_end_object(&jw);
   jw_end_object(&jw);
   ctl_send(w, tb);
}

static void op_wave_stop(worker_t *w)
{
   if (w->wave == NULL && !live_active(w)) {
      reply_error(w, "No recording is active");
      return;
   }

   char *file = w->wave_file ? xstrdup(w->wave_file) : NULL;
   uint64_t bytes = 0;

   if (w->wave != NULL) {
      wave_finish(w);

      char *base = xstrdup(file);
      char *slash = strrchr(base, '/');
      *slash = '\0';
      bytes = prefix_bytes(base, slash + 1);
      free(base);
   }

   live_stop(w);

   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);
   begin_reply(&jw, true);
   jw_key(&jw, "result");
   jw_begin_object(&jw);
   if (file != NULL) {
      jw_key(&jw, "file");
      jw_string(&jw, file);
      jw_key(&jw, "bytes");
      jw_int(&jw, bytes);
   }
   write_state(w, &jw);
   write_files(w, &jw);
   jw_end_object(&jw);
   jw_end_object(&jw);
   ctl_send(w, tb);

   free(file);
}

// The cursor is a plain count of items already returned
static char *cursor_string(int64_t next)
{
   return xasprintf("%"PRIi64, next);
}

static void op_list(worker_t *w, const json_t *req)
{
   const char *path = json_string(json_get(req, "path")) ?: "/";

   int64_t limit = 50, cursor = 0;
   json_as_int(json_get(req, "limit"), &limit);
   const char *cursor_str = json_string(json_get(req, "cursor"));
   if (cursor_str != NULL)
      cursor = strtoll(cursor_str, NULL, 10);
   if (cursor < 0)
      cursor = 0;
   limit = CLAMP_INT(limit, 1, MAX_LIST_LIMIT);

   design_obj_t *region = find_obj(w, path);
   if (region == NULL || region->kind != OBJ_REGION) {
      LOCAL_TEXT_BUF hint = tb_new();
      suggest_paths(w, path, hint);
      reply_error(w, "Region '%s' was not found%s", path, tb_get(hint));
      return;
   }

   const int self = region - w->objs;

   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);

   begin_reply(&jw, true);
   jw_key(&jw, "result");
   jw_begin_object(&jw);
   jw_key(&jw, "path");
   jw_string(&jw, istr(region->path));
   jw_key(&jw, "items");
   jw_begin_array(&jw);

   int64_t seen = 0, shown = 0;
   for (size_t i = 0; i < w->nobjs; i++) {
      if (w->objs[i].parent != self || (int)i == self)
         continue;

      if (seen++ < cursor)
         continue;
      else if (shown >= limit)
         continue;

      write_obj(w, &jw, &(w->objs[i]));
      shown++;
   }

   jw_end_array(&jw);
   jw_key(&jw, "total");
   jw_int(&jw, seen);
   jw_key(&jw, "truncated");
   jw_bool(&jw, cursor + shown < seen);
   if (cursor + shown < seen) {
      char *next = cursor_string(cursor + shown);
      jw_key(&jw, "next_cursor");
      jw_string(&jw, next);
      free(next);
   }
   jw_end_object(&jw);
   jw_end_object(&jw);

   ctl_send(w, tb);
}

// ident_glob does not accept an empty match for * and has no ?, so use
// fnmatch.  A pattern without a slash is matched against the last path
// component.
static bool obj_matches(const design_obj_t *o, const char *glob, bool by_leaf)
{
   return fnmatch(glob, istr(by_leaf ? o->name : o->path), FNM_CASEFOLD) == 0;
}

static void op_search(worker_t *w, const json_t *req)
{
   const char *pattern = json_string(json_get(req, "pattern"));
   const char *kind = json_string(json_get(req, "kind")) ?: "any";

   int64_t limit = 50, cursor = 0;
   json_as_int(json_get(req, "limit"), &limit);
   const char *cursor_str = json_string(json_get(req, "cursor"));
   if (cursor_str != NULL)
      cursor = strtoll(cursor_str, NULL, 10);
   if (cursor < 0)
      cursor = 0;
   limit = CLAMP_INT(limit, 1, MAX_LIST_LIMIT);

   if (pattern == NULL) {
      reply_error(w, "pattern is required");
      return;
   }

   const bool want_signals = strcmp(kind, "region") != 0;
   const bool want_regions = strcmp(kind, "signal") != 0;

   // A pattern without a slash is matched against the last component
   const bool by_leaf = strchr(pattern, '/') == NULL;

   LOCAL_TEXT_BUF glob = tb_new();
   tb_cat(glob, pattern);
   tb_downcase(glob);

   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);

   begin_reply(&jw, true);
   jw_key(&jw, "result");
   jw_begin_object(&jw);
   jw_key(&jw, "pattern");
   jw_string(&jw, pattern);
   jw_key(&jw, "items");
   jw_begin_array(&jw);

   int64_t seen = 0, shown = 0;
   for (size_t i = 0; i < w->nobjs; i++) {
      design_obj_t *o = &(w->objs[i]);

      if (o->kind == OBJ_SIGNAL ? !want_signals : !want_regions)
         continue;

      if (!obj_matches(o, tb_get(glob), by_leaf))
         continue;

      if (seen++ < cursor || shown >= limit)
         continue;

      write_obj(w, &jw, o);
      shown++;
   }

   jw_end_array(&jw);
   jw_key(&jw, "total");
   jw_int(&jw, seen);
   jw_key(&jw, "truncated");
   jw_bool(&jw, cursor + shown < seen);
   if (cursor + shown < seen) {
      char *next = cursor_string(cursor + shown);
      jw_key(&jw, "next_cursor");
      jw_string(&jw, next);
      free(next);
   }
   jw_end_object(&jw);
   jw_end_object(&jw);

   ctl_send(w, tb);
}

// Vectors of characters print as ('0', '0', '0', '1') which wastes a
// lot of space: return them as 0001 instead, or in the radix asked for.
// Returns false if the text is not in a form this understands.
static bool format_vector(const char *printed, int width, const char *type,
                          const char *radix, text_buf_t *out)
{
   LOCAL_TEXT_BUF bits = tb_new();

   const char *p = printed;
   if (*p == '"') {
      for (p++; *p && *p != '"'; p++)
         tb_append(bits, *p);
   }
   else if (*p == '(') {
      while (*p) {
         if (*p == '\'' && p[1] && p[2] == '\'') {
            tb_append(bits, p[1]);
            p += 3;
         }
         else
            p++;
      }
   }
   else
      return false;

   if ((int)tb_len(bits) != width)
      return false;

   bool binary = true;
   for (int i = 0; i < width; i++) {
      const char c = tb_get(bits)[i];
      binary &= c == '0' || c == '1';
   }

   if (binary && strcmp(radix, "hex") == 0) {
      tb_cat(out, "0x");
      const int ndigits = (width + 3) / 4;
      for (int d = 0; d < ndigits; d++) {
         int nibble = 0;
         for (int b = 0; b < 4; b++) {
            // Bit positions counted from the right, missing ones are zero
            const int pos = width - 1 - ((ndigits - 1 - d) * 4 + (3 - b));
            nibble = (nibble << 1)
               | (pos >= 0 && pos < width && tb_get(bits)[pos] == '1');
         }
         tb_append(out, "0123456789ABCDEF"[nibble]);
      }
      return true;
   }
   else if (binary && strcmp(radix, "dec") == 0 && width <= 63) {
      uint64_t value = 0;
      for (int i = 0; i < width; i++)
         value = (value << 1) | (tb_get(bits)[i] == '1');

      if (strcmp(type, "SIGNED") == 0 && width > 0 && tb_get(bits)[0] == '1')
         tb_printf(out, "%"PRIi64, (int64_t)value - ((int64_t)1 << width));
      else
         tb_printf(out, "%"PRIu64, value);
      return true;
   }

   tb_cat(out, tb_get(bits));
   return true;
}

static void op_get(worker_t *w, const json_t *req)
{
   const json_t *paths = json_get(req, "paths");
   const char *radix = json_string(json_get(req, "radix")) ?: "default";

   print_flags_t flags = 0;
   if (!strcmp(radix, "hex"))
      flags |= PRINT_F_HEX;
   else if (!strcmp(radix, "bin"))
      flags |= PRINT_F_BIN;
   else if (!strcmp(radix, "dec"))
      flags |= PRINT_F_DEC;

   if (!json_is(paths, JSON_ARRAY)) {
      reply_error(w, "paths is required");
      return;
   }

   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);

   begin_reply(&jw, true);
   jw_key(&jw, "result");
   jw_begin_object(&jw);
   jw_key(&jw, "values");
   jw_begin_array(&jw);

   for (size_t i = 0; i < paths->array.count; i++) {
      const char *path = json_string(paths->array.items[i]);
      design_obj_t *o = path ? find_obj(w, path) : NULL;

      if (o == NULL) {
         LOCAL_TEXT_BUF hint = tb_new();
         suggest_paths(w, path ?: "", hint);
         reply_error(w, "Signal '%s' was not found%s", path ?: "", tb_get(hint));
         return;
      }
      else if (o->kind != OBJ_SIGNAL) {
         reply_error(w, "'%s' is a region, not a signal. Use nvc_design_list "
                     "to see the signals inside it", path);
         return;
      }
      else if (!obj_printer(w, o)) {
         reply_error(w, "Signals of type %s cannot be displayed",
                     type_pp(tree_type(o->signal->where)));
         return;
      }

      jw_begin_object(&jw);
      jw_key(&jw, "path");
      jw_string(&jw, istr(o->path));
      type_t type = tree_type(o->signal->where);
      const char *printed = print_signal(o->printer, o->signal, flags);

      LOCAL_TEXT_BUF compact = tb_new();
      const bool is_vector = type_is_character_array(type)
         && format_vector(printed, signal_width(o->signal), type_pp(type),
                          radix, compact);

      jw_key(&jw, "value");
      jw_string(&jw, is_vector ? tb_get(compact) : printed);
      jw_key(&jw, "type");
      jw_string(&jw, type_pp(type));
      jw_key(&jw, "width");
      jw_int(&jw, signal_width(o->signal));
      jw_key(&jw, "forced");
      jw_bool(&jw, !!(o->signal->nexus.flags & NET_F_FORCED));
      jw_end_object(&jw);
   }

   jw_end_array(&jw);
   write_state(w, &jw);
   jw_end_object(&jw);
   jw_end_object(&jw);

   ctl_send(w, tb);
}

static void op_set(worker_t *w, const json_t *req)
{
   const char *path = json_string(json_get(req, "path"));
   const char *value = json_string(json_get(req, "value"));
   const char *mode = json_string(json_get(req, "mode")) ?: "force";

   if (path == NULL) {
      reply_error(w, "path is required");
      return;
   }

   const bool release = strcmp(mode, "release") == 0;

   if (release && !strcmp(path, "*")) {
      unsigned count = 0;
      for (size_t i = 0; i < w->nobjs; i++) {
         rt_signal_t *s = w->objs[i].signal;
         if (s != NULL && (s->nexus.flags & NET_F_FORCED)) {
            release_signal(w->model, s, 0, signal_width(s), RELEASE_RECOMPUTE);
            count++;
         }
      }

      LOCAL_TEXT_BUF tb = tb_new();
      json_writer_t jw;
      jw_init(&jw, tb);
      begin_reply(&jw, true);
      jw_key(&jw, "result");
      jw_begin_object(&jw);
      jw_key(&jw, "path");
      jw_string(&jw, "*");
      jw_key(&jw, "mode");
      jw_string(&jw, "release");
      jw_key(&jw, "released");
      jw_int(&jw, count);
      jw_end_object(&jw);
      jw_end_object(&jw);
      ctl_send(w, tb);
      return;
   }

   design_obj_t *o = find_obj(w, path);
   if (o == NULL) {
      LOCAL_TEXT_BUF hint = tb_new();
      suggest_paths(w, path, hint);
      reply_error(w, "Signal '%s' was not found%s", path, tb_get(hint));
      return;
   }
   else if (o->kind != OBJ_SIGNAL) {
      reply_error(w, "'%s' is a region, not a signal", path);
      return;
   }

   rt_signal_t *s = o->signal;
   type_t type = tree_type(s->where);
   unsigned released = 0;

   if (release) {
      if (!(s->nexus.flags & NET_F_FORCED)) {
         reply_error(w, "Signal '%s' is not forced", path);
         return;
      }

      release_signal(w->model, s, 0, signal_width(s), RELEASE_RECOMPUTE);
      released = 1;
   }
   else {
      if (value == NULL) {
         reply_error(w, "value is required to force a signal");
         return;
      }

      parsed_value_t parsed;
      if (!parse_value(type, value, &parsed)) {
         reply_error(w, "Value '%s' is not valid for type %s. Examples: '1' "
                     "for a bit or std_logic, 42 for an integer, \"1010\" for "
                     "a vector", value, type_pp(type));
         return;
      }

      if (type_is_scalar(type))
         force_signal(w->model, s, &parsed.integer, 0, 1);
      else if (type_is_character_array(type)) {
         const int width = signal_width(s);
         if ((int)parsed.enums->count != width) {
            reply_error(w, "Signal '%s' has %d elements but the value has %u",
                        path, width, parsed.enums->count);
            free(parsed.enums);
            return;
         }

         force_signal(w->model, s, parsed.enums->values, 0, width);
         free(parsed.enums);
      }
      else {
         reply_error(w, "Signals of type %s cannot be forced", type_pp(type));
         return;
      }
   }

   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);
   begin_reply(&jw, true);
   jw_key(&jw, "result");
   jw_begin_object(&jw);
   jw_key(&jw, "path");
   jw_string(&jw, istr(o->path));
   jw_key(&jw, "mode");
   jw_string(&jw, release ? "release" : "force");
   if (release) {
      jw_key(&jw, "released");
      jw_int(&jw, released);
   }
   jw_end_object(&jw);
   jw_end_object(&jw);
   ctl_send(w, tb);
}

static void op_close(worker_t *w)
{
   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);
   begin_reply(&jw, true);
   jw_end_object(&jw);
   ctl_send(w, tb);
}

////////////////////////////////////////////////////////////////////////////////
// Traces: a bounded record of signal changes
//
// Waveform files have to be asked for before a run.  A trace lets the
// agent look back instead: it keeps the most recent changes of chosen
// signals in a ring buffer, so what happened just before a failure can be
// read without running again.  Values are those at the end of each time
// step, as in a waveform dump.

typedef struct {
   trace_t  *trace;
   unsigned  index;
} trace_sig_t;

struct _trace {
   unsigned       id;
   bool           stopped;   // Watches removed, buffer still readable
   unsigned       nsigs;
   design_obj_t **objs;
   rt_watch_t   **watches;
   trace_sig_t   *ctx;
   uint8_t       *buf;
   size_t         stride;
   size_t         cap;
   size_t         head;      // Total records written
   size_t         count;     // Records held
   size_t         dropped;   // Records overwritten
};

#define TRACE_HEADER 16   // time (8), signal (4), length (4)

static void trace_record(trace_t *t, int64_t now, unsigned sig,
                         const void *data, size_t size)
{
   uint8_t *rec = t->buf + (t->head % t->cap) * t->stride;

   const uint32_t sig32 = sig, len32 = size;
   memcpy(rec, &now, sizeof(now));
   memcpy(rec + 8, &sig32, sizeof(sig32));
   memcpy(rec + 12, &len32, sizeof(len32));
   memcpy(rec + TRACE_HEADER, data, MIN(size, t->stride - TRACE_HEADER));

   t->head++;
   if (t->count < t->cap)
      t->count++;
   else
      t->dropped++;
}

static void trace_event_cb(uint64_t now, rt_signal_t *s, rt_watch_t *w,
                           void *user)
{
   trace_sig_t *ctx = user;
   trace_record(ctx->trace, now, ctx->index, signal_value(s), s->shared.size);
}

static print_flags_t radix_flags(const char *radix)
{
   if (!strcmp(radix, "hex"))
      return PRINT_F_HEX;
   else if (!strcmp(radix, "bin"))
      return PRINT_F_BIN;
   else if (!strcmp(radix, "dec"))
      return PRINT_F_DEC;
   else
      return 0;
}

static void format_raw(worker_t *w, design_obj_t *o, const void *data,
                       size_t size, const char *radix, text_buf_t *out)
{
   type_t type = tree_type(o->signal->where);
   const char *printed = print_raw(o->printer, data, size, radix_flags(radix));

   if (!type_is_character_array(type)
       || !format_vector(printed, signal_width(o->signal), type_pp(type),
                         radix, out))
      tb_cat(out, printed);
}

static bool glob_wanted(const char *p)
{
   return strpbrk(p, "*?") != NULL;
}

// Mark the signals an include or exclude entry names.  Returns the number
// of signals marked, or -1 after writing the reason to err.
static int mark_entry(worker_t *w, const char *entry, bool *mark, bool value,
                      text_buf_t *err)
{
   int count = 0;

   if (glob_wanted(entry)) {
      LOCAL_TEXT_BUF glob = tb_new();
      tb_cat(glob, entry);
      tb_downcase(glob);
      const bool by_leaf = strchr(entry, '/') == NULL;

      for (size_t i = 0; i < w->nobjs; i++) {
         design_obj_t *o = &(w->objs[i]);
         if (o->kind == OBJ_SIGNAL && obj_matches(o, tb_get(glob), by_leaf)) {
            mark[i] = value;
            count++;
         }
      }
   }
   else {
      design_obj_t *o = find_obj(w, entry);
      if (o == NULL) {
         LOCAL_TEXT_BUF hint = tb_new();
         suggest_paths(w, entry, hint);
         tb_printf(err, "Signal '%s' was not found%s", entry, tb_get(hint));
         return -1;
      }
      else if (o->kind != OBJ_SIGNAL) {
         tb_printf(err, "'%s' is a region, not a signal: add /* to include "
                   "everything below it", entry);
         return -1;
      }

      mark[o - w->objs] = value;
      count = 1;
   }

   return count;
}

static bool live_pick(worker_t *w, const json_t *include,
                      const json_t *exclude, design_obj_t ***picked,
                      size_t *npicked, text_buf_t *err)
{
   bool *mark = xcalloc_array(w->nobjs, sizeof(bool));
   const size_t ninc = json_is(include, JSON_ARRAY) ? include->array.count : 0;
   const size_t nexc = json_is(exclude, JSON_ARRAY) ? exclude->array.count : 0;

   if (ninc == 0) {
      for (size_t i = 0; i < w->nobjs; i++)
         mark[i] = w->objs[i].kind == OBJ_SIGNAL;
   }

   for (size_t i = 0; i < ninc; i++) {
      const char *entry = json_string(include->array.items[i]);
      if (entry == NULL)
         continue;

      const int n = mark_entry(w, entry, mark, true, err);
      if (n < 0) {
         free(mark);
         return false;
      }
      else if (n == 0) {
         tb_printf(err, "No signal matches '%s'. Find signals with "
                   "nvc_design_search", entry);
         free(mark);
         return false;
      }
   }

   for (size_t i = 0; i < nexc; i++) {
      const char *entry = json_string(exclude->array.items[i]);
      if (entry != NULL && mark_entry(w, entry, mark, false, err) < 0) {
         free(mark);
         return false;
      }
   }

   size_t n = 0;
   for (size_t i = 0; i < w->nobjs; i++)
      n += mark[i];

   *picked = xmalloc_array(MAX(n, 1), sizeof(design_obj_t *));
   *npicked = 0;
   for (size_t i = 0; i < w->nobjs; i++) {
      if (mark[i])
         (*picked)[(*npicked)++] = &(w->objs[i]);
   }

   free(mark);
   return true;
}

static void live_detach(worker_t *w, trace_t *t)
{
   if (t->stopped)
      return;

   for (unsigned i = 0; i < t->nsigs; i++)
      watch_free(w->model, t->watches[i]);

   t->stopped = true;
}

static void live_free(worker_t *w)
{
   trace_t *t = w->live;
   if (t == NULL)
      return;

   live_detach(w, t);
   free(t->objs);
   free(t->watches);
   free(t->ctx);
   free(t->buf);
   free(t);
   w->live = NULL;
}

static bool live_active(const worker_t *w)
{
   return w->live != NULL && !w->live->stopped;
}

// Stop recording changes.  What was recorded stays readable.
static void live_stop(worker_t *w)
{
   if (w->live != NULL)
      live_detach(w, w->live);
}

// Start the in-memory recording.  Returns false after saying why not.
static bool live_start(worker_t *w, design_obj_t **picked, size_t npicked,
                       int64_t max_changes, text_buf_t *err)
{
   if (npicked == 0) {
      tb_cat(err, "no signal selected");
      return false;
   }
   else if (npicked > MAX_TRACE_SIGNALS) {
      tb_printf(err, "%zu signals are selected but the in-memory recording "
                "holds at most %d: narrow include", npicked,
                MAX_TRACE_SIGNALS);
      return false;
   }

   size_t maxbytes = 0;
   for (size_t i = 0; i < npicked; i++) {
      design_obj_t *o = picked[i];
      const size_t bytes = o->signal->shared.size;

      if (bytes > MAX_TRACE_BYTES) {
         tb_printf(err, "signal '%s' is too wide for the in-memory "
                   "recording (%zu bytes, the limit is %d)", istr(o->path),
                   bytes, MAX_TRACE_BYTES);
         return false;
      }
      else if (!obj_printer(w, o)) {
         tb_printf(err, "signals of type %s cannot be displayed",
                   type_pp(tree_type(o->signal->where)));
         return false;
      }

      maxbytes = MAX(maxbytes, bytes);
   }

   live_free(w);

   max_changes = CLAMP_INT(max_changes, 100, MAX_TRACE_CHANGES);

   trace_t *t = xcalloc(sizeof(trace_t));
   t->id      = ++w->next_trace_id;
   t->nsigs   = npicked;
   t->stride  = TRACE_HEADER + ((maxbytes + 7) & ~(size_t)7);
   t->cap     = MIN((size_t)max_changes, TRACE_MEMORY_LIMIT / t->stride);
   t->objs    = xmalloc_array(npicked, sizeof(design_obj_t *));
   t->watches = xmalloc_array(npicked, sizeof(rt_watch_t *));
   t->ctx     = xmalloc_array(npicked, sizeof(trace_sig_t));
   t->buf     = xmalloc(t->cap * t->stride);

   const int64_t now = model_now(w->model, NULL);

   for (size_t i = 0; i < npicked; i++) {
      t->objs[i] = picked[i];
      t->ctx[i] = (trace_sig_t){ t, i };

      rt_signal_t *s = picked[i]->signal;
      t->watches[i] = watch_new(w->model, trace_event_cb, &(t->ctx[i]),
                                WATCH_POSTPONED, 1);
      model_set_event_cb(w->model, s, 0, signal_width(s), t->watches[i]);

      // The starting value, so the record is complete from here
      trace_record(t, now, i, signal_value(s), s->shared.size);
   }

   w->live = t;
   return true;
}

static void write_live(worker_t *w, json_writer_t *jw)
{
   const trace_t *t = w->live;

   jw_key(jw, "memory");
   jw_begin_object(jw);
   jw_key(jw, "active");
   jw_bool(jw, t != NULL && !t->stopped);
   if (t != NULL) {
      jw_key(jw, "signals");
      jw_int(jw, t->nsigs);
      jw_key(jw, "changes_kept");
      jw_int(jw, t->count);
      jw_key(jw, "changes_dropped");
      jw_int(jw, t->dropped);
      jw_key(jw, "capacity");
      jw_int(jw, t->cap);
      if (t->count > 0) {
         int64_t oldest;
         const size_t first = t->head - t->count;
         memcpy(&oldest, t->buf + (first % t->cap) * t->stride,
                sizeof(oldest));
         jw_key(jw, "oldest_time_fs");
         jw_int(jw, oldest);
      }
   }
   jw_end_object(jw);
}

// Read changes of recorded signals from the in-memory recording
static void op_wave_read(worker_t *w, const json_t *req)
{
   trace_t *t = w->live;
   if (t == NULL) {
      reply_error(w, "Nothing is recorded in memory. Start a recording with "
                  "nvc_wave_start (to memory or both), or give file to read "
                  "a finished waveform file");
      return;
   }

   int64_t from_fs = 0, to_fs = INT64_MAX, max_changes = 200;
   json_as_int(json_get(req, "from_fs"), &from_fs);
   json_as_int(json_get(req, "to_fs"), &to_fs);
   json_as_int(json_get(req, "max_changes"), &max_changes);
   max_changes = CLAMP_INT(max_changes, 1, 200000);

   const char *radix = json_string(json_get(req, "radix")) ?: "default";
   const json_t *paths = json_get(req, "paths");
   const size_t npaths = json_is(paths, JSON_ARRAY) ? paths->array.count : 0;

   unsigned *which = xmalloc_array(MAX(npaths, 1), sizeof(unsigned));
   for (size_t i = 0; i < npaths; i++) {
      const char *path = json_string(paths->array.items[i]);
      design_obj_t *o = path ? find_obj(w, path) : NULL;

      bool found = false;
      for (unsigned j = 0; o != NULL && j < t->nsigs; j++) {
         if (t->objs[j] == o) {
            which[i] = j;
            found = true;
         }
      }

      if (!found) {
         LOCAL_TEXT_BUF recorded = tb_new();
         for (unsigned j = 0; j < t->nsigs && j < 10; j++)
            tb_printf(recorded, "%s%s", j > 0 ? ", " : "",
                      istr(t->objs[j]->path));
         if (t->nsigs > 10)
            tb_cat(recorded, ", ...");

         reply_error(w, "Signal '%s' is not in the in-memory recording. It "
                     "holds: %s", path ?: "", tb_get(recorded));
         free(which);
         return;
      }
   }

   const size_t first = t->head - t->count;
   const int64_t now = model_now(w->model, NULL);

   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t jw;
   jw_init(&jw, tb);
   begin_reply(&jw, true);
   jw_key(&jw, "result");
   jw_begin_object(&jw);
   jw_key(&jw, "source");
   jw_string(&jw, "memory");
   jw_key(&jw, "from_fs");
   jw_int(&jw, from_fs);
   jw_key(&jw, "until_fs");
   jw_int(&jw, MIN(to_fs, now));

   int64_t total = 0, kept = 0;

   jw_key(&jw, "signals");
   jw_begin_array(&jw);

   for (size_t i = 0; i < npaths; i++) {
      const unsigned sig = which[i];
      design_obj_t *o = t->objs[sig];

      jw_begin_object(&jw);
      jw_key(&jw, "path");
      jw_string(&jw, istr(o->path));
      jw_key(&jw, "width");
      jw_int(&jw, signal_width(o->signal));

      // The value when the window opens is the last change before it
      const uint8_t *before = NULL;
      for (size_t n = 0; n < t->count; n++) {
         const uint8_t *rec = t->buf + ((first + n) % t->cap) * t->stride;
         int64_t time;
         uint32_t rsig;
         memcpy(&time, rec, sizeof(time));
         memcpy(&rsig, rec + 8, sizeof(rsig));
         if (rsig == sig && time <= from_fs)
            before = rec;
      }

      if (before != NULL) {
         uint32_t len;
         memcpy(&len, before + 12, sizeof(len));
         LOCAL_TEXT_BUF value = tb_new();
         format_raw(w, o, before + TRACE_HEADER, len, radix, value);
         jw_key(&jw, "value_at_from");
         jw_string(&jw, tb_get(value));
      }

      jw_key(&jw, "changes");
      jw_begin_array(&jw);
      for (size_t n = 0; n < t->count; n++) {
         const uint8_t *rec = t->buf + ((first + n) % t->cap) * t->stride;
         int64_t time;
         uint32_t rsig, len;
         memcpy(&time, rec, sizeof(time));
         memcpy(&rsig, rec + 8, sizeof(rsig));
         memcpy(&len, rec + 12, sizeof(len));

         if (rsig != sig || time < from_fs || time > to_fs)
            continue;

         total++;
         if (kept >= max_changes)
            continue;

         kept++;
         LOCAL_TEXT_BUF value = tb_new();
         format_raw(w, o, rec + TRACE_HEADER, len, radix, value);
         jw_begin_array(&jw);
         jw_int(&jw, time);
         jw_string(&jw, tb_get(value));
         jw_end_array(&jw);
      }
      jw_end_array(&jw);
      jw_end_object(&jw);
   }

   jw_end_array(&jw);
   jw_key(&jw, "changes_total");
   jw_int(&jw, total);
   jw_key(&jw, "truncated");
   jw_bool(&jw, total > kept);
   if (total > kept) {
      jw_key(&jw, "hint");
      jw_string(&jw, "More changes exist than were returned. Narrow the "
                "window with from and until, or raise max_changes.");
   }

   if (t->count > 0) {
      int64_t oldest;
      memcpy(&oldest, t->buf + (first % t->cap) * t->stride, sizeof(oldest));
      jw_key(&jw, "oldest_time_fs");
      jw_int(&jw, oldest);
   }
   jw_key(&jw, "dropped");
   jw_int(&jw, t->dropped);
   write_state(w, &jw);
   jw_end_object(&jw);
   jw_end_object(&jw);

   ctl_send(w, tb);
   free(which);
}

////////////////////////////////////////////////////////////////////////////////
// Entry point

int mcp_worker_main(rt_model_t *m, tree_t top, jit_t *jit, int ctl_fd,
                    const char *files_dir)
{
   worker_t w = {
      .model       = m,
      .top         = top,
      .ctl_fd      = ctl_fd,
      .jit         = jit,
      .files_dir   = files_dir,
      .log_max     = DEFAULT_LOG_MAX,
      .out_fd      = -1,
      .pause_level = DIAG_FATAL + 1,
      .printer     = printer_new(),
   };

   // A smaller log limit makes the limit testable
   const char *log_max = getenv("NVC_MCP_LOG_MAX");
   if (log_max != NULL && atoll(log_max) > 0)
      w.log_max = atoll(log_max);

   // Messages emitted while the design initialises are reported with the
   // ready event
   diag_set_consumer(worker_diag, &w);

   model_reset(m);

   rt_scope_t *root = find_scope(m, tree_stmt(top, 0));
   if (root == NULL)
      fatal_trace("cannot find the root scope of the design");

   w.by_path = hash_new(1024);

   LOCAL_TEXT_BUF path = tb_new();
   tb_cat(path, "/");
   index_scope(&w, root, -1, path);

   size_t nsignals = 0, nregions = 0;
   for (size_t i = 0; i < w.nobjs; i++) {
      if (w.objs[i].kind == OBJ_SIGNAL)
         nsignals++;
      else
         nregions++;
   }

   capture_output(&w);

   {
      LOCAL_TEXT_BUF tb = tb_new();
      json_writer_t jw;
      jw_init(&jw, tb);

      jw_begin_object(&jw);
      jw_key(&jw, "event");
      jw_string(&jw, "ready");
      jw_key(&jw, "signals");
      jw_int(&jw, nsignals);
      jw_key(&jw, "regions");
      jw_int(&jw, nregions);
      jw_key(&jw, "root");
      jw_string(&jw, "/");
      write_state(&w, &jw);
      write_messages(&w, &jw);
      write_output(&w, &jw);
      jw_end_object(&jw);

      ctl_send(&w, tb);
   }

   reset_messages(&w);

   for (;;) {
      // While a run is in progress only poll for requests so it advances
      char *line = ctl_read_line(&w, w.run.active ? 0 : -1);
      if (line == NULL) {
         if (w.eof)
            break;
         else if (w.run.active)
            run_step(&w);
         continue;
      }

      mem_pool_t *mp = pool_new();
      const char *perr;
      json_t *req = json_parse(mp, line, strlen(line), NULL, &perr);
      free(line);

      const char *op = json_string(json_get(req, "op"));
      const bool busy = w.run.active;

      if (op == NULL)
         reply_error(&w, "malformed request");
      else if (!strcmp(op, "close")) {
         op_close(&w);
         pool_free(mp);
         break;
      }
      else if (!strcmp(op, "cancel")) {
         // Meant for a call that waits for a foreground run: a background
         // run goes on, and the message may arrive after the run finished
         if (!w.run.background)
            op_stop(&w);
      }
      else if (!strcmp(op, "stop")) {
         op_stop(&w);
         op_status(&w, req);
      }
      else if (!strcmp(op, "status"))
         op_status(&w, req);
      else if (!strcmp(op, "run"))
         op_run(&w, req);
      else if (!strcmp(op, "list"))
         op_list(&w, req);
      else if (!strcmp(op, "search"))
         op_search(&w, req);
      else if (!strcmp(op, "get"))
         op_get(&w, req);
      else if (!strcmp(op, "wave_read"))
         op_wave_read(&w, req);
      else if (!strcmp(op, "wave_start"))
         op_wave_start(&w, req);
      else if (!strcmp(op, "wave_stop"))
         op_wave_stop(&w);
      else if (busy)
         reply_error(&w, "The simulation is running a background run, which "
                     "only allows reading: wait for it with nvc_sim_wait or "
                     "end it with nvc_sim_stop, then repeat this call");
      else if (!strcmp(op, "set"))
         op_set(&w, req);
      else
         reply_error(&w, "unknown operation '%s'", op);

      pool_free(mp);
   }

   // Leave the waveform files complete
   wave_finish(&w);
   live_free(&w);

   diag_set_consumer(NULL, NULL);
   printer_free(w.printer);

   return EXIT_SUCCESS;
}
