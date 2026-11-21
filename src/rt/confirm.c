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

// Confirmation of a changed design by segments.  The original run left
// checkpoints C1..Cn.  Segment i runs the changed design from C(i-1) (from
// time zero for the first) to the time of Ci, saves, and compares the state
// with Ci.  If all agree then by induction the changed design run from zero
// reaches Cn: the first segment starts from the same reset state, each
// restore is verified to put exactly the saved state back, and the run is
// deterministic.  See CHECKPOINT-DESIGN.md section 12.

#include "util.h"
#include "rt/checkpoint.h"
#include "rt/confirm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef enum { SEG_TODO, SEG_RUNNING, SEG_SAME, SEG_DIFFERENT, SEG_INCOMPLETE,
               SEG_CANCELLED } seg_state_t;

typedef struct {
   char        *from_path, *to_path;
   uint64_t     from_fs, to_fs;
   seg_state_t  state;
   char        *detail;
   pid_t        pid;
   char        *dir;
   double       cpu_s, start_s;
   bool         resumed;
   uint64_t     key;           // What the result depends on
   bool         retried;       // Run again to see if the difference repeats
   double       cpu_s_first;
} segment_t;

static volatile sig_atomic_t interrupted;

static void on_signal(int sig)
{
   interrupted = 1;
}

static double now_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int str_cmp(const void *a, const void *b)
{
   return strcmp(*(char *const *)a, *(char *const *)b);
}

static void remove_tree(const char *path)
{
   DIR *d = opendir(path);
   if (d != NULL) {
      for (struct dirent *e; (e = readdir(d)); ) {
         if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
         char *sub = xasprintf("%s/%s", path, e->d_name);
         struct stat st;
         if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode))
            remove_tree(sub);
         else
            unlink(sub);
         free(sub);
      }
      closedir(d);
   }
   rmdir(path);
}

static char *read_file(const char *path)
{
   FILE *f = fopen(path, "r");
   if (f == NULL)
      return NULL;

   char *buf = xmalloc(65536);
   const size_t n = fread(buf, 1, 65535, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}

// First line of an error message in the output of a segment
static char *failure_reason(const char *dir)
{
   char *path = xasprintf("%s/err.txt", dir);
   char *text = read_file(path);
   free(path);
   if (text == NULL)
      return xstrdup("the segment produced no output");

   char *reason = NULL;
   const char *p = strstr(text, "cannot restore checkpoint: ");
   if (p == NULL)
      p = strstr(text, "** Fatal:");
   if (p == NULL)
      p = strstr(text, "** Error:");
   if (p != NULL) {
      const char *nl = strchr(p, '\n');
      reason = xstrndup(p, nl ? (size_t)(nl - p) : strlen(p));
   }
   else
      reason = xstrdup("the segment ended before reaching the time of the "
                       "checkpoint");

   free(text);
   return reason;
}

static void stage_inputs(const char *segdir, char **inputs, unsigned n)
{
   // Files the design reads are made available by name under the directory
   // of the segment, which is otherwise empty so that what the design
   // writes does not collide between segments
   for (unsigned i = 0; i < n; i++) {
      const char *name = inputs[i];
      if (name[0] == '/' || strstr(name, "..") != NULL)
         continue;

      char *src = realpath(name, NULL);
      if (src == NULL)
         continue;

      char *dst = xasprintf("%s/%s", segdir, name);
      for (char *slash = strchr(dst + strlen(segdir) + 1, '/'); slash;
           slash = strchr(slash + 1, '/')) {
         *slash = '\0';
         mkdir(dst, 0777);
         *slash = '/';
      }
      if (symlink(src, dst) != 0 && errno != EEXIST)
         warnf("cannot make input file %s available: %s", name,
               strerror(errno));
      free(dst);
      free(src);
   }
}

static void start_segment(const confirm_opts_t *o, segment_t *s, unsigned idx,
                          char **inputs, unsigned ninputs)
{
   s->dir = xasprintf("%s/seg-%020"PRIu64, o->dir, s->to_fs);
   remove_tree(s->dir);
   mkdir(s->dir, 0777);
   stage_inputs(s->dir, inputs, ninputs);

   unsigned nchild = 0;
   while (o->child_args[nchild] != NULL)
      nchild++;

   char **argv = xcalloc_array(nchild + 12, sizeof(char *));
   unsigned n = 0;
   for (unsigned i = 0; i < nchild; i++)
      argv[n++] = o->child_args[i];

   argv[n++] = "-r";
   if (s->from_path != NULL) {
      argv[n++] = xasprintf("--restore=%s", s->from_path);
      argv[n++] = "--allow-changes";
      argv[n++] = xasprintf("--verdict=%s/verdict.json", s->dir);
   }
   argv[n++] = xasprintf("--checkpoint=%s/out.ckpt@%"PRIu64"fs", s->dir,
                         s->to_fs);
   argv[n++] = xasprintf("--stop-time=%"PRIu64"fs", s->to_fs);
   argv[n++] = (char *)o->top;
   argv[n] = NULL;

   s->start_s = now_s();
   s->pid = fork();
   if (s->pid == 0) {
      if (chdir(s->dir) != 0)
         _exit(127);

      setenv("NVC_CKPT_ANYDIR", "1", 1);

      int out = open("out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
      int err = open("err.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
      int in = open("/dev/null", O_RDONLY);
      dup2(in, 0);
      dup2(out, 1);
      dup2(err, 2);
      execv(argv[0], argv);
      _exit(127);
   }

   s->state = SEG_RUNNING;
   (void)idx;
}

static void finish_segment(segment_t *s, int status, const struct rusage *ru)
{
   s->cpu_s = ru->ru_utime.tv_sec + ru->ru_utime.tv_usec / 1e6
      + ru->ru_stime.tv_sec + ru->ru_stime.tv_usec / 1e6;
   s->pid = 0;

   char *out = xasprintf("%s/out.ckpt", s->dir);
   struct stat st;
   const bool have = stat(out, &st) == 0;

   if (!have) {
      s->state = SEG_INCOMPLETE;
      s->detail = failure_reason(s->dir);

      // The original run went on to this time; if the changed design ends
      // first (a failed assertion, or nothing left to do) it does not
      // behave as the original did, provided the segment started from the
      // state it was meant to
      char *errpath = xasprintf("%s/err.txt", s->dir);
      char *errtxt = read_file(errpath);
      free(errpath);
      const bool ended = errtxt != NULL
         && strstr(errtxt, "before the checkpoint time") != NULL;
      free(errtxt);

      if (ended) {
         bool verified = s->from_path == NULL;
         if (!verified) {
            char *vpath = xasprintf("%s/verdict.json", s->dir);
            char *v = read_file(vpath);
            free(vpath);
            verified = v != NULL
               && strstr(v, "\"state_verified\":true") != NULL;
            free(v);
         }

         if (verified) {
            s->state = SEG_DIFFERENT;
            char *msg = xasprintf("the changed design ends before the time "
                                  "of the checkpoint: %s", s->detail);
            free(s->detail);
            s->detail = msg;
         }
      }

      free(out);
      return;
   }

   // The restore that started the segment must have put back exactly the
   // state that was saved, or the segment says nothing about the original
   if (s->from_path != NULL) {
      char *vpath = xasprintf("%s/verdict.json", s->dir);
      char *v = read_file(vpath);
      free(vpath);
      if (v == NULL || strstr(v, "\"state_verified\":true") == NULL) {
         s->state = SEG_INCOMPLETE;
         s->detail = xstrdup("the restore into the changed design could not "
                             "be verified to reproduce the saved state");
         free(v);
         free(out);
         return;
      }
      free(v);
   }

   // A segment that is run twice from the same checkpoint must end in the
   // same state: bytes that are not part of what the design computes, such
   // as variables of loops not yet entered, can differ from run to run, and
   // a difference that is not repeated says nothing about the design
   char *first = xasprintf("%s.first.ckpt", s->dir);
   if (s->retried) {
      char *again = checkpoint_compare(out, first);
      unlink(first);
      if (again != NULL) {
         s->state = SEG_INCOMPLETE;
         free(s->detail);
         s->detail = xasprintf("two runs of this segment from the same "
                               "checkpoint end in different states (%s), so "
                               "it cannot be compared with the original",
                               again);
         free(again);
         free(first);
         free(out);
         return;
      }
   }

   char *diff = checkpoint_compare(out, s->to_path);
   if (diff == NULL)
      s->state = SEG_SAME;
   else if (!s->retried) {
      // Keep this state and run the segment once more
      s->retried = true;
      s->state = SEG_TODO;
      s->detail = diff;
      s->cpu_s_first = s->cpu_s;
      rename(out, first);
   }
   else {
      s->state = SEG_DIFFERENT;
      free(s->detail);
      s->detail = diff;
   }

   free(first);
   free(out);
   (void)status;
}

static void json_str(FILE *f, const char *s)
{
   fputc('"', f);
   for (; s && *s; s++) {
      if (*s == '"' || *s == '\\')
         fputc('\\', f);
      if ((unsigned char)*s < 0x20)
         fprintf(f, "\\u%04x", *s);
      else
         fputc(*s, f);
   }
   fputc('"', f);
}

static const char *state_name(seg_state_t st)
{
   static const char *names[] = {
      "not run", "running", "same", "different", "incomplete", "cancelled"
   };
   return names[st];
}


static uint64_t hash_bytes(uint64_t h, const void *p, size_t n)
{
   const unsigned char *b = p;
   for (size_t i = 0; i < n; i++)
      h = (h ^ b[i]) * 0x100000001b3ull;
   return h;
}

static uint64_t hash_file(uint64_t h, const char *path)
{
   FILE *f = fopen(path, "rb");
   if (f == NULL)
      return hash_bytes(h, "missing", 7);

   char buf[65536];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      h = hash_bytes(h, buf, n);
   fclose(f);
   return h ^ 0x9e3779b97f4a7c15ull;
}

static uint64_t hash_tree(uint64_t h, const char *path, unsigned depth)
{
   // Names and contents of everything below a library directory
   struct stat st;
   if (stat(path, &st) != 0)
      return hash_bytes(h, "none", 4);
   if (!S_ISDIR(st.st_mode))
      return hash_file(h, path);
   if (depth > 8)
      return h;

   char **names = NULL;
   unsigned n = 0;
   DIR *d = opendir(path);
   if (d == NULL)
      return hash_bytes(h, "unreadable", 10);
   for (struct dirent *e; (e = readdir(d)); ) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
         continue;
      names = xrealloc_array(names, n + 1, sizeof(char *));
      names[n++] = xstrdup(e->d_name);
   }
   closedir(d);
   qsort(names, n, sizeof(char *), str_cmp);

   for (unsigned i = 0; i < n; i++) {
      h = hash_bytes(h, names[i], strlen(names[i]) + 1);
      char *sub = xasprintf("%s/%s", path, names[i]);
      h = hash_tree(h, sub, depth + 1);
      free(sub);
      free(names[i]);
   }
   free(names);
   return h;
}

// Everything the result of a segment depends on other than the two
// checkpoints: this program, the design libraries and the way it is run
static uint64_t design_key(const confirm_opts_t *o, char **inputs,
                           unsigned ninputs)
{
   uint64_t h = 0xcbf29ce484222325ull;
   h = hash_file(h, "/proc/self/exe");
   h = hash_bytes(h, o->top, strlen(o->top) + 1);

   for (unsigned i = 0; o->child_args[i] != NULL; i++) {
      const char *a = o->child_args[i];
      h = hash_bytes(h, a, strlen(a) + 1);
      const char *lib = NULL;
      if (!strncmp(a, "--work=", 7))
         lib = a + 7;
      else if (!strncmp(a, "-L", 2) && a[2] != '\0')
         lib = a + 2;
      else if (!strcmp(a, "-L") && o->child_args[i + 1] != NULL)
         lib = o->child_args[i + 1];
      if (lib != NULL)
         h = hash_tree(h, lib, 0);
   }

   const char *libpath = getenv("NVC_LIBPATH");
   if (libpath != NULL) {
      char *copy = xstrdup(libpath);
      char *sp = NULL;
      for (char *tok = strtok_r(copy, ":", &sp); tok;
           tok = strtok_r(NULL, ":", &sp))
         h = hash_tree(h, tok, 0);
      free(copy);
   }

   for (unsigned i = 0; i < ninputs; i++) {
      h = hash_bytes(h, inputs[i], strlen(inputs[i]) + 1);
      h = hash_file(h, inputs[i]);
   }

   return h;
}

static void save_progress(const char *dir, segment_t *segs, unsigned n)
{
   char *path = xasprintf("%s/confirm-progress.txt", dir);
   FILE *f = fopen(path, "w");
   free(path);
   if (f == NULL)
      return;

   for (unsigned i = 0; i < n; i++) {
      if (segs[i].state == SEG_SAME)
         fprintf(f, "%"PRIu64" %"PRIu64" same %.3f %016"PRIx64"\n",
                 segs[i].from_fs, segs[i].to_fs, segs[i].cpu_s, segs[i].key);
   }
   fclose(f);
}

static void load_progress(const char *dir, segment_t *segs, unsigned n)
{
   char *path = xasprintf("%s/confirm-progress.txt", dir);
   FILE *f = fopen(path, "r");
   free(path);
   if (f == NULL)
      return;

   uint64_t from, to;
   char what[16];
   double cpu;
   uint64_t key;
   while (fscanf(f, "%"SCNu64" %"SCNu64" %15s %lf %"SCNx64, &from, &to, what,
                 &cpu, &key) == 5) {
      for (unsigned i = 0; i < n; i++) {
         // Only if the checkpoints at both ends are as they were
         if (segs[i].from_fs == from && segs[i].to_fs == to
             && segs[i].key == key && !strcmp(what, "same")) {
            segs[i].state = SEG_SAME;
            segs[i].cpu_s = cpu;
            segs[i].resumed = true;
         }
      }
   }
   fclose(f);
}

int confirm_run(const confirm_opts_t *opts)
{
   // The segments run in other directories
   confirm_opts_t abs_opts = *opts;
   abs_opts.dir = realpath(opts->dir, NULL);
   if (abs_opts.dir == NULL)
      fatal_errno("cannot open %s", opts->dir);
   const confirm_opts_t *o = &abs_opts;

   // The checkpoints, oldest first
   char **names = NULL;
   unsigned nck = 0;
   DIR *d = opendir(o->dir);
   if (d == NULL)
      fatal_errno("cannot open %s", o->dir);

   for (struct dirent *e; (e = readdir(d)); ) {
      const size_t len = strlen(e->d_name);
      if (strncmp(e->d_name, "ck-", 3) == 0 && len > 8
          && strcmp(e->d_name + len - 5, ".ckpt") == 0) {
         names = xrealloc_array(names, nck + 1, sizeof(char *));
         names[nck++] = xstrdup(e->d_name);
      }
   }
   closedir(d);

   if (nck == 0)
      fatal("no checkpoints in %s: the original run needs "
            "--checkpoint=%s@every=INTERVAL", o->dir, o->dir);

   qsort(names, nck, sizeof(char *), str_cmp);

   segment_t *segs = xcalloc(nck * sizeof(segment_t));
   char **inputs = NULL;
   unsigned ninputs = 0;
   uint64_t prev = 0;
   char *prev_path = NULL;
   bool any_wave = false;

   for (unsigned i = 0; i < nck; i++) {
      char *path = xasprintf("%s/%s", o->dir, names[i]);
      ckpt_info_t info;
      char *err = checkpoint_describe(path, &info);
      if (err != NULL)
         fatal("%s", err);

      segment_t *s = &(segs[i]);
      s->from_path = prev_path;
      s->from_fs = prev;
      s->to_path = path;
      s->to_fs = info.time_fs;
      s->state = SEG_TODO;
      if (info.wave) {
         s->state = SEG_INCOMPLETE;
         any_wave = true;
         s->detail = xstrdup("the checkpoint was written with waveform "
                             "dumping on, which changes the delta cycle "
                             "numbers in the state, so it cannot be compared");
         if (i + 1 < nck)
            ;   // The next segment starts from it: it is marked below
      }

      for (unsigned k = 0; k < info.ninputs; k++) {
         bool have = false;
         for (unsigned j = 0; j < ninputs; j++)
            have |= !strcmp(inputs[j], info.inputs[k]);
         if (!have) {
            inputs = xrealloc_array(inputs, ninputs + 1, sizeof(char *));
            inputs[ninputs++] = xstrdup(info.inputs[k]);
         }
      }

      checkpoint_info_free(&info);
      prev = s->to_fs;
      prev_path = path;
   }

   // A segment that starts at a checkpoint taken with dumping cannot be
   // compared either
   for (unsigned i = 1; i < nck; i++) {
      if (segs[i - 1].state == SEG_INCOMPLETE && segs[i].state == SEG_TODO
          && any_wave) {
         segs[i].state = SEG_INCOMPLETE;
         segs[i].detail = xstrdup(segs[i - 1].detail);
      }
   }

   // A result saved by an earlier run counts only if the design, this
   // program, the inputs and the checkpoints at both ends are the same
   const uint64_t dkey = design_key(o, inputs, ninputs);
   for (unsigned i = 0; i < nck; i++) {
      uint64_t k = hash_bytes(dkey, &segs[i].from_fs, sizeof(uint64_t));
      k = hash_bytes(k, &segs[i].to_fs, sizeof(uint64_t));
      if (segs[i].from_path != NULL)
         k = hash_file(k, segs[i].from_path);
      segs[i].key = hash_file(k, segs[i].to_path);
   }

   load_progress(o->dir, segs, nck);

   struct sigaction sa = { .sa_handler = on_signal };
   sigaction(SIGINT, &sa, NULL);
   sigaction(SIGTERM, &sa, NULL);

   const double start = now_s();
   const unsigned jobs = MAX(o->jobs, 1);
   unsigned running = 0, started = 0, first_diff = nck;
   bool budget_out = false;

   for (;;) {
      // Start work, unless a difference has been found at or before it
      for (unsigned i = 0; i < nck && running < jobs && !interrupted; i++) {
         if (segs[i].state != SEG_TODO || i > first_diff)
            continue;

         if ((o->budget_s > 0 && now_s() - start > o->budget_s)
             || (o->max_segments > 0 && started >= o->max_segments)) {
            budget_out = true;
            break;
         }

         start_segment(o, &(segs[i]), i, inputs, ninputs);
         running++;
         started++;
      }

      if (running == 0)
         break;

      int status;
      struct rusage ru;
      pid_t pid = wait4(-1, &status, WNOHANG, &ru);
      if (pid > 0) {
         for (unsigned i = 0; i < nck; i++) {
            if (segs[i].state == SEG_RUNNING && segs[i].pid == pid) {
               finish_segment(&(segs[i]), status, &ru);
               running--;

               if (segs[i].state == SEG_TODO) {
                  // The state differs: see if it does again
                  started--;
                  break;
               }
               segs[i].cpu_s += segs[i].cpu_s_first;

               fprintf(stderr, "confirm: segment %u of %u (%"PRIu64" to "
                       "%"PRIu64" fs): %s%s%s (%.1f s)\n", i + 1, nck,
                       segs[i].from_fs, segs[i].to_fs,
                       state_name(segs[i].state),
                       segs[i].detail ? ": " : "",
                       segs[i].detail ? segs[i].detail : "",
                       now_s() - segs[i].start_s);

               if (segs[i].state == SEG_DIFFERENT && i < first_diff)
                  first_diff = i;
               if (segs[i].state == SEG_SAME)
                  save_progress(o->dir, segs, nck);
               break;
            }
         }
      }
      else if (pid == 0) {
         struct timespec ts = { 0, 20000000 };
         nanosleep(&ts, NULL);
      }

      if (interrupted) {
         for (unsigned i = 0; i < nck; i++) {
            if (segs[i].state == SEG_RUNNING) {
               kill(segs[i].pid, SIGTERM);
               waitpid(segs[i].pid, NULL, 0);
               segs[i].state = SEG_CANCELLED;
               running--;
            }
         }
      }
   }

   save_progress(o->dir, segs, nck);

   // The result.  Only an unbroken run of segments from time zero says
   // anything about the design run from zero.
   unsigned prefix = 0;
   while (prefix < nck && segs[prefix].state == SEG_SAME)
      prefix++;

   const uint64_t covered = prefix > 0 ? segs[prefix - 1].to_fs : 0;
   const uint64_t total = segs[nck - 1].to_fs;

   const char *result;
   int rc;
   if (first_diff < nck && first_diff == prefix) {
      result = "different";
      rc = 1;
   }
   else if (prefix == nck) {
      result = "confirmed";
      rc = 0;
   }
   else if (first_diff < nck) {
      // There is a difference but an earlier segment is not settled
      result = "different";
      rc = 1;
   }
   else {
      result = "incomplete";
      rc = 2;
   }

   double cpu = 0;
   for (unsigned i = 0; i < nck; i++)
      cpu += segs[i].cpu_s;

   FILE *f = o->result_file ? fopen(o->result_file, "w") : stdout;
   if (f == NULL)
      fatal_errno("%s", o->result_file);

   fprintf(f, "{\"result\":\"%s\",\"confirmed_up_to_fs\":%"PRIu64
           ",\"final_checkpoint_fs\":%"PRIu64",\"segments\":%u,"
           "\"wall_s\":%.3f,\"cpu_s\":%.3f,\"budget_exhausted\":%s,"
           "\"interrupted\":%s", result, covered, total, nck, now_s() - start,
           cpu, budget_out ? "true" : "false", interrupted ? "true" : "false");

   if (first_diff < nck) {
      fprintf(f, ",\"first_difference\":{\"segment\":%u,\"from_fs\":%"PRIu64
              ",\"to_fs\":%"PRIu64",\"detail\":", first_diff + 1,
              segs[first_diff].from_fs, segs[first_diff].to_fs);
      json_str(f, segs[first_diff].detail);
      fputc('}', f);
   }

   fputs(",\"segment_results\":[", f);
   for (unsigned i = 0; i < nck; i++) {
      fprintf(f, "%s{\"segment\":%u,\"from_fs\":%"PRIu64",\"to_fs\":%"PRIu64
              ",\"state\":\"%s\",\"resumed\":%s,\"cpu_s\":%.3f,\"detail\":",
              i ? "," : "", i + 1, segs[i].from_fs, segs[i].to_fs,
              state_name(segs[i].state), segs[i].resumed ? "true" : "false",
              segs[i].cpu_s);
      json_str(f, segs[i].detail);
      fputc('}', f);
   }
   fputs("]}\n", f);

   if (f != stdout)
      fclose(f);

   // The directories of the segments are not needed once settled
   for (unsigned i = 0; i < nck; i++) {
      if (segs[i].dir != NULL && segs[i].state != SEG_DIFFERENT
          && segs[i].state != SEG_INCOMPLETE)
         remove_tree(segs[i].dir);
   }

   return interrupted ? 130 : rc;
}
