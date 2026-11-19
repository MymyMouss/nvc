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
#include "mcp/mcp-server.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// The schemas are written as JSON text and kept as the single source of
// truth: they are sent to the client by tools/list and also interpreted
// by the argument validator.  Avoid apostrophes in descriptions.
#define JSON(...) #__VA_ARGS__

#define WORKER_OUTPUT_LIMIT  8192
#define MAX_PARSED_DIAGS     10000
#define MAX_WORKER_ERRORS    200
#define MAX_SUGGESTIONS      5
#define MAX_SEARCH_DEPTH     6
#define MAX_SEARCH_ENTRIES   5000
#define MAX_ROOT_LISTING     20

////////////////////////////////////////////////////////////////////////////////
// nvc_analyse

static const char analyse_input[] = JSON({
   "type": "object",
   "properties": {
      "files": {
         "type": "array",
         "description": "Source files to compile, in dependency order. Paths are relative to the workspace root and may not leave it.",
         "minItems": 1,
         "maxItems": 64,
         "items": { "type": "string", "minLength": 1, "maxLength": 4096 }
      },
      "std": {
         "type": "string",
         "enum": ["1993", "2002", "2008", "2019"],
         "description": "VHDL standard revision. Defaults to the nvc default."
      },
      "library": {
         "type": "string",
         "minLength": 1,
         "maxLength": 64,
         "description": "Name of the library to compile into: a letter followed by letters, digits or underscores. Defaults to work."
      },
      "relaxed": {
         "type": "boolean",
         "description": "Downgrade some standard conformance errors to warnings."
      },
      "max_diagnostics": {
         "type": "integer",
         "minimum": 1,
         "maximum": 200,
         "description": "Maximum number of diagnostics to return. Defaults to 50."
      },
      "timeout_ms": {
         "type": "integer",
         "minimum": 1000,
         "maximum": 300000,
         "description": "Wall clock limit for the analysis. Defaults to 60000."
      }
   },
   "required": ["files"],
   "additionalProperties": false
});

static const char analyse_output[] = JSON({
   "type": "object",
   "properties": {
      "ok": { "type": "boolean" },
      "status": { "type": "string", "enum": ["ok", "errors", "failed"] },
      "exit_status": { "type": "integer" },
      "counts": {
         "type": "object",
         "properties": {
            "error": { "type": "integer" },
            "warning": { "type": "integer" },
            "note": { "type": "integer" }
         },
         "required": ["error", "warning", "note"]
      },
      "diagnostics": {
         "type": "array",
         "items": {
            "type": "object",
            "properties": {
               "severity": { "type": "string" },
               "file": { "type": "string" },
               "line": { "type": "integer" },
               "column": { "type": "integer" },
               "end_line": { "type": "integer" },
               "end_column": { "type": "integer" },
               "message": { "type": "string" },
               "hints": { "type": "array", "items": { "type": "string" } }
            },
            "required": ["severity", "message"]
         }
      },
      "diagnostics_truncated": { "type": "boolean" },
      "worker_output": { "type": "string" },
      "worker_output_truncated": { "type": "boolean" }
   },
   "required": ["ok", "status", "exit_status", "counts", "diagnostics",
                "diagnostics_truncated"]
});

static const char analyse_example[] = JSON({
   "files": ["src/counter.vhd", "tb/counter_tb.vhd"],
   "std": "2008"
});

static const char analyse_description[] =
   "Compile VHDL or SystemVerilog source files into a library in the "
   "workspace, as the nvc -a command does.  Compile files in dependency "
   "order.  A compilation that finds errors is a normal result: check ok "
   "and read diagnostics, each with file, line and column.  The library "
   "is written to the workspace, so later tools can elaborate and "
   "simulate the design.  status is failed when nvc stopped without "
   "producing diagnostics: read worker_output.";

static bool valid_library_name(const char *name)
{
   if (!isalpha((unsigned char)name[0]))
      return false;

   for (const char *p = name; *p; p++) {
      if (!isalnum((unsigned char)*p) && *p != '_')
         return false;
   }

   return true;
}

typedef struct {
   const char *base;
   size_t      rootlen;
   size_t      visited;
   char       *found[MAX_SUGGESTIONS];
   unsigned    nfound;
} search_t;

static int compare_names(const void *a, const void *b)
{
   return strcmp(*(char *const *)a, *(char *const *)b);
}

// Look for regular files called base below dir.  Symbolic links are not
// followed and the walk is bounded in depth and size.
static void search_dir(search_t *s, const char *dir, unsigned depth)
{
   if (depth > MAX_SEARCH_DEPTH)
      return;

   DIR *d = opendir(dir);
   if (d == NULL)
      return;

   struct dirent *e;
   while ((e = readdir(d)) != NULL && s->nfound < MAX_SUGGESTIONS
          && s->visited < MAX_SEARCH_ENTRIES) {
      if (e->d_name[0] == '.')
         continue;

      s->visited++;

      char *full = xasprintf("%s/%s", dir, e->d_name);

      struct stat st;
      if (lstat(full, &st) == 0) {
         if (S_ISREG(st.st_mode) && strcasecmp(e->d_name, s->base) == 0)
            s->found[s->nfound++] = xstrdup(full + s->rootlen + 1);
         else if (S_ISDIR(st.st_mode))
            search_dir(s, full, depth + 1);
      }

      free(full);
   }

   closedir(d);
}

// Describe where a missing file might be so the caller can recover
// without a way to list directories.
static void describe_missing(const char *root, const char *path,
                             text_buf_t *out)
{
   const char *slash = strrchr(path, '/');

   search_t s = {
      .base    = slash ? slash + 1 : path,
      .rootlen = strlen(root),
   };

   search_dir(&s, root, 0);

   if (s.nfound > 0) {
      qsort(s.found, s.nfound, sizeof(char *), compare_names);

      tb_cat(out, ". Did you mean: ");
      for (unsigned i = 0; i < s.nfound; i++) {
         if (i > 0)
            tb_cat(out, ", ");
         tb_cat(out, s.found[i]);
         free(s.found[i]);
      }
      tb_append(out, '?');
      return;
   }

   // No file of that name: show what the root holds
   DIR *d = opendir(root);
   if (d == NULL)
      return;

   char *names[MAX_ROOT_LISTING + 1];
   unsigned n = 0;
   bool more = false;

   struct dirent *e;
   while ((e = readdir(d)) != NULL) {
      if (e->d_name[0] == '.')
         continue;
      else if (n == MAX_ROOT_LISTING) {
         more = true;
         break;
      }

      char *full = xasprintf("%s/%s", root, e->d_name);
      struct stat st;
      const bool is_dir = lstat(full, &st) == 0 && S_ISDIR(st.st_mode);
      free(full);

      names[n++] = xasprintf("%s%s", e->d_name, is_dir ? "/" : "");
   }

   closedir(d);

   qsort(names, n, sizeof(char *), compare_names);

   tb_printf(out, ". No file named '%s' exists below the root", s.base);
   if (n > 0)
      tb_cat(out, ". The root contains: ");
   for (unsigned i = 0; i < n; i++) {
      if (i > 0)
         tb_cat(out, ", ");
      tb_cat(out, names[i]);
      free(names[i]);
   }
   if (more)
      tb_cat(out, ", ...");
}

// Resolve a client supplied path against the workspace root.  The result
// is relative to the root and guaranteed not to escape it, even through
// symbolic links.
static char *resolve_in_root(mcp_call_t *call, const char *path)
{
   const char *root = mcp_call_root(call);
   const size_t rootlen = strlen(root);

   LOCAL_TEXT_BUF full = tb_new();
   if (path[0] == '/')
      tb_cat(full, path);
   else
      tb_printf(full, "%s/%s", root, path);

   char *real = realpath(tb_get(full), NULL);
   if (real == NULL) {
      LOCAL_TEXT_BUF hint = tb_new();
      if (errno == ENOENT)
         describe_missing(root, path, hint);

      mcp_call_error(call, "File '%s' was not found. Paths are relative to "
                     "the workspace root %s%s", path, root, tb_get(hint));
      return NULL;
   }

   const bool inside = strncmp(real, root, rootlen) == 0
      && (real[rootlen] == '/' || real[rootlen] == '\0');

   if (!inside || real[rootlen] == '\0') {
      mcp_call_error(call, "File '%s' is outside the workspace root %s. Use "
                     "a path inside the root", path, root);
      free(real);
      return NULL;
   }

   struct stat st;
   if (stat(real, &st) < 0 || !S_ISREG(st.st_mode)) {
      mcp_call_error(call, "'%s' is not a regular file", path);
      free(real);
      return NULL;
   }

   // A name starting with a dash would be read as an option
   const char *rel = real + rootlen + 1;
   char *result;
   if (rel[0] == '-')
      result = xasprintf("./%s", rel);
   else
      result = xstrdup(rel);

   free(real);
   return result;
}

static const char *signal_name(int sig)
{
   switch (sig) {
   case SIGSEGV: return "SIGSEGV";
   case SIGABRT: return "SIGABRT";
   case SIGBUS:  return "SIGBUS";
   case SIGFPE:  return "SIGFPE";
   case SIGILL:  return "SIGILL";
   case SIGKILL: return "SIGKILL";
   case SIGXCPU: return "SIGXCPU";
   case SIGTERM: return "SIGTERM";
   default:      return "a signal";
   }
}

static void write_worker_output(json_writer_t *w, const mcp_child_result_t *res)
{
   if (res->output_len == 0)
      return;

   const size_t len = MIN(res->output_len, WORKER_OUTPUT_LIMIT);

   jw_key(w, "worker_output");
   jw_stringn(w, res->output, len);

   if (len < res->output_len || res->output_truncated) {
      jw_key(w, "worker_output_truncated");
      jw_bool(w, true);
   }
}

static void write_diagnostic(json_writer_t *w, const json_t *d)
{
   jw_begin_object(w);

   static const char *const strings[] = { "severity", "file" };
   for (size_t i = 0; i < ARRAY_LEN(strings); i++) {
      const char *str = json_string(json_get(d, strings[i]));
      if (str != NULL) {
         jw_key(w, strings[i]);
         jw_string(w, str);
      }
   }

   static const char *const ints[] = { "line", "column", "end_line",
                                       "end_column" };
   for (size_t i = 0; i < ARRAY_LEN(ints); i++) {
      int64_t value;
      if (json_as_int(json_get(d, ints[i]), &value)) {
         jw_key(w, ints[i]);
         jw_int(w, value);
      }
   }

   const char *message = json_string(json_get(d, "message"));
   jw_key(w, "message");
   jw_string(w, message ?: "");

   const json_t *hints = json_get(d, "hints");
   if (json_is(hints, JSON_ARRAY) && hints->array.count > 0) {
      jw_key(w, "hints");
      jw_begin_array(w);
      for (size_t i = 0; i < hints->array.count; i++) {
         const char *hint = json_string(hints->array.items[i]);
         if (hint != NULL)
            jw_string(w, hint);
      }
      jw_end_array(w);
   }

   jw_end_object(w);
}

static bool tool_analyse(mcp_call_t *call, const json_t *args,
                         json_writer_t *out)
{
   const json_t *files = json_get(args, "files");
   const char *std = json_string(json_get(args, "std"));
   const char *library = json_string(json_get(args, "library"));

   bool relaxed = false;
   json_as_bool(json_get(args, "relaxed"), &relaxed);

   int64_t max_diag = 50, timeout_ms = 60000;
   json_as_int(json_get(args, "max_diagnostics"), &max_diag);
   json_as_int(json_get(args, "timeout_ms"), &timeout_ms);

   if (library != NULL && !valid_library_name(library)) {
      mcp_call_error(call, "Invalid library name '%s': use a letter followed "
                     "by letters, digits or underscores, for example "
                     "\"work\"", library);
      return false;
   }

   const size_t nfiles = files->array.count;
   char **rel = xcalloc_array(nfiles, sizeof(char *));
   bool ok = true;

   for (size_t i = 0; ok && i < nfiles; i++) {
      rel[i] = resolve_in_root(call, files->array.items[i]->string.chars);
      ok = rel[i] != NULL;
   }

   if (!ok) {
      for (size_t i = 0; i < nfiles; i++)
         free(rel[i]);
      free(rel);
      return false;
   }

   // exe, forwarded global options, --work, --std, -a, options, files
   int ngargs;
   const char *const *gargs = mcp_call_global_argv(call, &ngargs);

   const char **argv =
      xcalloc_array(ngargs + nfiles + 10, sizeof(const char *));
   size_t argc = 0;

   char *work_opt = library != NULL ? xasprintf("--work=%s", library) : NULL;
   char *std_opt = std != NULL ? xasprintf("--std=%s", std) : NULL;
   // Count every diagnostic up to a fixed limit so the totals stay
   // accurate when only some of them are returned
   char *limit_opt = xasprintf("--error-limit=%d", MAX_WORKER_ERRORS);

   argv[argc++] = mcp_call_exe(call);
   for (int i = 0; i < ngargs; i++)
      argv[argc++] = gargs[i];
   if (work_opt != NULL)
      argv[argc++] = work_opt;
   if (std_opt != NULL)
      argv[argc++] = std_opt;
   argv[argc++] = "-a";
   argv[argc++] = limit_opt;
   if (relaxed)
      argv[argc++] = "--relaxed";
   for (size_t i = 0; i < nfiles; i++)
      argv[argc++] = rel[i];
   argv[argc] = NULL;

   const mcp_child_spec_t spec = {
      .cwd        = mcp_call_root(call),
      .argv       = argv,
      .wall_ms    = (unsigned)timeout_ms,
      .max_output = 64 * 1024,
      .max_diag   = 4 * 1024 * 1024,
   };

   mcp_child_result_t res;
   mcp_child_run(call, &spec, &res);

   free(argv);
   free(work_opt);
   free(std_opt);
   free(limit_opt);
   for (size_t i = 0; i < nfiles; i++)
      free(rel[i]);
   free(rel);

   bool result = false;

   if (res.cancelled)
      mcp_call_error(call, "The request was cancelled");
   else if (res.spawn_failed)
      mcp_call_error(call, "The nvc worker could not be started (executable "
                     "%s). Check that the nvc binary is installed",
                     mcp_call_exe(call));
   else if (res.timed_out)
      mcp_call_error(call, "The analysis did not finish within %"PRIi64" ms "
                     "and was stopped. Analyse fewer files per call or raise "
                     "timeout_ms (maximum 300000)", timeout_ms);
   else if (res.term_signal != 0) {
      const size_t n = MIN(res.output_len, 2000);
      mcp_call_error(call, "The nvc worker was terminated by %s. This is a "
                     "bug in nvc. Worker output: %.*s",
                     signal_name(res.term_signal), (int)n,
                     res.output ?: "");
   }
   else {
      mem_pool_t *mp = pool_new();

      int64_t nerror = 0, nwarn = 0, nnote = 0, ndiags = 0;
      LOCAL_TEXT_BUF list = tb_new();
      json_writer_t lw;
      jw_init(&lw, list);
      jw_begin_array(&lw);

      bool truncated = res.diag_truncated;

      const char *p = res.diag ?: "", *end = p + res.diag_len;
      while (p < end && ndiags < MAX_PARSED_DIAGS) {
         const char *nl = memchr(p, '\n', end - p);
         const size_t len = (nl ? nl : end) - p;

         const char *perr;
         json_t *d = len > 0 ? json_parse(mp, p, len, NULL, &perr) : NULL;

         if (json_is(d, JSON_OBJECT)) {
            const char *sev = json_string(json_get(d, "severity")) ?: "";

            if (!strcmp(sev, "error") || !strcmp(sev, "failure")
                || !strcmp(sev, "fatal"))
               nerror++;
            else if (!strcmp(sev, "warning"))
               nwarn++;
            else if (!strcmp(sev, "note"))
               nnote++;

            if (ndiags < max_diag)
               write_diagnostic(&lw, d);
            else
               truncated = true;

            ndiags++;
         }

         p = nl ? nl + 1 : end;
      }

      if (p < end)
         truncated = true;

      jw_end_array(&lw);

      const bool succeeded = res.exited && res.exit_status == 0;
      const char *status = succeeded ? "ok" : nerror > 0 ? "errors" : "failed";

      jw_begin_object(out);
      jw_key(out, "ok");
      jw_bool(out, succeeded);
      jw_key(out, "status");
      jw_string(out, status);
      jw_key(out, "exit_status");
      jw_int(out, res.exit_status);
      jw_key(out, "counts");
      jw_begin_object(out);
      jw_key(out, "error");
      jw_int(out, nerror);
      jw_key(out, "warning");
      jw_int(out, nwarn);
      jw_key(out, "note");
      jw_int(out, nnote);
      jw_end_object(out);
      jw_key(out, "diagnostics");
      jw_raw(out, tb_get(list));
      jw_key(out, "diagnostics_truncated");
      jw_bool(out, truncated);
      write_worker_output(out, &res);
      jw_end_object(out);

      pool_free(mp);
      result = true;
   }

   mcp_child_free(&res);
   return result;
}

////////////////////////////////////////////////////////////////////////////////
// Tool table

static const mcp_tool_t tools[] = {
   {
      .name          = "nvc_analyse",
      .title         = "Analyse source files",
      .description   = analyse_description,
      .input_schema  = analyse_input,
      .output_schema = analyse_output,
      .example       = analyse_example,
      .read_only     = false,
      .destructive   = false,
      .idempotent    = true,
      .fn            = tool_analyse,
   },
};

const mcp_tool_t *mcp_tool_table(size_t *count)
{
   *count = ARRAY_LEN(tools);
   return tools;
}
