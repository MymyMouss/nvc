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

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
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
      "worker_output_truncated": { "type": "boolean" },
      "hint": { "type": "string" }
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

// Turn the outcome of a worker that analysed or elaborated into the
// tool result: diagnostics are a normal result, a crashed or stuck worker
// is a tool error.
static bool compile_result(mcp_call_t *call, const mcp_child_result_t *res_in,
                           const char *what, int64_t max_diag,
                           int64_t timeout_ms, json_writer_t *out)
{
   const mcp_child_result_t res = *res_in;

   bool result = false;

   if (res.cancelled)
      mcp_call_error(call, "The request was cancelled");
   else if (res.spawn_failed)
      mcp_call_error(call, "The nvc worker could not be started (executable "
                     "%s). Check that the nvc binary is installed",
                     mcp_call_exe(call));
   else if (res.timed_out)
      mcp_call_error(call, "The %s did not finish within %"PRIi64" ms "
                     "and was stopped. Raise timeout_ms (maximum 300000) or "
                     "reduce the work per call", what, timeout_ms);
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

      if (!succeeded && nerror == 0 && res.output != NULL) {
         const char *hint = NULL;
         if (strstr(res.output, "cannot find unit") != NULL)
            hint = "The unit was not found in the library. Analyse the file "
               "that defines it with nvc_analyse first, and check the name "
               "and library with nvc_library_list";
         else if (strstr(res.output, "library") != NULL
                  && strstr(res.output, "not found") != NULL)
            hint = "A library could not be found. Analyse the files into "
               "it first with nvc_analyse (library argument), and pass the "
               "same library and std to every tool";

         if (hint != NULL) {
            jw_key(out, "hint");
            jw_string(out, hint);
         }
      }

      jw_end_object(out);

      pool_free(mp);
      result = true;
   }

   return result;
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

   const bool result = compile_result(call, &res, "analysis", max_diag,
                                      timeout_ms, out);
   mcp_child_free(&res);
   return result;
}


////////////////////////////////////////////////////////////////////////////////
// nvc_elaborate

static const char elaborate_input[] = MCP_JSON({
   "type": "object",
   "properties": {
      "top": {
         "type": "string",
         "minLength": 1,
         "maxLength": 128,
         "description": "Name of the top-level entity or configuration to elaborate, for example counter_tb. It must already have been analysed with nvc_analyse."
      },
      "library": {
         "type": "string",
         "minLength": 1,
         "maxLength": 64,
         "description": "Library the top-level unit was analysed into. Defaults to work."
      },
      "std": {
         "type": "string",
         "enum": ["1993", "2002", "2008", "2019"],
         "description": "VHDL standard revision. Use the same value as when the files were analysed."
      },
      "generics": {
         "type": "object",
         "description": "Values for the generics of the top-level unit, by name. Each value is written as a VHDL literal: 8, true, '1' or a quoted string. At most 32."
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
         "description": "Wall clock limit for the elaboration. Defaults to 120000."
      }
   },
   "required": ["top"],
   "additionalProperties": false
});

static const char elaborate_example[] = MCP_JSON({
   "top": "counter_tb", "std": "2008", "generics": { "WIDTH": 8 }
});

static const char elaborate_description[] =
   "Elaborate a top-level design unit that was analysed earlier: resolve "
   "its hierarchy and generics and prepare it for simulation, as nvc -e "
   "does.  Run nvc_analyse on every source file first.  A design with "
   "errors is a normal result: check ok and read diagnostics.  When ok is "
   "true, pass the same top (and std and library) to nvc_sim_open.";

#define MAX_GENERICS 32

static bool valid_identifier(const char *name)
{
   if (!isalpha((unsigned char)name[0]))
      return false;

   for (const char *p = name; *p; p++) {
      if (!isalnum((unsigned char)*p) && *p != '_')
         return false;
   }

   return true;
}

static bool valid_unit_name(const char *name)
{
   if (!isalpha((unsigned char)name[0]))
      return false;

   for (const char *p = name; *p; p++) {
      if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-'
          && *p != '(' && *p != ')')
         return false;
   }

   return true;
}

static bool tool_elaborate(mcp_call_t *call, const json_t *args,
                           json_writer_t *out)
{
   const char *top = json_string(json_get(args, "top"));
   const char *std = json_string(json_get(args, "std"));
   const char *library = json_string(json_get(args, "library"));
   const json_t *generics = json_get(args, "generics");

   int64_t max_diag = 50, timeout_ms = 120000;
   json_as_int(json_get(args, "max_diagnostics"), &max_diag);
   json_as_int(json_get(args, "timeout_ms"), &timeout_ms);

   if (!valid_unit_name(top)) {
      mcp_call_error(call, "Invalid design unit name '%s': use a letter "
                     "followed by letters, digits, underscores or hyphens, for "
                     "example \"counter_tb\"", top);
      return false;
   }
   else if (library != NULL && !valid_library_name(library)) {
      mcp_call_error(call, "Invalid library name '%s': use a letter followed "
                     "by letters, digits or underscores", library);
      return false;
   }

   const size_t ngen = generics ? generics->object.count : 0;
   if (generics != NULL && !json_is(generics, JSON_OBJECT)) {
      mcp_call_error(call, "generics must be an object such as "
                     "{\"WIDTH\": 8}");
      return false;
   }
   else if (ngen > MAX_GENERICS) {
      mcp_call_error(call, "At most %d generics can be set (got %zu)",
                     MAX_GENERICS, ngen);
      return false;
   }

   // Each generic becomes one -gNAME=VALUE argument: names are plain
   // identifiers and values cannot hold control characters
   char **gen_opts = xcalloc_array(ngen + 1, sizeof(char *));
   bool ok = true;

   for (size_t i = 0; ok && i < ngen; i++) {
      const char *name = generics->object.keys[i];
      const json_t *v = generics->object.values[i];

      LOCAL_TEXT_BUF value = tb_new();
      switch (v->kind) {
      case JSON_STRING: tb_cat(value, v->string.chars); break;
      case JSON_INT:    tb_printf(value, "%"PRIi64, v->integer); break;
      case JSON_REAL:   tb_printf(value, "%.17g", v->real); break;
      case JSON_BOOL:   tb_cat(value, v->boolean ? "true" : "false"); break;
      default:
         mcp_call_error(call, "The value of generic %s must be a string, "
                        "number or boolean", name);
         ok = false;
         continue;
      }

      bool clean = tb_len(value) > 0 && tb_len(value) <= 256;
      for (const char *c = tb_get(value); *c; c++)
         clean &= (unsigned char)*c >= 0x20;

      if (!valid_identifier(name)) {
         mcp_call_error(call, "Invalid generic name '%s': use the name of a "
                        "generic of %s", name, top);
         ok = false;
      }
      else if (!clean) {
         mcp_call_error(call, "Invalid value for generic %s: give a VHDL "
                        "literal of 1 to 256 printable characters", name);
         ok = false;
      }
      else
         gen_opts[i] = xasprintf("-g%s=%s", name, tb_get(value));
   }

   int ngargs;
   const char *const *gargs = mcp_call_global_argv(call, &ngargs);

   const char **argv = xcalloc_array(ngargs + ngen + 8, sizeof(char *));
   size_t argc = 0;

   char *work_opt = library ? xasprintf("--work=%s", library) : NULL;
   char *std_opt = std ? xasprintf("--std=%s", std) : NULL;

   if (ok) {
      argv[argc++] = mcp_call_exe(call);
      for (int i = 0; i < ngargs; i++)
         argv[argc++] = gargs[i];
      if (work_opt != NULL)
         argv[argc++] = work_opt;
      if (std_opt != NULL)
         argv[argc++] = std_opt;
      argv[argc++] = "-e";
      for (size_t i = 0; i < ngen; i++)
         argv[argc++] = gen_opts[i];
      argv[argc++] = top;
      argv[argc] = NULL;
   }

   bool result = false;
   if (ok) {
      const mcp_child_spec_t spec = {
         .cwd        = mcp_call_root(call),
         .argv       = argv,
         .wall_ms    = (unsigned)timeout_ms,
         .max_output = 64 * 1024,
         .max_diag   = 4 * 1024 * 1024,
      };

      mcp_child_result_t res;
      mcp_child_run(call, &spec, &res);

      result = compile_result(call, &res, "elaboration", max_diag, timeout_ms,
                              out);
      mcp_child_free(&res);
   }

   for (size_t i = 0; i < ngen; i++)
      free(gen_opts[i]);
   free(gen_opts);
   free(argv);
   free(work_opt);
   free(std_opt);
   return result;
}

////////////////////////////////////////////////////////////////////////////////
// nvc_workspace_list

#define MAX_WALK_ENTRIES   20000
#define MAX_LIST_PAGE      500

typedef struct {
   char       *path;       // Relative to the workspace root
   const char *type;       // file or library
   const char *language;   // For files
   off_t       size;
} ws_entry_t;

typedef struct {
   ws_entry_t *items;
   size_t      count;
   size_t      alloc;
   size_t      visited;
   size_t      rootlen;
   const char *pattern;
   bool        hdl_only;
   unsigned    max_depth;
   bool        scan_truncated;
} ws_walk_t;

static const char *language_of(const char *name)
{
   const char *dot = strrchr(name, '.');
   if (dot == NULL)
      return "other";

   if (!strcasecmp(dot, ".vhd") || !strcasecmp(dot, ".vhdl"))
      return "vhdl";
   else if (!strcasecmp(dot, ".v") || !strcasecmp(dot, ".vh"))
      return "verilog";
   else if (!strcasecmp(dot, ".sv") || !strcasecmp(dot, ".svh"))
      return "systemverilog";
   else
      return "other";
}

static int compare_entries(const void *a, const void *b)
{
   return strcmp(((const ws_entry_t *)a)->path, ((const ws_entry_t *)b)->path);
}

static void walk_add(ws_walk_t *w, const char *full, const char *type,
                     const char *language, off_t size)
{
   if (w->count == w->alloc) {
      w->alloc = MAX(w->alloc * 2, 64);
      w->items = xrealloc_array(w->items, w->alloc, sizeof(ws_entry_t));
   }

   w->items[w->count++] = (ws_entry_t){
      .path = xstrdup(full + w->rootlen + 1),
      .type = type,
      .language = language,
      .size = size,
   };
}

static void walk_dir(ws_walk_t *w, const char *dir, unsigned depth)
{
   DIR *d = opendir(dir);
   if (d == NULL)
      return;

   struct dirent *e;
   while ((e = readdir(d)) != NULL) {
      if (e->d_name[0] == '.')
         continue;
      else if (w->visited >= MAX_WALK_ENTRIES) {
         w->scan_truncated = true;
         break;
      }

      w->visited++;

      char *full = xasprintf("%s/%s", dir, e->d_name);

      struct stat st;
      if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
         char *marker = xasprintf("%s/_NVC_LIB", full);
         const bool is_lib = access(marker, F_OK) == 0;
         free(marker);

         if (is_lib) {
            if (w->pattern == NULL
                || fnmatch(w->pattern, e->d_name, FNM_CASEFOLD) == 0)
               walk_add(w, full, "library", NULL, 0);
         }
         else if (depth < w->max_depth)
            walk_dir(w, full, depth + 1);
      }
      else if (lstat(full, &st) == 0 && S_ISREG(st.st_mode)) {
         const char *lang = language_of(e->d_name);
         const bool wanted = !w->hdl_only || strcmp(lang, "other") != 0;

         if (wanted && (w->pattern == NULL
                        || fnmatch(w->pattern, e->d_name, FNM_CASEFOLD) == 0))
            walk_add(w, full, "file", lang, st.st_size);
      }

      free(full);
   }

   closedir(d);
}

static const char workspace_input[] = MCP_JSON({
   "type": "object",
   "properties": {
      "path": {
         "type": "string",
         "maxLength": 4096,
         "description": "Directory to list, relative to the workspace root. Defaults to the root itself."
      },
      "pattern": {
         "type": "string",
         "maxLength": 256,
         "description": "Glob on the file name, case insensitive, for example *_tb.vhd. Defaults to all."
      },
      "kind": {
         "type": "string",
         "enum": ["hdl", "all"],
         "description": "hdl lists VHDL and Verilog sources only; all lists every file. Defaults to hdl."
      },
      "max_depth": {
         "type": "integer",
         "minimum": 0,
         "maximum": 8,
         "description": "How many directory levels to descend below path. Defaults to 4."
      },
      "limit": { "type": "integer", "minimum": 1, "maximum": 500, "description": "Maximum entries to return. Defaults to 100." },
      "cursor": { "type": "string", "maxLength": 20, "description": "next_cursor from the previous page." }
   },
   "additionalProperties": false
});

static const char workspace_output[] = MCP_JSON({
   "type": "object",
   "properties": {
      "root": { "type": "string" },
      "path": { "type": "string" },
      "entries": {
         "type": "array",
         "items": {
            "type": "object",
            "properties": {
               "path": { "type": "string" },
               "type": { "type": "string", "enum": ["file", "library"] },
               "language": { "type": "string" },
               "size_bytes": { "type": "integer" }
            },
            "required": ["path", "type"]
         }
      },
      "total": { "type": "integer" },
      "truncated": { "type": "boolean" },
      "next_cursor": { "type": "string" },
      "scan_truncated": { "type": "boolean" }
   },
   "required": ["root", "path", "entries", "total", "truncated"]
});

static const char workspace_example[] = MCP_JSON({
   "path": "src", "pattern": "*.vhd"
});

static const char workspace_description[] =
   "List the source files in the workspace, sorted by path, so you can "
   "find what to compile with nvc_analyse.  Entries of type library are "
   "directories that hold analysed designs (such as work) and are not "
   "descended into: list them with nvc_library_list.  Hidden files and "
   "symbolic links are skipped.  Results are paged: pass next_cursor to "
   "get the next page.  Paths in the result are relative to root and can "
   "be passed to nvc_analyse as they are.";

static bool tool_workspace_list(mcp_call_t *call, const json_t *args,
                                json_writer_t *out)
{
   const char *root = mcp_call_root(call);
   const size_t rootlen = strlen(root);

   const char *path = json_string(json_get(args, "path")) ?: ".";
   const char *pattern = json_string(json_get(args, "pattern"));
   const char *kind = json_string(json_get(args, "kind")) ?: "hdl";
   const char *cursor = json_string(json_get(args, "cursor"));

   int64_t max_depth = 4, limit = 100;
   json_as_int(json_get(args, "max_depth"), &max_depth);
   json_as_int(json_get(args, "limit"), &limit);

   int64_t offset = cursor ? strtoll(cursor, NULL, 10) : 0;
   if (offset < 0)
      offset = 0;

   LOCAL_TEXT_BUF full = tb_new();
   if (path[0] == '/')
      tb_cat(full, path);
   else
      tb_printf(full, "%s/%s", root, path);

   char *dir = realpath(tb_get(full), NULL);
   struct stat st;
   if (dir == NULL || stat(dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
      mcp_call_error(call, "'%s' is not a directory under the workspace "
                     "root %s. Call nvc_workspace_list without a path to see "
                     "the layout", path, root);
      free(dir);
      return false;
   }

   const bool inside = strncmp(dir, root, rootlen) == 0
      && (dir[rootlen] == '/' || dir[rootlen] == '\0');
   if (!inside) {
      mcp_call_error(call, "'%s' is outside the workspace root %s. Use a "
                     "path inside the root", path, root);
      free(dir);
      return false;
   }

   ws_walk_t w = {
      .rootlen   = rootlen,
      .pattern   = pattern,
      .hdl_only  = strcmp(kind, "all") != 0,
      .max_depth = (unsigned)max_depth,
   };

   walk_dir(&w, dir, 0);

   qsort(w.items, w.count, sizeof(ws_entry_t), compare_entries);

   jw_begin_object(out);
   jw_key(out, "root");
   jw_string(out, root);
   jw_key(out, "path");
   jw_string(out, dir[rootlen] == '\0' ? "." : dir + rootlen + 1);
   jw_key(out, "entries");
   jw_begin_array(out);

   size_t shown = 0;
   for (size_t i = offset; i < w.count && (int64_t)shown < limit; i++, shown++) {
      const ws_entry_t *e = &(w.items[i]);
      jw_begin_object(out);
      jw_key(out, "path");
      jw_string(out, e->path);
      jw_key(out, "type");
      jw_string(out, e->type);
      if (e->language != NULL) {
         jw_key(out, "language");
         jw_string(out, e->language);
         jw_key(out, "size_bytes");
         jw_int(out, e->size);
      }
      jw_end_object(out);
   }

   jw_end_array(out);
   jw_key(out, "total");
   jw_int(out, w.count);

   const bool more = (size_t)offset + shown < w.count;
   jw_key(out, "truncated");
   jw_bool(out, more);
   if (more) {
      char *next = xasprintf("%zu", (size_t)offset + shown);
      jw_key(out, "next_cursor");
      jw_string(out, next);
      free(next);
   }

   if (w.scan_truncated) {
      jw_key(out, "scan_truncated");
      jw_bool(out, true);
   }

   jw_end_object(out);

   for (size_t i = 0; i < w.count; i++)
      free(w.items[i].path);
   free(w.items);
   free(dir);
   return true;
}

////////////////////////////////////////////////////////////////////////////////
// nvc_library_list

#define MAX_LIBRARY_UNITS 500

static const char library_input[] = MCP_JSON({
   "type": "object",
   "properties": {
      "library": {
         "type": "string",
         "minLength": 1,
         "maxLength": 64,
         "description": "Library to list. Defaults to work."
      },
      "timeout_ms": {
         "type": "integer",
         "minimum": 1000,
         "maximum": 300000,
         "description": "Wall clock limit. Defaults to 30000."
      }
   },
   "additionalProperties": false
});

static const char library_output[] = MCP_JSON({
   "type": "object",
   "properties": {
      "library": { "type": "string" },
      "units": {
         "type": "array",
         "items": {
            "type": "object",
            "properties": {
               "name": { "type": "string" },
               "kind": { "type": "string" },
               "entity": { "type": "string" },
               "architecture": { "type": "string" }
            },
            "required": ["name", "kind"]
         }
      },
      "elaborated": { "type": "array", "items": { "type": "string" } },
      "total": { "type": "integer" },
      "truncated": { "type": "boolean" }
   },
   "required": ["library", "units", "elaborated", "total", "truncated"]
});

static const char library_example[] = MCP_JSON({ "library": "work" });

static const char library_description[] =
   "List the design units that have been analysed into a library: entities, "
   "architectures, packages and so on.  elaborated names the top-level "
   "designs that have been elaborated and are ready for nvc_sim_open.  Use "
   "it to check what nvc_analyse and nvc_elaborate produced.  A library "
   "exists only after files were analysed into it.";

static const char *unit_kind(const char *pretty, size_t len)
{
   static const struct { const char *pretty, *kind; } kinds[] = {
      { "Entity", "entity" }, { "Architecture", "architecture" },
      { "Package", "package" }, { "Package body", "package_body" },
      { "Instantiated package", "package_instance" },
      { "Configuration", "configuration" }, { "Context", "context" },
      { "Verilog", "verilog_module" }, { "Elaborated", "elaborated" },
   };

   for (size_t i = 0; i < ARRAY_LEN(kinds); i++) {
      if (strlen(kinds[i].pretty) == len
          && strncmp(kinds[i].pretty, pretty, len) == 0)
         return kinds[i].kind;
   }

   return "unknown";
}

static bool tool_library_list(mcp_call_t *call, const json_t *args,
                              json_writer_t *out)
{
   const char *library = json_string(json_get(args, "library")) ?: "work";

   int64_t timeout_ms = 30000;
   json_as_int(json_get(args, "timeout_ms"), &timeout_ms);

   if (!valid_library_name(library)) {
      mcp_call_error(call, "Invalid library name '%s': use a letter followed "
                     "by letters, digits or underscores", library);
      return false;
   }

   // nvc creates a library directory when asked about one that does not
   // exist, so look first and report what is there instead
   const char *root = mcp_call_root(call);
   char *marker = xasprintf("%s/%s/_NVC_LIB", root, library);
   const bool exists = access(marker, F_OK) == 0;
   free(marker);

   if (!exists) {
      LOCAL_TEXT_BUF known = tb_new();
      DIR *d = opendir(root);
      unsigned n = 0;
      struct dirent *e;
      while (d != NULL && (e = readdir(d)) != NULL) {
         char *m = xasprintf("%s/%s/_NVC_LIB", root, e->d_name);
         if (e->d_name[0] != '.' && access(m, F_OK) == 0)
            tb_printf(known, "%s%s", n++ ? ", " : "", e->d_name);
         free(m);
      }
      if (d != NULL)
         closedir(d);

      mcp_call_error(call, "There is no library '%s' in the workspace. "
                     "Libraries are created by nvc_analyse. Libraries "
                     "present: %s", library,
                     n > 0 ? tb_get(known) : "none");
      return false;
   }

   int ngargs;
   const char *const *gargs = mcp_call_global_argv(call, &ngargs);

   const char **argv = xcalloc_array(ngargs + 5, sizeof(char *));
   size_t argc = 0;
   char *work_opt = xasprintf("--work=%s", library);

   argv[argc++] = mcp_call_exe(call);
   for (int i = 0; i < ngargs; i++)
      argv[argc++] = gargs[i];
   argv[argc++] = work_opt;
   argv[argc++] = "--list";
   argv[argc] = NULL;

   const mcp_child_spec_t spec = {
      .cwd        = root,
      .argv       = argv,
      .wall_ms    = (unsigned)timeout_ms,
      .max_output = 4 * 1024 * 1024,
      .max_diag   = 64 * 1024,
   };

   mcp_child_result_t res;
   mcp_child_run(call, &spec, &res);

   free(argv);
   free(work_opt);

   bool result = false;

   if (res.cancelled)
      mcp_call_error(call, "The request was cancelled");
   else if (res.spawn_failed || res.timed_out || res.term_signal != 0
            || !res.exited || res.exit_status != 0)
      mcp_call_error(call, "Listing the library failed (%s). Output: %.*s",
                     res.timed_out ? "timed out" : "the nvc process ended "
                     "abnormally", (int)MIN(res.output_len, 1000),
                     res.output ?: "");
   else {
      LOCAL_TEXT_BUF units = tb_new();
      LOCAL_TEXT_BUF tops = tb_new();
      json_writer_t uw, tw;
      jw_init(&uw, units);
      jw_init(&tw, tops);
      jw_begin_array(&uw);
      jw_begin_array(&tw);

      size_t nunits = 0, shown = 0;

      // Each line reads "NAME      : Kind"
      const char *p = res.output ?: "", *end = p + res.output_len;
      while (p < end) {
         const char *nl = memchr(p, '\n', end - p);
         const char *line_end = nl ? nl : end;
         const char *sep = NULL;
         for (const char *q = p; q + 2 < line_end; q++) {
            if (q[0] == ' ' && q[1] == ':' && q[2] == ' ') {
               sep = q;
               break;
            }
         }

         if (sep != NULL) {
            const char *name_end = sep;
            while (name_end > p && name_end[-1] == ' ')
               name_end--;

            const char *kind_start = sep + 3;
            const char *kind = unit_kind(kind_start, line_end - kind_start);
            const size_t name_len = name_end - p;

            if (!strcmp(kind, "elaborated")) {
               // WORK.TB.elab: the name to pass to nvc_sim_open is TB
               const char *dot = memchr(p, '.', name_len);
               if (dot != NULL && name_len > (size_t)(dot - p) + 6) {
                  LOCAL_TEXT_BUF top = tb_new();
                  tb_catn(top, dot + 1, name_len - (dot - p) - 1 - 5);
                  tb_downcase(top);
                  jw_string(&tw, tb_get(top));
               }
            }
            else if (nunits++ < MAX_LIBRARY_UNITS) {
               shown++;

               jw_begin_object(&uw);
               jw_key(&uw, "name");
               jw_stringn(&uw, p, name_len);
               jw_key(&uw, "kind");
               jw_string(&uw, kind);

               if (!strcmp(kind, "architecture")) {
                  // LIB.ENTITY-ARCH
                  const char *dot = memchr(p, '.', name_len);
                  const char *dash = memchr(p, '-', name_len);
                  if (dot != NULL && dash != NULL && dash > dot) {
                     jw_key(&uw, "entity");
                     jw_stringn(&uw, dot + 1, dash - dot - 1);
                     jw_key(&uw, "architecture");
                     jw_stringn(&uw, dash + 1, name_end - dash - 1);
                  }
               }

               jw_end_object(&uw);
            }
         }

         p = nl ? nl + 1 : end;
      }

      jw_end_array(&uw);
      jw_end_array(&tw);

      jw_begin_object(out);
      jw_key(out, "library");
      jw_string(out, library);
      jw_key(out, "units");
      jw_raw(out, tb_get(units));
      jw_key(out, "elaborated");
      jw_raw(out, tb_get(tops));
      jw_key(out, "total");
      jw_int(out, nunits);
      jw_key(out, "truncated");
      jw_bool(out, nunits > shown);
      jw_end_object(out);

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
   {
      .name          = "nvc_elaborate",
      .title         = "Elaborate a design",
      .description   = elaborate_description,
      .input_schema  = elaborate_input,
      .output_schema = analyse_output,
      .example       = elaborate_example,
      .read_only     = false,
      .destructive   = false,
      .idempotent    = true,
      .fn            = tool_elaborate,
   },
   {
      .name          = "nvc_workspace_list",
      .title         = "List source files",
      .description   = workspace_description,
      .input_schema  = workspace_input,
      .output_schema = workspace_output,
      .example       = workspace_example,
      .read_only     = true,
      .destructive   = false,
      .idempotent    = true,
      .fn            = tool_workspace_list,
   },
   {
      .name          = "nvc_library_list",
      .title         = "List analysed design units",
      .description   = library_description,
      .input_schema  = library_input,
      .output_schema = library_output,
      .example       = library_example,
      .read_only     = true,
      .destructive   = false,
      .idempotent    = true,
      .fn            = tool_library_list,
   },
};

const mcp_tool_t *mcp_tool_table(size_t *count)
{
   // The order is fixed so clients can cache the list
   static mcp_tool_t all[40];
   static size_t n;

   if (n == 0) {
      for (size_t i = 0; i < ARRAY_LEN(tools); i++)
         all[n++] = tools[i];

      size_t nsim;
      const mcp_tool_t *sim = mcp_sim_tools(&nsim);
      for (size_t i = 0; i < nsim; i++)
         all[n++] = sim[i];

      const mcp_tool_t *files = mcp_file_tools(&nsim);
      for (size_t i = 0; i < nsim; i++)
         all[n++] = files[i];

      assert(n <= ARRAY_LEN(all));
   }

   *count = n;
   return all;
}
