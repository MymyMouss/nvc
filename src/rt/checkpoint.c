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

// Typed save and restore of a simulation to a file.
//
// The state of the model is written as a stream of values identified by
// name (signal path, process name, variable name) and never by address.
// A restore creates a fresh model of the same design, runs the normal
// reset, and then overwrites the dynamic state from the file.  Afterwards
// the model is serialised again and compared with the file contents: if
// they differ the restore is rejected, so a restore is either exact or
// refused.  See CHECKPOINT-DESIGN.md.

#include "util.h"
#include "array.h"
#include "common.h"
#include "diag.h"
#include "hash.h"
#include "ident.h"
#include "jit/jit.h"
#include "lib.h"
#include "mir/mir-unit.h"
#include "rt/assert.h"
#include "rt/checkpoint.h"
#include "rt/fileio.h"
#include "rt/heap.h"
#include "rt/model.h"
#include "rt/model-priv.h"
#include "rt/mspace.h"
#include "rt/structs.h"
#include "tree.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CKPT_MAGIC   "NVCCKPT1"
#define CKPT_VERSION 2

////////////////////////////////////////////////////////////////////////////////
// Output and input buffers

typedef struct {
   uint8_t  *d;
   size_t    n, cap;
   hash_t   *names;     // ident_t to index + 1
   ident_t  *nlist;     // The names in index order
   unsigned  nnames, nmax;
   char     *err;
} wbuf_t;

typedef struct {
   const uint8_t *p, *end;
   ident_t       *names;
   unsigned       nnames, maxnames;
   char          *err;
} rbuf_t;

__attribute__((format(printf, 2, 3)))
static void w_fail(wbuf_t *w, const char *fmt, ...)
{
   if (w->err != NULL)
      return;

   va_list ap;
   va_start(ap, fmt);
   if (vasprintf(&w->err, fmt, ap) < 0)
      w->err = xstrdup("out of memory");
   va_end(ap);
}

__attribute__((format(printf, 2, 3)))
static void r_fail(rbuf_t *r, const char *fmt, ...)
{
   if (r->err != NULL)
      return;

   va_list ap;
   va_start(ap, fmt);
   if (vasprintf(&r->err, fmt, ap) < 0)
      r->err = xstrdup("out of memory");
   va_end(ap);
}

static void w_init(wbuf_t *w)
{
   memset(w, '\0', sizeof(*w));
   w->names = hash_new(256);
}

static void w_free(wbuf_t *w)
{
   hash_free(w->names);
   free(w->nlist);
   free(w->d);
   free(w->err);
}

static void *w_reserve(wbuf_t *w, size_t k)
{
   if (w->n + k > w->cap) {
      w->cap = MAX(w->cap * 2, MAX(w->n + k, 4096));
      w->d = xrealloc(w->d, w->cap);
   }

   void *p = w->d + w->n;
   w->n += k;
   return p;
}

static void w_u8(wbuf_t *w, uint8_t v) { *(uint8_t *)w_reserve(w, 1) = v; }

static void w_u32(wbuf_t *w, uint32_t v) { memcpy(w_reserve(w, 4), &v, 4); }

static void w_u64(wbuf_t *w, uint64_t v) { memcpy(w_reserve(w, 8), &v, 8); }

static void w_bytes(wbuf_t *w, const void *p, size_t k)
{
   if (k > 0)
      memcpy(w_reserve(w, k), p, k);
}

static void w_tag(wbuf_t *w, const char *tag) { w_bytes(w, tag, 4); }

static void w_str(wbuf_t *w, const char *s)
{
   const size_t len = strlen(s);
   w_u32(w, len);
   w_bytes(w, s, len);
}

// Names are collected in a table written before the rest, so a record
// can be skipped without reading it
static unsigned w_nameidx(wbuf_t *w, ident_t id)
{
   uintptr_t idx = (uintptr_t)hash_get(w->names, id);
   if (idx != 0)
      return idx - 1;

   if (w->nnames == w->nmax) {
      w->nmax = MAX(256, w->nmax * 2);
      w->nlist = xrealloc_array(w->nlist, w->nmax, sizeof(ident_t));
   }

   w->nlist[w->nnames] = id;
   hash_put(w->names, id, (void *)(uintptr_t)(w->nnames + 1));
   return w->nnames++;
}

static void w_name(wbuf_t *w, ident_t id)
{
   w_u32(w, w_nameidx(w, id));
}

// The body of the file prefixed by its table of names
static uint8_t *w_finish(wbuf_t *w, size_t *len)
{
   wbuf_t t;
   memset(&t, '\0', sizeof(t));
   w_tag(&t, "NAMT");
   w_u32(&t, w->nnames);
   for (unsigned i = 0; i < w->nnames; i++)
      w_str(&t, istr(w->nlist[i]));
   w_bytes(&t, w->d, w->n);
   *len = t.n;
   return t.d;
}

// The empty string cannot be an identifier
static ident_t empty_ident(void)
{
   return ident_new("\001");
}

static const char *name_str(ident_t id)
{
   const char *s = istr(id);
   return s[0] == '\001' ? "" : s;
}

static void w_cname(wbuf_t *w, const char *s)
{
   w_name(w, *s ? ident_new(s) : empty_ident());
}

// A record is framed by its length so a reader that does not recognise it
// can skip it
static size_t w_rec_begin(wbuf_t *w)
{
   w_u32(w, 0);
   return w->n;
}

static void w_rec_end(wbuf_t *w, size_t start)
{
   const uint32_t len = w->n - start;
   memcpy(w->d + start - 4, &len, 4);
}

static bool r_ok(rbuf_t *r, size_t k)
{
   if (r->err != NULL)
      return false;
   else if ((size_t)(r->end - r->p) < k) {
      r_fail(r, "checkpoint file is truncated or corrupt");
      return false;
   }
   return true;
}

static uint8_t r_u8(rbuf_t *r)
{
   if (!r_ok(r, 1))
      return 0;
   return *r->p++;
}

static uint32_t r_u32(rbuf_t *r)
{
   uint32_t v = 0;
   if (r_ok(r, 4)) {
      memcpy(&v, r->p, 4);
      r->p += 4;
   }
   return v;
}

static uint64_t r_u64(rbuf_t *r)
{
   uint64_t v = 0;
   if (r_ok(r, 8)) {
      memcpy(&v, r->p, 8);
      r->p += 8;
   }
   return v;
}

static const uint8_t *r_bytes(rbuf_t *r, size_t k)
{
   if (!r_ok(r, k))
      return NULL;
   const uint8_t *p = r->p;
   r->p += k;
   return p;
}

static bool r_tag(rbuf_t *r, const char *tag)
{
   const uint8_t *p = r_bytes(r, 4);
   if (p == NULL || memcmp(p, tag, 4) != 0) {
      r_fail(r, "expected section %s in checkpoint file", tag);
      return false;
   }
   return true;
}

static ident_t r_name(rbuf_t *r)
{
   const uint32_t idx = r_u32(r);
   if (r->err != NULL)
      return NULL;
   else if (idx >= r->nnames) {
      r_fail(r, "bad name index in checkpoint file");
      return NULL;
   }
   return r->names[idx];
}

static bool r_names_load(rbuf_t *r)
{
   if (!r_tag(r, "NAMT"))
      return false;

   const uint32_t n = r_u32(r);
   if (r->err == NULL && n > (size_t)(r->end - r->p)) {
      r_fail(r, "checkpoint file is corrupt");
      return false;
   }

   r->names = xcalloc(MAX(n, 1) * sizeof(ident_t));
   r->maxnames = n;
   for (unsigned i = 0; i < n && r->err == NULL; i++) {
      const uint32_t len = r_u32(r);
      const uint8_t *str = r_bytes(r, len);
      if (str != NULL)
         r->names[r->nnames++] = ident_new_n((const char *)str, len);
   }

   return r->err == NULL;
}

////////////////////////////////////////////////////////////////////////////////
// Frames

typedef enum {
   FRAME_SCOPE, FRAME_PROCESS, FRAME_PACKAGE, FRAME_PROCEDURE
} frame_kind_t;

typedef struct {
   frame_kind_t   kind;
   ident_t        name;
   jit_handle_t   handle;
   void          *data;
   jit_framelay_t lay;
   size_t         size;
   rt_proc_t     *proc;
   rt_scope_t    *scope;
} frame_t;

typedef void (*frame_fn_t)(const frame_t *, void *);

static size_t layout_size(const jit_framelay_t *lay)
{
   size_t size = lay->hdrsz;
   for (unsigned i = 0; i < lay->nvars; i++) {
      if (lay->vars[i].name != NULL)
         size = MAX(size, lay->vars[i].offset + lay->vars[i].type->size);
   }
   return size;
}

static void each_scope_frame(rt_model_t *m, jit_t *jit, rt_scope_t *s,
                             frame_fn_t fn, void *ctx)
{
   if (s->kind == SCOPE_INSTANCE && s->privdata != MPTR_INVALID) {
      frame_t f = { .kind = FRAME_SCOPE, .name = s->name, .scope = s };
      f.data = *mptr_get(s->privdata);
      f.handle = jit_lazy_compile(jit, s->name);
      if (f.data != NULL && f.handle != JIT_HANDLE_INVALID
          && jit_get_framelay(jit, f.handle, &f.lay, true)) {
         f.size = layout_size(&f.lay);
         (*fn)(&f, ctx);
      }
   }

   for (int i = 0; i < s->procs.count; i++) {
      rt_proc_t *p = s->procs.items[i];
      frame_t f = { .kind = FRAME_PROCESS, .name = p->name, .proc = p,
                    .scope = s };
      f.handle = p->closure.handle;
      f.data = *mptr_get(p->privdata);
      if (f.data != NULL && jit_get_framelay(jit, f.handle, &f.lay, true)) {
         f.size = layout_size(&f.lay);
         (*fn)(&f, ctx);
      }
   }

   for (int i = 0; i < s->children.count; i++)
      each_scope_frame(m, jit, s->children.items[i], fn, ctx);
}

static void each_package_frame(jit_t *jit, frame_fn_t fn, void *ctx)
{
   const unsigned n = jit_handle_count(jit);
   for (jit_handle_t h = 0; h < n; h++) {
      void *data = jit_get_privdata(jit, h);
      if (data == NULL)
         continue;

      frame_t f = { .kind = FRAME_PACKAGE, .handle = h, .data = data };
      if (!jit_get_framelay(jit, h, &f.lay, false)
          || f.lay.kind != MIR_UNIT_PACKAGE)
         continue;

      f.name = jit_get_name(jit, h);
      f.size = layout_size(&f.lay);
      (*fn)(&f, ctx);
   }
}

static void each_frame(rt_model_t *m, jit_t *jit, frame_fn_t fn, void *ctx)
{
   each_scope_frame(m, jit, root_scope(m), fn, ctx);
   each_package_frame(jit, fn, ctx);
}

// The compiler makes up names for some variables with a counter that
// depends on the order in which units happened to be lowered, such as
// right24 or RAM.ename3, so these differ between processes.  They are
// recognised by a lower case final component (user identifiers are upper
// case) and compared without the number.
static const char *stable_name(ident_t id, char *buf, size_t len)
{
   const char *s = istr(id);
   const char *comp = strrchr(s, '.');
   comp = comp ? comp + 1 : s;

   if (!(*comp >= 'a' && *comp <= 'z'))
      return s;

   size_t n = strlen(s);
   while (n > 0 && s[n - 1] >= '0' && s[n - 1] <= '9')
      n--;

   if (n == 0 || n >= len)
      return s;

   memcpy(buf, s, n);
   buf[n] = '\0';
   return buf;
}

// Variable names are not always unique within a frame: the second and
// later occurrences get a numeric suffix
static ident_t var_key(const frame_t *f, unsigned nth)
{
   char b1[256], b2[256];
   const jit_var_t *v = &(f->lay.vars[nth]);
   const char *name = stable_name(v->name, b1, sizeof(b1));

   unsigned dup = 0;
   for (unsigned i = 0; i < nth; i++) {
      if (f->lay.vars[i].name != NULL
          && strcmp(stable_name(f->lay.vars[i].name, b2, sizeof(b2)),
                    name) == 0)
         dup++;
   }

   return dup == 0 ? ident_new(name) : ident_sprintf("%s#%u", name, dup);
}

////////////////////////////////////////////////////////////////////////////////
// Index of the named objects in a model

typedef enum {
   U_SAME,       // Same code
   U_COMPAT,     // Changed, but only code that had never run
   U_APPROX,     // Changed code had run
   U_REMOVED,    // No longer in the design
} unit_class_t;

typedef struct {
   unit_class_t cls;
   bool         old_restart, new_restart;
   int          new_state;     // Resume state of the new code, if it restarts
} unit_t;

__attribute__((format(printf, 2, 3)))
static void report_note(ckpt_report_t *rep, const char *fmt, ...)
{
   if (rep == NULL || rep->nnotes >= 50)
      return;

   va_list ap;
   va_start(ap, fmt);
   char *msg = NULL;
   if (vasprintf(&msg, fmt, ap) < 0)
      msg = xstrdup("?");
   va_end(ap);

   rep->notes = xrealloc_array(rep->notes, rep->nnotes + 1, sizeof(char *));
   rep->notes[rep->nnotes++] = msg;
}

void checkpoint_report_free(ckpt_report_t *rep)
{
   for (unsigned i = 0; i < rep->nchanged; i++)
      free(rep->changed[i]);
   for (unsigned i = 0; i < rep->nnotes; i++)
      free(rep->notes[i]);
   free(rep->changed);
   free(rep->executed);
   free(rep->notes);
   memset(rep, '\0', sizeof(*rep));
}

static void json_escape(text_buf_t *tb, const char *s)
{
   tb_append(tb, '"');
   for (; *s; s++) {
      if (*s == '"' || *s == '\\')
         tb_append(tb, '\\');
      if ((unsigned char)*s < 0x20)
         tb_printf(tb, "\\u%04x", *s);
      else
         tb_append(tb, *s);
   }
   tb_append(tb, '"');
}

char *checkpoint_report_json(const ckpt_report_t *rep)
{
   LOCAL_TEXT_BUF tb = tb_new();
   tb_printf(tb, "{\"verdict\":\"%s\",\"tracked\":%s,\"changed_units\":[",
             rep->exact ? "exact" : "approximate",
             rep->tracked ? "true" : "false");
   for (unsigned i = 0; i < rep->nchanged; i++) {
      tb_printf(tb, "%s{\"unit\":", i ? "," : "");
      json_escape(tb, rep->changed[i]);
      tb_printf(tb, ",\"executed_before_checkpoint\":%s}",
                rep->executed[i] ? "true" : "false");
   }
   tb_cat(tb, "],\"reasons\":[");
   for (unsigned i = 0; i < rep->nnotes; i++) {
      if (i > 0)
         tb_append(tb, ',');
      json_escape(tb, rep->notes[i]);
   }
   tb_cat(tb, "]}");
   return xstrdup(tb_get(tb));
}

typedef struct {
   char        *path;
   rt_signal_t *sig;
} sigent_t;

// Arrays on the heap that frame variables point into.  Only the part that
// is referred to is saved: ranges are unioned across all the references.
typedef struct {
   const uint8_t *base;
   size_t         lo, hi;
   unsigned       esize;    // Zero if referred to as a whole object
   unsigned       fixup;    // Offset the first word must point to, if any
} obj_t;

typedef enum { RANGE_FRAME, RANGE_CPOOL } rkind_t;

typedef struct {
   uintptr_t    lo, hi;
   rkind_t kind;
   ident_t      name;
   unsigned     fkind;
   size_t       size;
} range_t;

typedef struct {
   rt_model_t   *m;
   jit_t        *jit;
   sigent_t     *sigs;
   unsigned      nsigs;
   hash_t       *sigptr;      // sig_shared_t pointer to sigent_t
   shash_t      *sigpath;     // Path to sigent_t
   rt_proc_t   **procs;
   unsigned      nprocs;
   shash_t      *procname;
   frame_t      *frames;
   unsigned      nframes;
   hash_t       *frameptr;    // Data pointer to frame_t
   shash_t      *framename;   // "kind:name" to frame_t
   range_t      *ranges;
   unsigned      nranges;
   rt_trigger_t **trigs;
   unsigned      ntrigs;
   hash_t       *trigptr;
   shash_t      *trigname;
   char        **trignames;
   hash_t       *transptr;    // Signal transfer to the name of its process
   shash_t      *transname;
   file_handle_t *fh;         // Open files, in canonical order
   file_ckpt_t  *fst;
   unsigned      nfiles;
   tlab_t      **tlabs;       // Private TLABs of processes in a procedure
   unsigned      ntlabs;
   frame_t     **xframes;     // Procedure frames made by a restore
   unsigned      nxframes;
   obj_t        *objs;        // Heap arrays referred to from frames
   unsigned      nobjs;
   hash_t       *objmap;      // Base address to index + 1
   bool          collect;     // First pass: find the objects only
   bool          objs_done;
   void        **robjs;       // Restore: the objects as allocated
   mptr_t       *rroots;      // and the roots keeping them alive
   unsigned      nrobjs;
   file_ckpt_t  *sfiles;      // Files named by a checkpoint being restored
   file_handle_t *sfh;        // Handle each was given in this model
   unsigned      nsfiles;
   bool          allow;       // Restoring into a changed design
   ckpt_report_t *rep;
   shash_t      *units;       // Code of the checkpoint against this build
   char         *err;
} index_t;

static const char *frame_kind_name(frame_kind_t k)
{
   static const char *names[] = {
      "instance", "process", "package", "procedure"
   };
   return names[k];
}

static char *frame_key(frame_kind_t k, ident_t name)
{
   return xasprintf("%d:%s", k, istr(name));
}

static void collect_signals(rt_scope_t *s, sigent_t **arr, unsigned *n,
                            unsigned *max)
{
   for (int i = 0; i < s->signals.count; i++) {
      if (*n == *max) {
         *max = MAX(64, *max * 2);
         *arr = xrealloc_array(*arr, *max, sizeof(sigent_t));
      }

      (*arr)[(*n)++] = (sigent_t){ .sig = s->signals.items[i] };
   }

   for (int i = 0; i < s->children.count; i++)
      collect_signals(s->children.items[i], arr, n, max);
}

static char *signal_path(rt_signal_t *s)
{
   rt_scope_t *chain[64];
   int n = 0;
   for (rt_scope_t *it = s->parent;
        it != NULL && it->kind != SCOPE_ROOT && n < ARRAY_LEN(chain);
        it = it->parent)
      chain[n++] = it;

   LOCAL_TEXT_BUF tb = tb_new();
   while (n > 0) {
      tb_istr(tb, chain[--n]->name);
      tb_append(tb, '/');
   }
   tb_istr(tb, tree_ident(s->where));

   return xstrdup(tb_get(tb));
}

static int sigent_cmp(const void *a, const void *b)
{
   return strcmp(((const sigent_t *)a)->path, ((const sigent_t *)b)->path);
}

static void collect_procs(rt_scope_t *s, rt_proc_t ***arr, unsigned *n,
                          unsigned *max)
{
   for (int i = 0; i < s->procs.count; i++) {
      if (*n == *max) {
         *max = MAX(64, *max * 2);
         *arr = xrealloc_array(*arr, *max, sizeof(rt_proc_t *));
      }

      (*arr)[(*n)++] = s->procs.items[i];
   }

   for (int i = 0; i < s->children.count; i++)
      collect_procs(s->children.items[i], arr, n, max);
}

static int proc_cmp(const void *a, const void *b)
{
   return strcmp(istr((*(rt_proc_t *const *)a)->name),
                 istr((*(rt_proc_t *const *)b)->name));
}

static int frame_cmp(const void *a, const void *b)
{
   const frame_t *fa = a, *fb = b;
   if (fa->kind != fb->kind)
      return fa->kind - fb->kind;
   return strcmp(istr(fa->name), istr(fb->name));
}

static int range_cmp(const void *a, const void *b)
{
   const range_t *ra = a, *rb = b;
   return ra->lo < rb->lo ? -1 : ra->lo > rb->lo;
}

static int trig_cmp(const void *a, const void *b)
{
   const rt_trigger_t *ta = *(rt_trigger_t *const *)a;
   const rt_trigger_t *tb = *(rt_trigger_t *const *)b;
   return ta->id < tb->id ? -1 : ta->id > tb->id;
}

static void index_frame_cb(const frame_t *f, void *arg)
{
   index_t *ix = arg;
   ix->frames = xrealloc_array(ix->frames, ix->nframes + 1, sizeof(frame_t));
   ix->frames[ix->nframes++] = *f;
}

static void index_free(index_t *ix)
{
   for (unsigned i = 0; i < ix->nsigs; i++)
      free(ix->sigs[i].path);
   for (unsigned i = 0; i < ix->ntrigs; i++)
      free(ix->trignames[i]);
   free(ix->sigs);
   free(ix->procs);
   free(ix->frames);
   free(ix->ranges);
   free(ix->trigs);
   free(ix->trignames);
   for (unsigned i = 0; i < ix->nfiles; i++)
      file_ckpt_free(&(ix->fst[i]));
   for (unsigned i = 0; i < ix->nsfiles; i++)
      file_ckpt_free(&(ix->sfiles[i]));
   free(ix->fh);
   free(ix->fst);
   free(ix->sfiles);
   free(ix->sfh);
   free(ix->err);
   hash_free(ix->sigptr);
   shash_free(ix->sigpath);
   shash_free(ix->procname);
   hash_free(ix->frameptr);
   shash_free(ix->framename);
   hash_free(ix->trigptr);
   shash_free(ix->trigname);
   hash_free(ix->transptr);
   shash_free(ix->transname);
   hash_free(ix->objmap);
   free(ix->objs);
   for (unsigned i = 0; i < ix->nxframes; i++)
      free(ix->xframes[i]);
   free(ix->xframes);
   free(ix->tlabs);
   free(ix->robjs);
   free(ix->rroots);
}

static void index_build(index_t *ix, rt_model_t *m, jit_t *jit)
{
   memset(ix, '\0', sizeof(*ix));
   ix->m   = m;
   ix->jit = jit;

   ix->sigptr    = hash_new(256);
   ix->sigpath   = shash_new(256);
   ix->procname  = shash_new(64);
   ix->frameptr  = hash_new(64);
   ix->framename = shash_new(64);
   ix->trigptr   = hash_new(64);
   ix->trigname  = shash_new(64);
   ix->transptr  = hash_new(64);
   ix->transname = shash_new(64);
   ix->objmap    = hash_new(64);

   unsigned max = 0;
   collect_signals(root_scope(m), &ix->sigs, &ix->nsigs, &max);
   for (unsigned i = 0; i < ix->nsigs; i++)
      ix->sigs[i].path = signal_path(ix->sigs[i].sig);
   qsort(ix->sigs, ix->nsigs, sizeof(sigent_t), sigent_cmp);

   for (unsigned i = 0; i < ix->nsigs; i++) {
      sigent_t *e = &(ix->sigs[i]);
      if (shash_get(ix->sigpath, e->path) != NULL) {
         if (ix->err == NULL)
            ix->err = xasprintf("signal path %s is not unique", e->path);
      }
      shash_put(ix->sigpath, e->path, e);
      hash_put(ix->sigptr, &(e->sig->shared), e);
   }

   max = 0;
   collect_procs(root_scope(m), &ix->procs, &ix->nprocs, &max);
   qsort(ix->procs, ix->nprocs, sizeof(rt_proc_t *), proc_cmp);
   for (unsigned i = 0; i < ix->nprocs; i++) {
      if (shash_get(ix->procname, istr(ix->procs[i]->name)) != NULL
          && ix->err == NULL)
         ix->err = xasprintf("process name %s is not unique",
                             istr(ix->procs[i]->name));
      shash_put(ix->procname, istr(ix->procs[i]->name), ix->procs[i]);
   }

   each_frame(m, jit, index_frame_cb, ix);

   // Frames of procedures a process is suspended in are reached only
   // through the process frame and named after it and their depth
   const unsigned nstatic = ix->nframes;
   for (unsigned i = 0; i < nstatic; i++) {
      const frame_t pf = ix->frames[i];
      if (pf.kind != FRAME_PROCESS)
         continue;

      void *next = *(void **)((uint8_t *)pf.data + sizeof(void *));
      for (unsigned depth = 0; next != NULL; depth++) {
         const jit_handle_t h =
            *(int32_t *)((uint8_t *)next + 2 * sizeof(void *) + 4);

         frame_t f = { .kind = FRAME_PROCEDURE, .data = next, .proc = pf.proc,
                       .scope = pf.scope };
         if (h < 0 || h >= (jit_handle_t)jit_handle_count(jit)
             || !jit_get_framelay(jit, h, &f.lay, true)
             || f.lay.kind != MIR_UNIT_PROCEDURE) {
            if (ix->err == NULL)
               ix->err = xasprintf("process %s is suspended in a procedure "
                                   "whose frame is not recognised",
                                   istr(pf.name));
            break;
         }

         f.handle = h;
         f.name = ident_sprintf("%s/%u", istr(pf.name), depth);
         f.size = layout_size(&f.lay);
         index_frame_cb(&f, ix);

         next = *(void **)((uint8_t *)next + sizeof(void *));
      }

      if (pf.proc->tlab != NULL) {
         ix->tlabs = xrealloc_array(ix->tlabs, ix->ntlabs + 1,
                                    sizeof(tlab_t *));
         ix->tlabs[ix->ntlabs++] = pf.proc->tlab;
      }
   }

   qsort(ix->frames, ix->nframes, sizeof(frame_t), frame_cmp);

   const unsigned nhandles = jit_handle_count(jit);
   ix->ranges = xmalloc_array(ix->nframes + nhandles, sizeof(range_t));

   for (unsigned i = 0; i < ix->nframes; i++) {
      frame_t *f = &(ix->frames[i]);
      hash_put(ix->frameptr, f->data, f);
      shash_put(ix->framename, frame_key(f->kind, f->name), f);
      ix->ranges[ix->nranges++] = (range_t) {
         .lo = (uintptr_t)f->data,
         .hi = (uintptr_t)f->data + f->size,
         .kind = RANGE_FRAME,
         .name = f->name,
         .fkind = f->kind,
         .size = f->size,
      };
   }

   for (jit_handle_t h = 0; h < nhandles; h++) {
      const void *base;
      size_t size;
      if (jit_get_cpool(jit, h, &base, &size, false) && size > 0) {
         ix->ranges[ix->nranges++] = (range_t) {
            .lo = (uintptr_t)base,
            .hi = (uintptr_t)base + size,
            .kind = RANGE_CPOOL,
            .name = jit_get_name(jit, h),
            .size = size,
         };
      }
   }

   qsort(ix->ranges, ix->nranges, sizeof(range_t), range_cmp);

   for (int i = 0; i < TRIGGER_TAB_SIZE; i++) {
      for (rt_trigger_t *t = m->triggertab[i]; t; t = t->chain) {
         ix->trigs = xrealloc_array(ix->trigs, ix->ntrigs + 1,
                                    sizeof(rt_trigger_t *));
         ix->trigs[ix->ntrigs++] = t;
      }
   }

   qsort(ix->trigs, ix->ntrigs, sizeof(rt_trigger_t *), trig_cmp);

   ix->trignames = xmalloc_array(ix->ntrigs, sizeof(char *));

   // Triggers are named after the process that created them and their
   // order within it, which is deterministic as they are made at reset
   hash_t *counts = hash_new(64);
   for (unsigned i = 0; i < ix->ntrigs; i++) {
      rt_trigger_t *t = ix->trigs[i];
      uintptr_t k = (uintptr_t)hash_get(counts, t->owner);
      hash_put(counts, t->owner, (void *)(k + 1));
      ix->trignames[i] = xasprintf("%s#trigger%u",
                                   t->owner ? istr(t->owner) : "", (unsigned)k);
      shash_put(ix->trigname, ix->trignames[i], t);
      hash_put(ix->trigptr, t, ix->trignames[i]);
   }
   hash_free(counts);

   // Signal transfers are made by the process that implements a simple
   // assignment and are only reachable from the lists of waiting objects
   for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
      void *pend = n->pending;
      rt_wakeable_t *single = NULL, **list = NULL;
      unsigned count = 0;
      if (pend == NULL)
         continue;
      else if (pointer_tag(pend) == 1) {
         single = untag_pointer(pend, rt_wakeable_t);
         list = &single;
         count = 1;
      }
      else {
         rt_pending_t *p = untag_pointer(pend, rt_pending_t);
         list = p->wake;
         count = p->count;
      }

      for (unsigned i = 0; i < count; i++) {
         if (list[i] == NULL || list[i]->kind != W_TRANSFER)
            continue;

         rt_transfer_t *t = container_of(list[i], rt_transfer_t, wakeable);
         if (hash_get(ix->transptr, t) != NULL)
            continue;

         const char *name = t->proc ? istr(t->proc->name) : "";
         if (shash_get(ix->transname, name) != NULL && ix->err == NULL)
            ix->err = xasprintf("process %s makes more than one signal "
                                "transfer", name);

         shash_put(ix->transname, name, t);
         hash_put(ix->transptr, t, (void *)name);
      }
   }
}

typedef struct {
   file_handle_t fh;
   file_ckpt_t   st;
   unsigned      order;
} fent_t;

static int fent_cmp(const void *a, const void *b)
{
   const fent_t *fa = a, *fb = b;
   if (fa->st.predef != fb->st.predef)
      return fb->st.predef - fa->st.predef;
   else if (fa->st.mode != fb->st.mode)
      return fa->st.mode - fb->st.mode;

   const int c = strcmp(fa->st.name, fb->st.name);
   return c ?: (int)fa->order - (int)fb->order;
}

// Describe the files that are open, in an order that does not depend on
// which slot each one happens to occupy
static void index_files(index_t *ix)
{
   if (ix->fst != NULL || ix->err != NULL)
      return;

   file_handle_t all[256];
   const unsigned n = file_enum_open(all, ARRAY_LEN(all));
   if (n == ARRAY_LEN(all)) {
      ix->err = xstrdup("too many files are open");
      return;
   }

   fent_t *ents = xmalloc_array(MAX(n, 1), sizeof(fent_t));
   for (unsigned i = 0; i < n; i++) {
      ents[i].fh = all[i];
      ents[i].order = i;
      char *err = file_ckpt_describe(all[i], &(ents[i].st));
      if (err != NULL) {
         if (ix->err == NULL)
            ix->err = xasprintf("file %s: %s", ents[i].st.name ?: "?", err);
         free(err);
      }
   }

   qsort(ents, n, sizeof(fent_t), fent_cmp);

   ix->fh  = xmalloc_array(MAX(n, 1), sizeof(file_handle_t));
   ix->fst = xmalloc_array(MAX(n, 1), sizeof(file_ckpt_t));
   ix->nfiles = n;
   for (unsigned i = 0; i < n; i++) {
      ix->fh[i]  = ents[i].fh;
      ix->fst[i] = ents[i].st;
   }

   free(ents);
}

static const unit_t *unit_lookup(const index_t *ix, jit_handle_t h)
{
   // Without the option the code is known to be the same
   if (!ix->allow || ix->units == NULL || h == JIT_HANDLE_INVALID)
      return NULL;
   return shash_get(ix->units, istr(jit_get_name(ix->jit, h)));
}

static const range_t *find_range(const index_t *ix, const void *ptr)
{
   const uintptr_t p = (uintptr_t)ptr;
   int lo = 0, hi = (int)ix->nranges - 1, best = -1;
   while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      if (ix->ranges[mid].lo <= p) {
         best = mid;
         lo = mid + 1;
      }
      else
         hi = mid - 1;
   }

   // Ranges may nest so walk back over any that start before the pointer
   for (int i = best; i >= 0 && i > best - 4; i--) {
      if (p >= ix->ranges[i].lo && p < ix->ranges[i].hi)
         return &(ix->ranges[i]);
   }

   return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// Typed values

// Ordered by how much work is needed to save the value
typedef enum {
   CLS_PLAIN, CLS_REF, CLS_RESOLUTION, CLS_FILE, CLS_UARRAY, CLS_POINTER,
   CLS_ACCESS,
} vt_class_t;

static const char *class_names[] = {
   "plain", "ref", "resolution", "file", "uarray", "pointer", "access"
};

static vt_class_t vt_class(const jit_vtype_t *vt)
{
   switch (vt->kind) {
   case VT_INT: case VT_OFFSET: case VT_REAL: case VT_VEC2: case VT_VEC4:
      return CLS_PLAIN;
   case VT_SIGNAL: case VT_CONTEXT: case VT_TRIGGER:
      return CLS_REF;
   case VT_RESOLUTION: return CLS_RESOLUTION;
   case VT_FILE: return CLS_FILE;
   case VT_UARRAY: return CLS_UARRAY;
   case VT_POINTER: return CLS_POINTER;
   case VT_ACCESS: return CLS_ACCESS;
   case VT_CARRAY: return vt_class(vt->elem);
   case VT_RECORD:
      {
         vt_class_t worst = CLS_PLAIN;
         for (unsigned i = 0; i < vt->count; i++)
            worst = MAX(worst, vt_class(vt->fields[i].type));
         return worst;
      }
   }

   return CLS_ACCESS;
}

// How much of a variable is saved by copying bytes
static bool vt_is_flat(const jit_vtype_t *vt)
{
   switch (vt->kind) {
   case VT_INT: case VT_OFFSET: case VT_REAL: case VT_VEC2: case VT_VEC4:
      return true;
   case VT_CARRAY:
      return vt_is_flat(vt->elem);
   default:
      return false;
   }
}


typedef struct {
   index_t     *ix;
   wbuf_t      *w;
   rbuf_t      *r;
   const char  *unit;
   const char  *var;
} vctx_t;

__attribute__((format(printf, 2, 3)))
static void v_fail(vctx_t *c, const char *fmt, ...)
{
   char *why = NULL;
   va_list ap;
   va_start(ap, fmt);
   if (vasprintf(&why, fmt, ap) < 0)
      why = xstrdup("?");
   va_end(ap);

   if (c->w != NULL)
      w_fail(c->w, "%s variable %s: %s", c->unit, c->var, why);
   else
      r_fail(c->r, "%s variable %s: %s", c->unit, c->var, why);

   free(why);
}

// Pointers inside frames may refer to the frame itself, to another frame
// (a constant in a package), or to constant data owned by a compiled unit.
// Anything else, notably heap objects, cannot be saved yet.
static bool save_target(vctx_t *c, const void *ptr)
{
   wbuf_t *w = c->w;

   if (ptr == NULL) {
      w_u8(w, 0);
      return true;
   }

   const range_t *r = find_range(c->ix, ptr);
   if (r == NULL) {
      size_t sz;
      if (mspace_find(jit_get_mspace(c->ix->jit), (void *)ptr, &sz) != NULL)
         v_fail(c, "points to an object allocated on the heap");
      else
         v_fail(c, "points to memory that cannot be identified");
      return false;
   }

   if (r->kind == RANGE_FRAME)
      w_u8(w, 1 + r->fkind);
   else
      w_u8(w, 7);

   w_name(w, r->name);
   w_u32(w, (uintptr_t)ptr - r->lo);
   return true;
}

// The length of an array from its bounds: the sign records the direction
static size_t uarray_count(const uint8_t *bounds, unsigned ndims)
{
   size_t count = 1;
   for (unsigned d = 0; d < ndims; d++) {
      int64_t len = *(const int64_t *)(bounds + (2 * d + 1) * sizeof(int64_t));
      if (len < 0)
         len = ~len;
      count *= len;
   }
   return count;
}

// Record a reference to [off, off + bytes) of the heap object at base and
// write the reference
static bool note_object(vctx_t *c, const uint8_t *base, size_t off,
                        size_t bytes, unsigned esize, unsigned fixup)
{
   index_t *ix = c->ix;
   wbuf_t *w = c->w;

   uintptr_t idx = (uintptr_t)hash_get(ix->objmap, base);
   if (idx == 0) {
      if (!ix->collect) {
         v_fail(c, "points to a heap object that was not found in the first "
                "pass");
         return false;
      }

      ix->objs = xrealloc_array(ix->objs, ix->nobjs + 1, sizeof(obj_t));
      ix->objs[ix->nobjs] = (obj_t){
         .base = base, .lo = off, .hi = off + bytes, .esize = esize,
         .fixup = fixup,
      };
      idx = ++ix->nobjs;
      hash_put(ix->objmap, base, (void *)idx);
   }
   else if (ix->collect) {
      obj_t *o = &(ix->objs[idx - 1]);
      if (o->esize != 0 && esize != 0 && o->esize != esize) {
         v_fail(c, "points to a heap array also used with another element "
                "type");
         return false;
      }
      else if (o->fixup != 0 && fixup != 0 && o->fixup != fixup) {
         v_fail(c, "points to a heap object of two different types");
         return false;
      }

      o->lo = MIN(o->lo, off);
      o->hi = MAX(o->hi, off + bytes);
      if (esize == 0)
         o->esize = 0;
      if (fixup != 0)
         o->fixup = fixup;
   }

   const obj_t *o = &(ix->objs[idx - 1]);
   w_u8(w, 5);
   w_u32(w, idx - 1);
   w_u32(w, off - o->lo);
   return true;
}

static const uint8_t *heap_base(index_t *ix, const void *ptr)
{
   size_t sz;
   const uint8_t *base = mspace_find(jit_get_mspace(ix->jit), (void *)ptr, &sz);
   for (unsigned i = 0; base == NULL && i < ix->ntlabs; i++) {
      // Temporary arrays of a suspended procedure live in the TLAB
      const uint8_t *lo = (uint8_t *)ix->tlabs[i]->data;
      if ((const uint8_t *)ptr >= lo
          && (const uint8_t *)ptr < lo + ix->tlabs[i]->alloc)
         base = lo;
   }
   return base;
}

static bool save_array_data(vctx_t *c, const void *ptr,
                            const jit_vtype_t *elem, size_t count)
{
   index_t *ix = c->ix;
   wbuf_t *w = c->w;

   if (count == 0) {
      w_u8(w, 6);   // The address of an empty array is of no consequence
      return true;
   }
   else if (ptr == NULL || find_range(ix, ptr) != NULL)
      return save_target(c, ptr);

   const uint8_t *base = heap_base(ix, ptr);
   if (base == NULL)
      return save_target(c, ptr);   // Fails with a message

   if (vt_class(elem) != CLS_PLAIN) {
      v_fail(c, "points to a heap array whose elements hold pointers or "
             "references");
      return false;
   }

   return note_object(c, base, (const uint8_t *)ptr - base,
                      count * elem->size, elem->size, 0);
}

// A value of an access type designates a whole object on the heap
static bool save_access(vctx_t *c, const void *ptr, const jit_vtype_t *to)
{
   index_t *ix = c->ix;

   if (ptr == NULL) {
      w_u8(c->w, 0);
      return true;
   }
   else if (to == NULL) {
      v_fail(c, "holds an access value to an object of a type that cannot "
             "be saved");
      return false;
   }

   // The designated type is the pointer to the object's value
   if (to->kind == VT_POINTER && to->elem != NULL)
      to = to->elem;

   const uint8_t *base = heap_base(ix, ptr);
   if (base != ptr) {
      v_fail(c, "holds an access value that does not point to an object "
             "allocated by new");
      return false;
   }

   if (to->kind == VT_UARRAY) {
      // The object is the bounds followed by the array, with a pointer
      // from one to the other
      if (to->elem->kind == VT_SIGNAL || vt_class(to->elem) != CLS_PLAIN) {
         v_fail(c, "holds an access value to an array whose elements hold "
                "pointers or references");
         return false;
      }

      // The body normally follows the header but may start later if
      // characters have been consumed from the front, as TEXTIO does
      const size_t hdr = to->size;
      const ptrdiff_t body = *(const uint8_t **)ptr - base;
      if (body < (ptrdiff_t)hdr || body > (1 << 24)) {
         v_fail(c, "holds an access value to an array with an unexpected "
                "layout (body at +%td, header %zu)", body, hdr);
         return false;
      }

      const size_t count = uarray_count(base + sizeof(void *), to->count);
      return note_object(c, base, 0, body + count * to->elem->size, 0, body);
   }
   else if (vt_class(to) != CLS_PLAIN) {
      v_fail(c, "holds an access value to an object that holds pointers or "
             "references");
      return false;
   }

   return note_object(c, base, 0, to->size, 0, 0);
}

static void *restore_target(vctx_t *c)
{
   rbuf_t *r = c->r;

   const uint8_t kind = r_u8(r);
   if (kind == 0 || kind == 6 || r->err != NULL)
      return NULL;

   if (kind == 5) {
      const uint32_t id = r_u32(r);
      const uint32_t off = r_u32(r);
      if (r->err != NULL)
         return NULL;
      else if (id >= c->ix->nrobjs) {
         v_fail(c, "refers to heap object %u which is not in the checkpoint",
                id);
         return NULL;
      }
      return (uint8_t *)c->ix->robjs[id] + off;
   }

   ident_t name = r_name(r);
   const uint32_t off = r_u32(r);
   if (r->err != NULL)
      return NULL;

   if (kind >= 1 && kind <= 4) {
      char *key = frame_key(kind - 1, name);
      const frame_t *f = shash_get(c->ix->framename, key);
      free(key);

      if (f == NULL)
         v_fail(c, "refers to %s %s which does not exist in this model",
                frame_kind_name(kind - 1), istr(name));
      else if (off >= f->size && off > 0)
         v_fail(c, "offset %u is outside of %s", off, istr(name));
      else
         return (uint8_t *)f->data + off;
   }
   else if (kind == 7) {
      jit_handle_t h = jit_lazy_compile(c->ix->jit, name);
      const void *base;
      size_t size;
      if (h == JIT_HANDLE_INVALID
          || !jit_get_cpool(c->ix->jit, h, &base, &size, true))
         v_fail(c, "refers to constant data of %s which is missing",
                istr(name));
      else if (off >= size)
         v_fail(c, "constant data offset %u is outside of %s", off,
                istr(name));
      else
         return (uint8_t *)base + off;
   }
   else
      v_fail(c, "bad pointer kind %d", kind);

   return NULL;
}

static bool save_signal_ref(vctx_t *c, const void *shared)
{
   wbuf_t *w = c->w;

   if (shared == NULL) {
      w_u8(w, 0);
      return true;
   }

   const sigent_t *e = hash_get(c->ix->sigptr, shared);
   if (e == NULL) {
      v_fail(c, "refers to a signal that is not in the design");
      return false;
   }

   w_u8(w, 1);
   w_cname(w, e->path);
   return true;
}

static void *restore_signal_ref(vctx_t *c)
{
   rbuf_t *r = c->r;
   if (r_u8(r) == 0 || r->err != NULL)
      return NULL;

   ident_t path = r_name(r);
   if (r->err != NULL)
      return NULL;

   const sigent_t *e = shash_get(c->ix->sigpath, istr(path));
   if (e == NULL) {
      v_fail(c, "refers to signal %s which does not exist", istr(path));
      return NULL;
   }

   return &(e->sig->shared);
}

static bool save_value(vctx_t *c, const jit_vtype_t *vt, const uint8_t *p)
{
   wbuf_t *w = c->w;

   switch (vt->kind) {
   case VT_INT: case VT_OFFSET: case VT_REAL: case VT_VEC2: case VT_VEC4:
      w_bytes(w, p, vt->size);
      return true;

   case VT_CARRAY:
      if (vt_is_flat(vt->elem)) {
         w_bytes(w, p, (size_t)vt->count * vt->elem->size);
         return true;
      }

      for (unsigned i = 0; i < vt->count; i++) {
         if (!save_value(c, vt->elem, p + (size_t)i * vt->elem->size))
            return false;
      }
      return true;

   case VT_RECORD:
      for (unsigned i = 0; i < vt->count; i++) {
         if (!save_value(c, vt->fields[i].type, p + vt->fields[i].offset))
            return false;
      }
      return true;

   case VT_SIGNAL:
      if (!save_signal_ref(c, *(void **)p))
         return false;
      w_u32(w, *(uint32_t *)(p + sizeof(void *)));
      return true;

   case VT_CONTEXT:
      {
         void *ptr = *(void **)p;
         if (ptr == NULL)
            w_u8(w, 0);
         else {
            const frame_t *f = hash_get(c->ix->frameptr, ptr);
            if (f == NULL) {
               v_fail(c, "refers to a context that is not a unit frame");
               return false;
            }
            w_u8(w, 1 + f->kind);
            w_name(w, f->name);
         }
         return true;
      }

   case VT_TRIGGER:
      {
         void *ptr = *(void **)p;
         if (ptr == NULL)
            w_u8(w, 0);
         else {
            const char *name = hash_get(c->ix->trigptr, ptr);
            if (name == NULL) {
               v_fail(c, "refers to a trigger that is not registered");
               return false;
            }
            w_u8(w, 1);
            w_cname(w, name);
         }
         return true;
      }

   case VT_RESOLUTION:
      // Created once at elaboration from a constant resolution function
      // so the value from the reset is already correct
      return true;

   case VT_FILE:
      {
         const uint32_t fh = *(uint32_t *)p;
         unsigned id = 0;
         for (unsigned i = 0; fh != 0 && i < c->ix->nfiles; i++) {
            if (c->ix->fh[i] == fh)
               id = i + 1;
         }

         if (fh != 0 && id == 0) {
            v_fail(c, "holds a file handle that is not open");
            return false;
         }

         w_u32(w, id);
         return true;
      }

   case VT_UARRAY:
      {
         void *ptr = *(void **)p;
         const bool sig = vt->elem->kind == VT_SIGNAL;
         const size_t hdr = sig ? 2 * sizeof(void *) : sizeof(void *);

         if (sig) {
            if (!save_signal_ref(c, ptr))
               return false;
            w_u64(w, *(uint64_t *)(p + sizeof(void *)));
         }
         else if (!save_array_data(c, ptr, vt->elem,
                                   uarray_count(p + hdr, vt->count)))
            return false;

         w_bytes(w, p + hdr, 2 * sizeof(int64_t) * vt->count);
         return true;
      }

   case VT_ACCESS:
      return save_access(c, *(void **)p, vt->elem);

   case VT_POINTER:
      return save_target(c, *(void **)p);
   }

   return false;
}

static bool restore_value(vctx_t *c, const jit_vtype_t *vt, uint8_t *p)
{
   rbuf_t *r = c->r;

   switch (vt->kind) {
   case VT_INT: case VT_OFFSET: case VT_REAL: case VT_VEC2: case VT_VEC4:
      {
         const uint8_t *b = r_bytes(r, vt->size);
         if (b != NULL)
            memcpy(p, b, vt->size);
         return b != NULL;
      }

   case VT_CARRAY:
      if (vt_is_flat(vt->elem)) {
         const size_t k = (size_t)vt->count * vt->elem->size;
         const uint8_t *b = r_bytes(r, k);
         if (b != NULL && k > 0)
            memcpy(p, b, k);
         return b != NULL || k == 0;
      }

      for (unsigned i = 0; i < vt->count; i++) {
         if (!restore_value(c, vt->elem, p + (size_t)i * vt->elem->size))
            return false;
      }
      return true;

   case VT_RECORD:
      for (unsigned i = 0; i < vt->count; i++) {
         if (!restore_value(c, vt->fields[i].type, p + vt->fields[i].offset))
            return false;
      }
      return true;

   case VT_SIGNAL:
      *(void **)p = restore_signal_ref(c);
      *(uint32_t *)(p + sizeof(void *)) = r_u32(r);
      return r->err == NULL;

   case VT_CONTEXT:
      {
         const uint8_t kind = r_u8(r);
         if (kind == 0) {
            *(void **)p = NULL;
            return r->err == NULL;
         }

         ident_t name = r_name(r);
         if (r->err != NULL)
            return false;

         char *key = frame_key(kind - 1, name);
         const frame_t *f = shash_get(c->ix->framename, key);
         free(key);
         if (f == NULL) {
            v_fail(c, "refers to %s %s which does not exist",
                   frame_kind_name(kind - 1), istr(name));
            return false;
         }

         *(void **)p = f->data;
         return true;
      }

   case VT_TRIGGER:
      {
         if (r_u8(r) == 0) {
            *(void **)p = NULL;
            return r->err == NULL;
         }

         ident_t name = r_name(r);
         if (r->err != NULL)
            return false;

         rt_trigger_t *t = shash_get(c->ix->trigname, istr(name));
         if (t == NULL) {
            v_fail(c, "refers to trigger %s which does not exist", istr(name));
            return false;
         }

         *(void **)p = t;
         return true;
      }

   case VT_RESOLUTION:
      return true;

   case VT_FILE:
      {
         const uint32_t id = r_u32(r);
         if (r->err != NULL)
            return false;

         index_t *ix = c->ix;
         file_handle_t cur = *(uint32_t *)p;

         if (id == 0) {
            if (cur != 0)
               file_close(cur);
            *(uint32_t *)p = 0;
            return true;
         }
         else if (id > ix->nsfiles) {
            v_fail(c, "refers to file %u which is not in the checkpoint", id);
            return false;
         }

         if (ix->sfh[id - 1] == 0) {
            char *err = file_ckpt_apply(&(ix->sfiles[id - 1]), cur,
                                        &(ix->sfh[id - 1]));
            if (err != NULL) {
               v_fail(c, "%s", err);
               free(err);
               return false;
            }
         }
         else if (cur != 0 && cur != ix->sfh[id - 1])
            file_close(cur);

         *(uint32_t *)p = ix->sfh[id - 1];
         return true;
      }

   case VT_UARRAY:
      {
         const bool sig = vt->elem->kind == VT_SIGNAL;
         const size_t hdr = sig ? 2 * sizeof(void *) : sizeof(void *);

         if (sig) {
            *(void **)p = restore_signal_ref(c);
            *(uint64_t *)(p + sizeof(void *)) = r_u64(r);
         }
         else
            *(void **)p = restore_target(c);

         const size_t k = 2 * sizeof(int64_t) * vt->count;
         const uint8_t *b = r_bytes(r, k);
         if (b != NULL)
            memcpy(p + hdr, b, k);
         return r->err == NULL;
      }

   case VT_POINTER:
   case VT_ACCESS:
      *(void **)p = restore_target(c);
      return r->err == NULL;
   }

   return false;
}

// Combined hash of the layout of a frame so a restore can tell that the
// code which owns it has changed
static uint64_t vt_hash(const jit_vtype_t *vt, uint64_t h)
{
   h = (h ^ vt->kind) * 0x100000001b3ULL;
   h = (h ^ vt->size) * 0x100000001b3ULL;
   h = (h ^ vt->count) * 0x100000001b3ULL;
   if (vt->elem != NULL)
      h = vt_hash(vt->elem, h);
   for (unsigned i = 0; vt->kind == VT_RECORD && i < vt->count; i++) {
      h = (h ^ vt->fields[i].offset) * 0x100000001b3ULL;
      h = vt_hash(vt->fields[i].type, h);
   }
   return h;
}

static uint64_t frame_layout_hash(const frame_t *f)
{
   uint64_t h = 0xcbf29ce484222325ULL;
   for (unsigned i = 0; i < f->lay.nvars; i++) {
      const jit_var_t *v = &(f->lay.vars[i]);
      if (v->name == NULL)
         continue;
      char sbuf[256];
      for (const char *s = stable_name(v->name, sbuf, sizeof(sbuf)); *s; s++)
         h = (h ^ (uint8_t)*s) * 0x100000001b3ULL;
      h = (h ^ (uint64_t)v->offset) * 0x100000001b3ULL;
      h = vt_hash(v->type, h);
   }
   return h;
}

// Save every named variable of a frame
static bool save_frame_vars(index_t *ix, wbuf_t *w, const frame_t *f)
{
   w_u64(w, frame_layout_hash(f));

   unsigned count = 0;
   for (unsigned i = 0; i < f->lay.nvars; i++)
      count += (f->lay.vars[i].name != NULL);
   w_u32(w, count);

   LOCAL_TEXT_BUF tb = tb_new();
   tb_istr(tb, f->name);

   for (unsigned i = 0; i < f->lay.nvars; i++) {
      const jit_var_t *v = &(f->lay.vars[i]);
      if (v->name == NULL)
         continue;

      w_name(w, var_key(f, i));
      w_u64(w, vt_hash(v->type, 0xcbf29ce484222325ULL));
      const size_t rec = w_rec_begin(w);

      vctx_t c = {
         .ix = ix, .w = w,
         .unit = xasprintf("%s %s", frame_kind_name(f->kind), tb_get(tb)),
         .var = istr(v->name),
      };

      const bool ok = save_value(&c, v->type, (uint8_t *)f->data + v->offset);
      free((void *)c.unit);
      if (!ok)
         return false;

      w_rec_end(w, rec);
   }

   return true;
}

static bool restore_frame_vars(index_t *ix, rbuf_t *r, const frame_t *f)
{
   const uint64_t hash = r_u64(r);
   const uint32_t count = r_u32(r);
   if (r->err != NULL)
      return false;

   const bool same_layout = (hash == frame_layout_hash(f));
   if (!same_layout) {
      if (!ix->allow) {
         r_fail(r, "layout of %s %s (%s) has changed since the checkpoint",
                frame_kind_name(f->kind), istr(f->name),
                f->handle == JIT_HANDLE_INVALID ? "?"
                : istr(jit_get_name(ix->jit, f->handle)));
         return false;
      }
      ix->rep->layout_changed = true;
      report_note(ix->rep, "variables of %s %s changed", frame_kind_name(f->kind),
                  istr(f->name));
   }

   shash_t *byname = shash_new(64);
   unsigned nfound = 0;
   for (unsigned i = 0; i < f->lay.nvars; i++) {
      if (f->lay.vars[i].name != NULL)
         shash_put(byname, istr(var_key(f, i)), (void *)(uintptr_t)(i + 1));
   }

   bool ok = true;
   for (unsigned i = 0; ok && i < count; i++) {
      ident_t key = r_name(r);
      const uint64_t thash = r_u64(r);
      const uint32_t len = r_u32(r);
      if (r->err != NULL || !r_ok(r, len)) {
         ok = false;
         break;
      }

      const uint8_t *end = r->p + len;

      uintptr_t idx = (uintptr_t)shash_get(byname, istr(key));
      const jit_var_t *v = idx ? &(f->lay.vars[idx - 1]) : NULL;

      if (v == NULL || thash != vt_hash(v->type, 0xcbf29ce484222325ULL)) {
         if (!ix->allow) {
            r_fail(r, "%s %s has no variable %s", frame_kind_name(f->kind),
                   istr(f->name), istr(key));
            ok = false;
            break;
         }

         // Removed or of a different type: nothing to restore it into
         ix->rep->layout_changed = true;
         report_note(ix->rep, "variable %s of %s %s not restored",
                     istr(key), frame_kind_name(f->kind), istr(f->name));
         r->p = end;
         continue;
      }

      nfound++;

      vctx_t c = {
         .ix = ix, .r = r,
         .unit = xasprintf("%s %s", frame_kind_name(f->kind), istr(f->name)),
         .var = istr(v->name),
      };

      ok = restore_value(&c, v->type, (uint8_t *)f->data + v->offset);
      free((void *)c.unit);

      if (ok && r->p != end) {
         r_fail(r, "variable %s of %s has an unexpected size", istr(key),
                istr(f->name));
         ok = false;
      }
   }

   // Variables the checkpoint knows nothing about keep their initial values
   if (ok && nfound < f->lay.nvars) {
      unsigned nnamed = 0;
      for (unsigned i = 0; i < f->lay.nvars; i++)
         nnamed += (f->lay.vars[i].name != NULL);
      if (nfound < nnamed) {
         if (!ix->allow) {
            r_fail(r, "%s %s has variables that are not in the checkpoint",
                   frame_kind_name(f->kind), istr(f->name));
            ok = false;
         }
         else {
            ix->rep->layout_changed = true;
            report_note(ix->rep, "%s %s has new variables",
                        frame_kind_name(f->kind), istr(f->name));
         }
      }
   }

   shash_free(byname);
   return ok;
}

////////////////////////////////////////////////////////////////////////////////
// Saving the model

static const char *wakeable_what(rt_wakeable_t *obj)
{
   switch (obj->kind) {
   case W_PROC: return "process";
   case W_WATCH: return "value change callback";
   case W_PROPERTY: return "PSL property";
   case W_TRANSFER: return "signal transfer";
   case W_TRIGGER: return "trigger";
   }
   return "?";
}

static bool nexus_first_source_is_inline(rt_nexus_t *n)
{
   return n->n_sources > 0;
}

// The wakeables registered on a nexus or trigger, in list order including
// the empty slots because a new registration takes the first empty slot
static bool save_pending(index_t *ix, wbuf_t *w, void *pending,
                         const char *what)
{
   rt_wakeable_t **list = NULL, *single;
   unsigned count = 0;

   if (pending == NULL)
      ;
   else if (pointer_tag(pending) == 1) {
      single = untag_pointer(pending, rt_wakeable_t);
      list = &single;
      count = 1;
   }
   else {
      rt_pending_t *p = untag_pointer(pending, rt_pending_t);
      list = p->wake;
      count = p->count;
   }

   // Value change callbacks belong to tools (waveform dumper) rather than
   // to the design and may be added later without changing the order of
   // processes in the list
   unsigned keep = 0;
   for (unsigned i = 0; i < count; i++) {
      if (list[i] == NULL || list[i]->kind != W_WATCH)
         keep++;
   }

   w_u32(w, keep);

   for (unsigned i = 0; i < count; i++) {
      rt_wakeable_t *obj = list[i];
      if (obj == NULL)
         w_u8(w, 0);
      else if (obj->kind == W_WATCH)
         continue;
      else if (obj->kind == W_PROC) {
         rt_proc_t *p = container_of(obj, rt_proc_t, wakeable);
         w_u8(w, 1);
         w_name(w, p->name);
      }
      else if (obj->kind == W_TRIGGER) {
         rt_trigger_t *t = container_of(obj, rt_trigger_t, wakeable);
         const char *name = hash_get(ix->trigptr, t);
         if (name == NULL) {
            w_fail(w, "%s is waiting on an unregistered trigger", what);
            return false;
         }
         w_u8(w, 2);
         w_cname(w, name);
      }
      else if (obj->kind == W_TRANSFER) {
         rt_transfer_t *t = container_of(obj, rt_transfer_t, wakeable);
         const char *name = hash_get(ix->transptr, t);
         if (name == NULL || t->wakeable.pending) {
            w_fail(w, "%s has a signal transfer that is queued or unknown",
                   what);
            return false;
         }
         w_u8(w, 3);
         w_cname(w, name);
      }
      else {
         w_fail(w, "%s has a %s waiting on it, which cannot be saved",
                what, wakeable_what(obj));
         return false;
      }
   }

   return true;
}

static bool save_source(index_t *ix, wbuf_t *w, rt_nexus_t *n,
                        rt_source_t *src, const char *what)
{
   w_u8(w, src->tag);
   w_u8(w, src->disconnected | (src->was_active << 1));

   if (src->fastqueued || src->sigqueued) {
      w_fail(w, "%s has a driver update queued: not at a quiescent point",
             what);
      return false;
   }

   const size_t valsz = (size_t)n->width * n->size;

   switch (src->tag) {
   case SOURCE_DRIVER:
      {
         rt_proc_t *p = src->u.driver.proc;
         w_name(w, p ? p->name : empty_ident());

         waveform_t *wf = &(src->u.driver.waveforms);
         w_u64(w, wf->when);
         w_bytes(w, model_value_ptr(n, &wf->value), valsz);

         unsigned nfuture = 0;
         for (waveform_t *it = wf->next; it; it = it->next)
            nfuture++;
         w_u32(w, nfuture);

         for (waveform_t *it = wf->next; it; it = it->next) {
            w_u64(w, it->when);
            w_bytes(w, model_value_ptr(n, &it->value), valsz);
         }
      }
      return true;

   case SOURCE_PORT:
      {
         rt_nexus_t *in = src->u.port.input;
         for (unsigned i = 0; i < ix->nsigs; i++) {
            if (ix->sigs[i].sig == in->signal) {
               w_cname(w, ix->sigs[i].path);
               w_u32(w, in->offset / in->size);
               return true;
            }
         }
         w_fail(w, "%s is connected to a signal that is not in the design",
                what);
         return false;
      }

   default:
      w_fail(w, "%s has a %s source (forced or deposited value)", what,
             src->tag == SOURCE_FORCING ? "forcing" : "deposit");
      return false;
   }
}

static bool save_signals(index_t *ix, wbuf_t *w)
{
   w_tag(w, "LAYT");
   w_u32(w, ix->nsigs);

   for (unsigned i = 0; i < ix->nsigs; i++) {
      rt_signal_t *s = ix->sigs[i].sig;
      w_cname(w, ix->sigs[i].path);
      w_u32(w, s->shared.size);
      w_u32(w, s->nexus.size);
      w_u32(w, s->n_nexus);

      unsigned k = 0;
      for (rt_nexus_t *n = &(s->nexus); n != NULL && k < s->n_nexus;
           n = n->chain, k++) {
         w_u32(w, n->offset / n->size);
         w_u32(w, n->width);
      }

      if (k != s->n_nexus) {
         w_fail(w, "signal %s has an inconsistent nexus list",
                ix->sigs[i].path);
         return false;
      }
   }

   w_tag(w, "SIGS");
   w_u32(w, ix->nsigs);

   for (unsigned i = 0; i < ix->nsigs; i++) {
      rt_signal_t *s = ix->sigs[i].sig;
      const char *path = ix->sigs[i].path;

      w_cname(w, path);
      const size_t sigrec = w_rec_begin(w);
      w_u32(w, s->shared.flags);

      // Signals whose events are cached are found at run time
      bool cached = false;
      for (unsigned k = 0; k < ix->m->eventsigs.count; k++)
         cached |= (ix->m->eventsigs.items[k] == s);
      w_u8(w, cached);

      w_bytes(w, s->shared.data, 3 * (size_t)s->shared.size);

      unsigned k = 0;
      for (rt_nexus_t *n = &(s->nexus); k < s->n_nexus; n = n->chain, k++) {
         char *what = xasprintf("signal %s", path);

         w_u8(w, n->flags);
         w_u32(w, n->active_delta);
         w_u32(w, n->event_delta);
         w_u64(w, n->last_event);
         w_u8(w, n->rank);
         w_u8(w, n->n_sources);

         if (n->flags & NET_F_PENDING) {
            w_fail(w, "%s has an update queued: not at a quiescent point",
                   what);
            free(what);
            return false;
         }

         for (rt_source_t *o = n->outputs; o; o = o->chain_output) {
            if (o->tag == SOURCE_ACTIVE) {
               w_fail(w, "%s is waited on with the 'active attribute", what);
               free(what);
               return false;
            }
         }

         unsigned nsrc = 0;
         if (nexus_first_source_is_inline(n)) {
            for (rt_source_t *src = &(n->sources); src; src = src->chain_input)
               nsrc++;
         }
         w_u32(w, nsrc);

         if (nsrc > 0) {
            for (rt_source_t *src = &(n->sources); src;
                 src = src->chain_input) {
               if (!save_source(ix, w, n, src, what)) {
                  free(what);
                  return false;
               }
            }
         }

         const bool ok = save_pending(ix, w, n->pending, what);
         free(what);
         if (!ok)
            return false;
      }

      w_rec_end(w, sigrec);
   }

   return true;
}

static bool save_triggers(index_t *ix, wbuf_t *w)
{
   w_tag(w, "TRIG");
   w_u32(w, ix->ntrigs);

   for (unsigned i = 0; i < ix->ntrigs; i++) {
      rt_trigger_t *t = ix->trigs[i];

      w_cname(w, ix->trignames[i]);
      w_u8(w, t->kind);
      w_u32(w, t->nargs);
      w_name(w, t->handle == JIT_HANDLE_INVALID
             ? empty_ident() : jit_get_name(ix->jit, t->handle));

      if (t->wakeable.pending) {
         w_fail(w, "trigger %s is queued: not at a quiescent point",
                ix->trignames[i]);
         return false;
      }

      if (!save_pending(ix, w, t->pending, ix->trignames[i]))
         return false;
   }

   return true;
}

static bool save_processes(index_t *ix, wbuf_t *w)
{
   w_tag(w, "PROC");
   w_u32(w, ix->nprocs);

   for (unsigned i = 0; i < ix->nprocs; i++) {
      rt_proc_t *p = ix->procs[i];

      w_name(w, p->name);
      const size_t prec = w_rec_begin(w);

      if (p->wakeable.pending) {
         w_fail(w, "process %s is queued to run: not at a quiescent point",
                istr(p->name));
         return false;
      }

      w_u8(w, p->wakeable.delayed);

      void *frame = *mptr_get(p->privdata);
      if (frame == NULL) {
         w_u8(w, 0);
         w_rec_end(w, prec);
         continue;
      }

      w_u8(w, 1);

      w_u32(w, *(uint32_t *)((uint8_t *)frame + 2 * sizeof(void *)));

      char *key = frame_key(FRAME_PROCESS, p->name);
      const frame_t *f = shash_get(ix->framename, key);
      free(key);
      if (f == NULL) {
         w_fail(w, "process %s has no frame layout", istr(p->name));
         return false;
      }

      // The procedures this process is suspended in, innermost last
      const frame_t *chain[64];
      unsigned depth = 0;
      for (; depth < ARRAY_LEN(chain); depth++) {
         ident_t cname = ident_sprintf("%s/%u", istr(p->name), depth);
         char *ckey = frame_key(FRAME_PROCEDURE, cname);
         chain[depth] = shash_get(ix->framename, ckey);
         free(ckey);
         if (chain[depth] == NULL)
            break;
      }

      if ((depth > 0) != (p->tlab != NULL)) {
         w_fail(w, "process %s has inconsistent procedure state",
                istr(p->name));
         return false;
      }

      w_u32(w, depth);
      for (unsigned k = 0; k < depth; k++)
         w_name(w, jit_get_name(ix->jit, chain[k]->handle));

      for (unsigned k = 0; k < depth; k++) {
         const uint8_t *fd = chain[k]->data;
         w_u32(w, *(uint32_t *)(fd + 2 * sizeof(void *)));

         vctx_t c = { .ix = ix, .w = w, .unit = "procedure frame",
                      .var = "context" };
         const jit_vtype_t ctxtype = { .kind = VT_CONTEXT,
                                       .size = sizeof(void *) };
         if (!save_value(&c, &ctxtype, fd))
            return false;
      }

      if (!save_frame_vars(ix, w, f))
         return false;

      for (unsigned k = 0; k < depth; k++) {
         if (!save_frame_vars(ix, w, chain[k]))
            return false;
      }

      w_rec_end(w, prec);
   }

   return true;
}

static bool save_frames(index_t *ix, wbuf_t *w)
{
   w_tag(w, "FRMS");

   unsigned count = 0;
   for (unsigned i = 0; i < ix->nframes; i++)
      count += (ix->frames[i].kind != FRAME_PROCESS
                && ix->frames[i].kind != FRAME_PROCEDURE);
   w_u32(w, count);

   for (unsigned i = 0; i < ix->nframes; i++) {
      const frame_t *f = &(ix->frames[i]);
      if (f->kind == FRAME_PROCESS || f->kind == FRAME_PROCEDURE)
         continue;

      w_u8(w, f->kind);
      w_name(w, f->name);
      const size_t frec = w_rec_begin(w);
      if (!save_frame_vars(ix, w, f))
         return false;
      w_rec_end(w, frec);
   }

   return true;
}

typedef struct {
   index_t *ix;
   wbuf_t  *w;
} heap_ctx_t;

static void save_event_cb(uint64_t key, void *user, void *arg)
{
   heap_ctx_t *hc = arg;
   wbuf_t *w = hc->w;
   if (w->err != NULL)
      return;

   w_u64(w, key);
   w_u8(w, pointer_tag(user));

   switch (pointer_tag(user)) {
   case EVENT_PROCESS:
      w_name(w, (untag_pointer(user, rt_proc_t))->name);
      break;

   case EVENT_DRIVER:
   case EVENT_PSEUDO:
      {
         rt_source_t *src = untag_pointer(user, rt_source_t);
         if (pointer_tag(user) == EVENT_PSEUDO) {
            w_fail(w, "a forced or deposited value is scheduled");
            return;
         }

         rt_nexus_t *n = src->u.driver.nexus;
         const sigent_t *e = NULL;
         for (unsigned i = 0; i < hc->ix->nsigs; i++) {
            if (hc->ix->sigs[i].sig == n->signal)
               e = &(hc->ix->sigs[i]);
         }

         unsigned idx = 0;
         rt_source_t *it = &(n->sources);
         for (; it && it != src; it = it->chain_input)
            idx++;

         if (e == NULL || it == NULL) {
            w_fail(w, "a driver update is scheduled for an unknown signal");
            return;
         }

         w_cname(w, e->path);
         w_u32(w, n->offset / n->size);
         w_u32(w, idx);
      }
      break;

   default:
      w_fail(w, "a timeout callback is scheduled (added by a debugger or "
             "plugin)");
      break;
   }
}

static bool deferq_empty(const deferq_t *dq)
{
   return dq->count == 0;
}

static void save_files(index_t *ix, wbuf_t *w)
{
   w_tag(w, "FILE");
   w_u32(w, ix->nfiles);

   bool user = false;
   for (unsigned i = 0; i < ix->nfiles; i++)
      user |= !ix->fst[i].predef;

   char cwd[4096] = "";
   if (user && getcwd(cwd, sizeof(cwd)) == NULL)
      cwd[0] = '\0';
   w_cname(w, cwd);

   for (unsigned i = 0; i < ix->nfiles; i++) {
      const file_ckpt_t *f = &(ix->fst[i]);
      w_u8(w, f->predef);
      w_u8(w, f->mode);
      w_cname(w, f->name);
      w_u64(w, f->pos);
      w_u64(w, f->size);
      w_u64(w, f->hash);
      if (f->data != NULL)
         w_bytes(w, f->data, f->size);
   }
}

static void save_objects(index_t *ix, wbuf_t *w)
{
   w_tag(w, "OBJS");
   w_u32(w, ix->nobjs);
   for (unsigned i = 0; i < ix->nobjs; i++) {
      const obj_t *o = &(ix->objs[i]);
      w_u32(w, o->esize);
      w_u32(w, o->fixup);
      w_u32(w, o->hi - o->lo);

      if (o->fixup > 0) {
         // The first word points into the object: not meaningful in a file
         w_u64(w, 0);
         w_bytes(w, o->base + sizeof(void *), o->hi - o->lo - sizeof(void *));
      }
      else
         w_bytes(w, o->base + o->lo, o->hi - o->lo);
   }
}

static char *check_quiescent(rt_model_t *m, jit_t *jit)
{
   const char *bad = NULL;

   if (m->next_is_delta)
      bad = "a delta cycle is pending";
   else if (!deferq_empty(&m->procq) || !deferq_empty(&m->next_procq))
      bad = "processes are queued to run";
   else if (!deferq_empty(&m->driverq) || !deferq_empty(&m->next_driverq))
      bad = "driver updates are queued";
   else if (!deferq_empty(&m->postponedq) || !deferq_empty(&m->triggerq))
      bad = "postponed processes or triggers are queued";
   else if (!deferq_empty(&m->inactiveq) || !deferq_empty(&m->next_inactiveq)
            || !deferq_empty(&m->nonblockq))
      bad = "inactive or non-blocking events are queued";
   else if (!deferq_empty(&m->reschedq))
      bad = "processes are queued for rescheduling";
   else if (heap_size(m->driving_heap) > 0 || heap_size(m->effective_heap) > 0)
      bad = "signal updates are queued";
   else if (m->force_stop)
      bad = "the simulation is stopping";
   else if (heap_size(m->eventq_heap) > 0
            && heap_min_key(m->eventq_heap) <= m->now)
      bad = "an event is due at the current time";

   int status;
   if (bad == NULL && jit_exit_status(jit, &status))
      bad = "the simulation has already failed";

   if (bad == NULL)
      return NULL;

   return xasprintf("simulation is not at a quiescent point: %s", bad);
}

static char *check_supported(rt_model_t *m, rt_scope_t *s)
{
   if (s->properties.count > 0)
      return xasprintf("scope %s contains PSL properties", istr(s->name));

   if (s->kind == SCOPE_INSTANCE && tree_kind(s->where) == T_BLOCK) {
      tree_t hier = tree_decl(s->where, 0);
      if (tree_kind(hier) == T_HIER && tree_subkind(hier) == T_VERILOG)
         return xasprintf("scope %s is Verilog", istr(s->name));
   }

   for (int i = 0; i < s->children.count; i++) {
      char *err = check_supported(m, s->children.items[i]);
      if (err != NULL)
         return err;
   }

   return NULL;
}

// Serialise the dynamic state of the model.  The same function is used to
// produce the file and to check a restore, so what is compared is exactly
// what is saved.
static char *serialise_model(index_t *ix, wbuf_t *w, bool check_quiet)
{
   rt_model_t *m = ix->m;

   if (ix->err != NULL)
      return xstrdup(ix->err);

   char *err = check_quiet ? check_quiescent(m, ix->jit) : NULL;
   if (err != NULL)
      return err;

   if (m->cover != NULL)
      return xstrdup("code coverage is enabled");
   else if (m->shuffle)
      return xstrdup("process shuffling is enabled");

   if ((err = check_supported(m, root_scope(m))))
      return err;

   w_tag(w, "META");
   w_u64(w, m->now);
   w_u32(w, m->iteration);
   w_u64(w, m->trigger_epoch);

   unsigned counts[4], mask;
   get_vhdl_assert_state(counts, &mask);
   for (int i = 0; i < 4; i++)
      w_u32(w, counts[i]);
   w_u32(w, mask);

   // Packages are initialised lazily so a restore must do the same ones
   w_tag(w, "PKGS");
   unsigned npkgs = 0;
   for (unsigned i = 0; i < ix->nframes; i++)
      npkgs += (ix->frames[i].kind == FRAME_PACKAGE);
   w_u32(w, npkgs);
   for (unsigned i = 0; i < ix->nframes; i++) {
      if (ix->frames[i].kind == FRAME_PACKAGE)
         w_name(w, ix->frames[i].name);
   }

   index_files(ix);
   if (ix->err != NULL)
      return xstrdup(ix->err);

   save_files(ix, w);

   // Heap arrays are found by a first pass over the frames as the same
   // object may be referred to from several places
   if (!ix->objs_done) {
      wbuf_t scratch;
      w_init(&scratch);
      ix->collect = true;
      const bool ok = save_processes(ix, &scratch) && save_frames(ix, &scratch);
      ix->collect = false;
      ix->objs_done = true;
      char *serr = ok ? NULL : xstrdup(scratch.err ?: "save failed");
      w_free(&scratch);
      if (serr != NULL)
         return serr;
   }

   save_objects(ix, w);

   if (!save_triggers(ix, w) || !save_signals(ix, w)
       || !save_processes(ix, w) || !save_frames(ix, w))
      return w->err ? xstrdup(w->err) : xstrdup("save failed");

   w_tag(w, "HEAP");
   w_u32(w, heap_size(m->eventq_heap));
   heap_ctx_t hc = { .ix = ix, .w = w };
   heap_walk(m->eventq_heap, save_event_cb, &hc);
   if (w->err != NULL)
      return xstrdup(w->err);

   return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// What code has run
//
// After the state comes a description of every compiled unit: a hash of
// each instruction (without source locations and with callees by name), the
// instructions that had run, and whether it always resumes at its start.
// A restore into changed code compares them to see what the change touches.

typedef struct {
   char     *name;
   unsigned  nirs;
   bool      restart;
   uint64_t  cphash;
   uint32_t *ihash;
   uint8_t  *bits;      // One bit per instruction, or NULL if not known
} oldunit_t;

typedef struct {
   bool       tracked;
   oldunit_t *units;
   unsigned   nunits;
} exec_info_t;

static void exec_free(exec_info_t *e)
{
   for (unsigned i = 0; i < e->nunits; i++) {
      free(e->units[i].name);
      free(e->units[i].ihash);
      free(e->units[i].bits);
   }
   free(e->units);
}

static void save_exec(jit_t *jit, wbuf_t *t)
{
   const unsigned n = jit_handle_count(jit);

   bool tracked = jit_exec_tracking;
   unsigned count = 0;
   jit_code_t *codes = xcalloc(MAX(n, 1) * sizeof(jit_code_t));
   bool *have = xcalloc(MAX(n, 1));

   for (jit_handle_t h = 0; h < n; h++) {
      if (!(have[h] = jit_get_code(jit, h, &codes[h])))
         continue;

      count++;
      if (codes[h].exec == NULL || codes[h].has_next_tier)
         tracked = false;
   }

   w_u8(t, tracked);
   w_u32(t, count);

   for (jit_handle_t h = 0; h < n; h++) {
      if (!have[h])
         continue;

      const jit_code_t *c = &codes[h];
      w_str(t, istr(jit_get_name(jit, h)));
      w_u32(t, c->nirs);
      w_u8(t, c->restart_top);
      w_u64(t, c->cpool_hash);
      for (unsigned i = 0; i < c->nirs; i++)
         w_u32(t, c->ihash[i]);
      if (tracked) {
         for (unsigned i = 0; i < c->nirs; i += 8) {
            uint8_t b = 0;
            for (unsigned k = 0; k < 8 && i + k < c->nirs; k++)
               b |= (c->exec[i + k] ? 1 : 0) << k;
            w_u8(t, b);
         }
      }

      free(c->ihash);
   }

   free(codes);
   free(have);
}

static bool parse_exec(const uint8_t *data, size_t len, exec_info_t *e)
{
   rbuf_t r = { .p = data, .end = data + len };

   e->tracked = r_u8(&r);
   const uint32_t count = r_u32(&r);
   if (r.err != NULL || count > len) {
      free(r.err);
      return false;
   }

   e->units = xcalloc(MAX(count, 1) * sizeof(oldunit_t));
   for (unsigned i = 0; i < count && r.err == NULL; i++) {
      oldunit_t *u = &(e->units[i]);
      const uint32_t nlen = r_u32(&r);
      const uint8_t *nm = r_bytes(&r, nlen);
      if (nm == NULL)
         break;

      u->name    = xstrndup((const char *)nm, nlen);
      u->nirs    = r_u32(&r);
      u->restart = r_u8(&r);
      u->cphash  = r_u64(&r);
      if (r.err != NULL || (size_t)u->nirs * 4 > (size_t)(r.end - r.p))
         break;

      u->ihash = xmalloc_array(MAX(u->nirs, 1), sizeof(uint32_t));
      for (unsigned k = 0; k < u->nirs; k++)
         u->ihash[k] = r_u32(&r);

      if (e->tracked) {
         const size_t nb = (u->nirs + 7) / 8;
         const uint8_t *b = r_bytes(&r, nb);
         if (b == NULL)
            break;
         u->bits = xmalloc(MAX(nb, 1));
         memcpy(u->bits, b, nb);
      }

      e->nunits++;
   }

   const bool ok = r.err == NULL && e->nunits == count;
   free(r.err);
   return ok;
}

static bool bit_set(const uint8_t *bits, unsigned i)
{
   return (bits[i / 8] >> (i % 8)) & 1;
}

// Compare the code of the checkpoint with the code of this build unit by
// unit, mark what has run in this build's copy, and classify each unit.
// A unit that changed is harmless only if nothing at or after the first
// instruction that differs had run: everything before it is the same code
// and so did the same things.
static void analyse_units(const exec_info_t *e, jit_t *jit, shash_t *units,
                          ckpt_report_t *rep)
{
   rep->tracked = e->tracked;

   for (unsigned i = 0; i < e->nunits; i++) {
      const oldunit_t *u = &(e->units[i]);

      unit_t *info = xcalloc(sizeof(unit_t));
      info->old_restart = u->restart;
      shash_put(units, u->name, info);

      jit_handle_t h = jit_lazy_compile(jit, ident_new(u->name));
      if (h != JIT_HANDLE_INVALID)
         jit_compile(jit, ident_new(u->name));

      jit_code_t c = {};
      if (h == JIT_HANDLE_INVALID || !jit_get_code(jit, h, &c)) {
         info->cls = U_REMOVED;

         bool ran = !e->tracked;
         for (unsigned k = 0; e->tracked && k < u->nirs; k++)
            ran |= bit_set(u->bits, k);

         if (ran || h != JIT_HANDLE_INVALID) {
            // Gone or no longer compiles: callers changed too
         }

         rep->changed = xrealloc_array(rep->changed, rep->nchanged + 1,
                                       sizeof(char *));
         rep->executed = xrealloc_array(rep->executed, rep->nchanged + 1,
                                        sizeof(bool));
         rep->changed[rep->nchanged] = xstrdup(u->name);
         rep->executed[rep->nchanged++] = ran;
         if (ran)
            info->cls = U_APPROX;
         continue;
      }

      info->new_restart = c.restart_top;
      info->new_state = c.restart_state;

      bool same = c.nirs == u->nirs && c.cpool_hash == u->cphash
         && memcmp(c.ihash, u->ihash, u->nirs * sizeof(uint32_t)) == 0;

      if (same) {
         info->cls = U_SAME;
         jit_set_exec(jit, h, u->bits, u->nirs, !e->tracked);
      }
      else {
         unsigned prefix = 0;
         const unsigned lim = MIN(c.nirs, u->nirs);
         while (prefix < lim && c.ihash[prefix] == u->ihash[prefix])
            prefix++;

         // Constants of the unit are shared by all its code so a change
         // to them counts as a change from the start
         if (c.cpool_hash != u->cphash)
            prefix = 0;

         if (getenv("NVC_CKPT_DEBUG"))
            fprintf(stderr, "unit %s changed: nirs %u->%u prefix %u cpool %s\n",
                    u->name, u->nirs, c.nirs, prefix,
                    c.cpool_hash != u->cphash ? "differs" : "same");

         bool ran = !e->tracked;
         for (unsigned k = prefix; e->tracked && k < u->nirs; k++)
            ran |= bit_set(u->bits, k);

         info->cls = ran ? U_APPROX : U_COMPAT;

         rep->changed = xrealloc_array(rep->changed, rep->nchanged + 1,
                                       sizeof(char *));
         rep->executed = xrealloc_array(rep->executed, rep->nchanged + 1,
                                        sizeof(bool));
         rep->changed[rep->nchanged] = xstrdup(u->name);
         rep->executed[rep->nchanged++] = ran;

         // Whatever ran in the old code says nothing about the new code
         jit_set_exec(jit, h, NULL, 0, true);
      }

      free(c.ihash);
   }
}

////////////////////////////////////////////////////////////////////////////////
// Restoring the model

static void free_pending(void **pending)
{
   if (*pending != NULL && pointer_tag(*pending) == 0)
      free(untag_pointer(*pending, rt_pending_t));
   *pending = NULL;
}

static bool restore_pending(index_t *ix, rbuf_t *r, void **pending,
                            const char *what)
{
   rt_model_t *m = ix->m;

   // Watches added by tools since the reset must survive
   rt_wakeable_t *keep[16];
   unsigned nkeep = 0;
   if (*pending != NULL) {
      if (pointer_tag(*pending) == 1) {
         rt_wakeable_t *w = untag_pointer(*pending, rt_wakeable_t);
         if (w->kind == W_WATCH)
            keep[nkeep++] = w;
      }
      else {
         rt_pending_t *p = untag_pointer(*pending, rt_pending_t);
         for (unsigned i = 0; i < p->count && nkeep < ARRAY_LEN(keep); i++) {
            if (p->wake[i] != NULL && p->wake[i]->kind == W_WATCH)
               keep[nkeep++] = p->wake[i];
         }
      }
   }

   free_pending(pending);

   const uint32_t count = r_u32(r);
   if (r->err != NULL)
      return false;

   if (count > 0 && count > (size_t)(r->end - r->p)) {
      r_fail(r, "checkpoint file is corrupt");
      return false;
   }

   rt_wakeable_t **objs = xmalloc_array(MAX(count, 1), sizeof(rt_wakeable_t *));
   for (unsigned i = 0; i < count; i++) {
      const uint8_t kind = r_u8(r);
      objs[i] = NULL;

      if (kind == 0 || r->err != NULL)
         continue;

      ident_t name = r_name(r);
      if (r->err != NULL)
         break;

      if (kind == 1) {
         rt_proc_t *p = shash_get(ix->procname, istr(name));
         if (p == NULL) {
            r_fail(r, "%s is waited on by process %s which does not exist",
                   what, istr(name));
            break;
         }
         objs[i] = &(p->wakeable);
      }
      else if (kind == 2) {
         rt_trigger_t *t = shash_get(ix->trigname, istr(name));
         if (t == NULL) {
            r_fail(r, "%s is waited on by trigger %s which does not exist",
                   what, istr(name));
            break;
         }
         objs[i] = &(t->wakeable);
      }
      else if (kind == 3) {
         rt_transfer_t *t = shash_get(ix->transname, name_str(name));
         if (t == NULL) {
            r_fail(r, "%s is waited on by a signal transfer of process %s "
                   "which does not exist", what, name_str(name));
            break;
         }
         objs[i] = &(t->wakeable);
      }
      else {
         r_fail(r, "bad wakeable kind in checkpoint file");
         break;
      }
   }

   if (r->err == NULL && count > 0) {
      // Rebuild the list with the same slots, trailing empty ones included
      unsigned n = count;
      if (n == 1 && objs[0] != NULL)
         *pending = tag_pointer(objs[0], 1);
      else {
         rt_pending_t *p = xmalloc_flex(sizeof(rt_pending_t),
                                        MAX(n, PENDING_MIN),
                                        sizeof(rt_wakeable_t *));
         p->max = MAX(n, PENDING_MIN);
         p->count = n;
         for (unsigned i = 0; i < n; i++)
            p->wake[i] = objs[i];
         *pending = tag_pointer(p, 0);
      }
   }

   free(objs);

   for (unsigned i = 0; i < nkeep && r->err == NULL; i++)
      model_sched_event(m, pending, keep[i]);

   return r->err == NULL;
}

static rt_nexus_t *nth_nexus(rt_signal_t *s, unsigned nth)
{
   rt_nexus_t *n = &(s->nexus);
   for (unsigned i = 0; i < nth && n != NULL; i++)
      n = n->chain;
   return n;
}

static rt_nexus_t *find_nexus(rt_signal_t *s, unsigned offset)
{
   for (rt_nexus_t *n = &(s->nexus); n != NULL; n = n->chain) {
      if (n->offset / n->size == offset)
         return n;
   }
   return NULL;
}

static rt_source_t *restore_source(index_t *ix, rbuf_t *r, rt_nexus_t *n,
                                   rt_source_t *src, const char *what)
{
   // A null source means one that did not exist after the reset, which can
   // only be a driver created by a process as it ran
   rt_model_t *m = ix->m;

   const uint8_t tag = r_u8(r);
   const uint8_t bits = r_u8(r);
   const size_t valsz = (size_t)n->width * n->size;

   if (r->err != NULL)
      return NULL;

   if (tag == SOURCE_DRIVER) {
      ident_t pname = r_name(r);
      if (r->err != NULL)
         return NULL;

      if (src == NULL) {
         rt_proc_t *p = shash_get(ix->procname, istr(pname));
         if (p == NULL) {
            r_fail(r, "%s is driven by process %s which does not exist",
                   what, istr(pname));
            return NULL;
         }

         src = model_add_source(m, n, SOURCE_DRIVER);
         src->u.driver.waveforms.value = model_alloc_value(m, n);
         src->u.driver.proc = p;
      }
      else if (src->tag != SOURCE_DRIVER
               || strcmp(src->u.driver.proc ? istr(src->u.driver.proc->name)
                         : "", name_str(pname)) != 0) {
         r_fail(r, "%s: driver of process %s in the checkpoint is not in "
                "the model", what, istr(pname));
         return NULL;
      }

      waveform_t *wf = &(src->u.driver.waveforms);
      wf->when = r_u64(r);
      const uint8_t *b = r_bytes(r, valsz);
      if (b == NULL)
         return NULL;
      memcpy(model_value_ptr(n, &wf->value), b, valsz);

      assert(wf->next == NULL);

      const uint32_t nfuture = r_u32(r);
      waveform_t *tail = wf;
      for (unsigned i = 0; i < nfuture && r->err == NULL; i++) {
         waveform_t *w = model_alloc_waveform(m);
         w->when = r_u64(r);
         w->next = NULL;
         w->value = model_alloc_value(m, n);
         b = r_bytes(r, valsz);
         if (b == NULL)
            return NULL;
         memcpy(model_value_ptr(n, &w->value), b, valsz);
         tail->next = w;
         tail = w;
      }
   }
   else if (tag == SOURCE_PORT) {
      ident_t path = r_name(r);
      const uint32_t off = r_u32(r);
      if (r->err != NULL)
         return NULL;

      if (src == NULL || src->tag != SOURCE_PORT) {
         r_fail(r, "%s: port connection in the checkpoint is not in the "
                "model", what);
         return NULL;
      }

      const sigent_t *e = shash_get(ix->sigpath, istr(path));
      rt_nexus_t *in = src->u.port.input;
      if (e == NULL || e->sig != in->signal || in->offset / in->size != off) {
         r_fail(r, "%s: port connection differs between checkpoint and "
                "model", what);
         return NULL;
      }
   }
   else {
      r_fail(r, "%s: unsupported source kind %d in checkpoint", what, tag);
      return NULL;
   }

   src->disconnected = bits & 1;
   src->was_active   = (bits >> 1) & 1;
   src->fastqueued   = 0;
   src->sigqueued    = 0;

   return r->err == NULL ? src : NULL;
}

static bool restore_layout(index_t *ix, rbuf_t *r)
{
   rt_model_t *m = ix->m;

   if (!r_tag(r, "LAYT"))
      return false;

   const uint32_t nsigs = r_u32(r);
   if (r->err == NULL && nsigs != ix->nsigs
       && !(ix->allow && nsigs < ix->nsigs)) {
      r_fail(r, "the checkpoint has %u signals but the model has %u",
             nsigs, ix->nsigs);
      return false;
   }

   // Signals may have been split into more nexuses since the reset so
   // apply the saved partition to the fresh signals.  Splitting one signal
   // can split others it is connected to so check the result afterwards.
   const uint8_t *mark = r->p;
   for (int pass = 0; pass < 2; pass++) {
      r->p = mark;

      for (unsigned i = 0; i < nsigs && r->err == NULL; i++) {
         ident_t path = r_name(r);
         const uint32_t size = r_u32(r);
         const uint32_t esize = r_u32(r);
         const uint32_t nn = r_u32(r);
         if (r->err != NULL)
            return false;

         const sigent_t *e = shash_get(ix->sigpath, istr(path));
         if (e == NULL) {
            r_fail(r, "signal %s is in the checkpoint but not in the model",
                   istr(path));
            return false;
         }

         rt_signal_t *s = e->sig;
         if (s->shared.size != size || s->nexus.size != esize) {
            r_fail(r, "signal %s has a different size in the model (%u/%u "
                   "in the checkpoint, %u/%u here)", istr(path), size, esize,
                   s->shared.size, s->nexus.size);
            return false;
         }

         for (unsigned k = 0; k < nn && r->err == NULL; k++) {
            const uint32_t off = r_u32(r);
            const uint32_t width = r_u32(r);
            if (r->err != NULL)
               return false;

            if (pass == 0) {
               if (off + width > size / esize) {
                  r_fail(r, "signal %s has a bad nexus in checkpoint",
                         istr(path));
                  return false;
               }
               model_split_nexus(m, s, off, width);
            }
            else {
               rt_nexus_t *n = nth_nexus(s, k);
               if (n == NULL || n->offset / n->size != off || n->width != width
                   || s->n_nexus != nn) {
                  r_fail(r, "signal %s is split differently in the model "
                         "(checkpoint nexus %u at %u+%u)", istr(path), k, off,
                         width);
                  return false;
               }
            }
         }
      }
   }

   return r->err == NULL;
}

static bool restore_signals(index_t *ix, rbuf_t *r)
{
   if (!r_tag(r, "SIGS"))
      return false;

   const uint32_t nsigs = r_u32(r);
   hash_t *seen = hash_new(64);

   for (unsigned i = 0; i < nsigs && r->err == NULL; i++) {
      ident_t path = r_name(r);
      const uint32_t reclen = r_u32(r);
      if (r->err != NULL || !r_ok(r, reclen))
         break;

      const uint8_t *recend = r->p + reclen;

      const sigent_t *e = shash_get(ix->sigpath, istr(path));
      assert(e != NULL);   // Checked by the layout pass
      rt_signal_t *s = e->sig;
      hash_put(seen, s, s);
      char *what = xasprintf("signal %s", istr(path));

      s->shared.flags = r_u32(r);
      if (r_u8(r) && r->err == NULL) {
         bool have = false;
         for (unsigned k = 0; k < ix->m->eventsigs.count; k++)
            have |= (ix->m->eventsigs.items[k] == s);
         if (!have)
            APUSH(ix->m->eventsigs, s);
      }
      const uint8_t *data = r_bytes(r, 3 * (size_t)s->shared.size);
      if (data != NULL)
         memcpy(s->shared.data, data, 3 * (size_t)s->shared.size);

      for (unsigned k = 0; k < s->n_nexus && r->err == NULL; k++) {
         rt_nexus_t *n = nth_nexus(s, k);

         const uint8_t flags = r_u8(r);
         n->active_delta = r_u32(r);
         n->event_delta  = r_u32(r);
         n->last_event   = r_u64(r);
         const uint8_t rank = r_u8(r);
         r_u8(r);   // n_sources, recomputed by add_source
         const uint32_t nsrc = r_u32(r);
         if (r->err != NULL)
            break;

         if (rank != n->rank) {
            r_fail(r, "%s: rank differs between checkpoint and model", what);
            break;
         }

         // Sources made at run time are appended after those from reset
         unsigned have = 0;
         if (n->n_sources > 0) {
            for (rt_source_t *it = &(n->sources); it; it = it->chain_input)
               have++;
         }

         if (have > nsrc) {
            r_fail(r, "%s has %u sources in the model but %u in the "
                   "checkpoint", what, have, nsrc);
            break;
         }

         rt_source_t *src = have > 0 ? &(n->sources) : NULL;
         for (unsigned j = 0; j < nsrc && r->err == NULL; j++) {
            rt_source_t *cur = j < have ? src : NULL;
            if (restore_source(ix, r, n, cur, what) == NULL)
               break;

            src = j < have ? src->chain_input : NULL;
         }

         if (r->err != NULL)
            break;

         n->flags = flags;

         if (!restore_pending(ix, r, &(n->pending), what))
            break;
      }

      if (r->err == NULL && r->p != recend)
         r_fail(r, "%s: the saved state has an unexpected size", what);

      free(what);
   }

   // Signals the checkpoint does not know about keep their initial values,
   // which is only right if nothing drives them yet
   for (unsigned i = 0; i < ix->nsigs && r->err == NULL; i++) {
      rt_signal_t *s = ix->sigs[i].sig;
      if (hash_get(seen, s) != NULL)
         continue;

      for (rt_nexus_t *n = &(s->nexus); n != NULL && n->signal == s;
           n = n->chain) {
         if (n->n_sources > 0 || n->pending != NULL) {
            r_fail(r, "signal %s is new and has drivers or readers: its "
                   "state cannot be derived from the checkpoint",
                   ix->sigs[i].path);
            break;
         }
      }

      ix->rep->layout_changed = true;
      report_note(ix->rep, "signal %s is new", ix->sigs[i].path);
   }

   hash_free(seen);
   return r->err == NULL;
}

static bool restore_file_table(index_t *ix, rbuf_t *r)
{
   if (!r_tag(r, "FILE"))
      return false;

   const uint32_t n = r_u32(r);
   ident_t cwd = r_name(r);
   if (r->err != NULL)
      return false;

   if (n > 0 && *name_str(cwd) != '\0') {
      char here[4096];
      if (getcwd(here, sizeof(here)) == NULL || strcmp(here, name_str(cwd)) != 0) {
         r_fail(r, "the design had files open, which are named relative to "
                "the directory %s: run from there", istr(cwd));
         return false;
      }
   }

   if (n > (size_t)(r->end - r->p)) {
      r_fail(r, "checkpoint file is corrupt");
      return false;
   }

   ix->sfiles = xcalloc(MAX(n, 1) * sizeof(file_ckpt_t));
   ix->sfh = xcalloc(MAX(n, 1) * sizeof(file_handle_t));
   ix->nsfiles = n;

   for (unsigned i = 0; i < n && r->err == NULL; i++) {
      file_ckpt_t *f = &(ix->sfiles[i]);
      f->predef = r_u8(r);
      f->mode   = r_u8(r);
      f->name   = xstrdup(istr(r_name(r)));
      f->pos    = r_u64(r);
      f->size   = r_u64(r);
      f->hash   = r_u64(r);
      if (!f->predef && f->mode != FILE_READ && r->err == NULL) {
         const uint8_t *b = r_bytes(r, f->size);
         if (b != NULL) {
            f->data = xmalloc(MAX(f->size, 1));
            memcpy(f->data, b, f->size);
         }
      }
   }

   return r->err == NULL;
}

static bool restore_objects(index_t *ix, rbuf_t *r)
{
   if (!r_tag(r, "OBJS"))
      return false;

   const uint32_t n = r_u32(r);
   if (r->err != NULL)
      return false;
   else if (n > (size_t)(r->end - r->p)) {
      r_fail(r, "checkpoint file is corrupt");
      return false;
   }

   mspace_t *ms = jit_get_mspace(ix->jit);

   ix->robjs  = xcalloc(MAX(n, 1) * sizeof(void *));
   ix->rroots = xcalloc(MAX(n, 1) * sizeof(mptr_t));
   ix->nrobjs = n;

   for (unsigned i = 0; i < n && r->err == NULL; i++) {
      r_u32(r);   // Element size, for checking only
      const uint32_t fixup = r_u32(r);
      const uint32_t len = r_u32(r);
      const uint8_t *b = r_bytes(r, len);
      if (b == NULL)
         break;

      uint8_t *mem = mspace_alloc(ms, len + OVERRUN_MARGIN);
      if (mem == NULL) {
         r_fail(r, "out of memory restoring a heap array of %u bytes", len);
         break;
      }

      memset(mem + len, '\0', OVERRUN_MARGIN);
      memcpy(mem, b, len);
      if (fixup > 0)
         *(void **)mem = mem + fixup;

      // Nothing else refers to it until the frames are restored
      ix->rroots[i] = mptr_new(ms, "checkpoint object");
      *mptr_get(ix->rroots[i]) = mem;
      ix->robjs[i] = mem;
   }

   return r->err == NULL;
}

static bool restore_triggers(index_t *ix, rbuf_t *r)
{
   if (!r_tag(r, "TRIG"))
      return false;

   const uint32_t n = r_u32(r);
   if (r->err == NULL && n != ix->ntrigs) {
      r_fail(r, "the checkpoint has %u triggers but the model has %u", n,
             ix->ntrigs);
      return false;
   }

   for (unsigned i = 0; i < n && r->err == NULL; i++) {
      ident_t name = r_name(r);
      const uint8_t kind = r_u8(r);
      const uint32_t nargs = r_u32(r);
      ident_t handle = r_name(r);
      if (r->err != NULL)
         break;

      rt_trigger_t *t = shash_get(ix->trigname, istr(name));
      if (t == NULL || t->kind != kind || t->nargs != nargs
          || strcmp(t->handle == JIT_HANDLE_INVALID ? ""
                    : istr(jit_get_name(ix->jit, t->handle)),
                    name_str(handle)) != 0) {
         r_fail(r, "trigger %s in the checkpoint does not match the model",
                istr(name));
         break;
      }

      t->wakeable.pending = false;
      if (!restore_pending(ix, r, &(t->pending), istr(name)))
         break;
   }

   return r->err == NULL;
}

static bool restore_processes(index_t *ix, rbuf_t *r)
{
   if (!r_tag(r, "PROC"))
      return false;

   const uint32_t n = r_u32(r);
   if (r->err == NULL && n != ix->nprocs) {
      r_fail(r, "the checkpoint has %u processes but the model has %u", n,
             ix->nprocs);
      return false;
   }

   for (unsigned i = 0; i < n && r->err == NULL; i++) {
      ident_t name = r_name(r);
      const uint32_t reclen = r_u32(r);
      if (r->err != NULL || !r_ok(r, reclen))
         break;
      const uint8_t *recend = r->p + reclen;
      const uint8_t delayed = r_u8(r);
      const uint8_t has_frame = r_u8(r);
      if (r->err != NULL)
         break;

      rt_proc_t *p = shash_get(ix->procname, istr(name));
      if (p == NULL) {
         r_fail(r, "process %s is in the checkpoint but not in the model",
                istr(name));
         break;
      }

      p->wakeable.delayed = delayed;
      p->wakeable.pending = false;

      void *frame = *mptr_get(p->privdata);
      if (!has_frame) {
         if (frame != NULL) {
            r_fail(r, "process %s has state in the model but none in the "
                   "checkpoint", istr(name));
            break;
         }
         r->p = recend;
         continue;
      }
      else if (frame == NULL) {
         r_fail(r, "process %s has state in the checkpoint but is "
                "stateless in the model", istr(name));
         break;
      }

      const uint32_t state = r_u32(r);

      // The code of a process that has changed and had already run cannot
      // be resumed from the middle: that is only possible if it always
      // starts again from the top
      const unit_t *pu = unit_lookup(ix, p->closure.handle);
      uint32_t resume = state;
      if (pu != NULL && pu->cls == U_APPROX
          && pu->old_restart && pu->new_restart && pu->new_state >= 0) {
         // It starts again from the top whatever it was waiting for
         resume = pu->new_state;
         ix->rep->layout_changed = true;   // So not byte for byte comparable
         report_note(ix->rep, "process %s resumes from its start", istr(name));
      }
      else if (pu != NULL && pu->cls == U_APPROX) {
         r_fail(r, "process %s has changed and is suspended inside code that "
                "has run (resume point %u), so it cannot be resumed; it "
                "could only be restored if it always waited at its end",
                istr(name), state);
         break;
      }

      *(uint32_t *)((uint8_t *)frame + 2 * sizeof(void *)) = resume;
      *(void **)((uint8_t *)frame + sizeof(void *)) = NULL;

      char *key = frame_key(FRAME_PROCESS, name);
      const frame_t *f = shash_get(ix->framename, key);
      free(key);
      if (f == NULL) {
         r_fail(r, "process %s has no frame layout", istr(name));
         break;
      }

      // Recreate the frames of the procedures the process is suspended in
      const uint32_t depth = r_u32(r);
      if (r->err != NULL || depth > 64) {
         r_fail(r, "bad procedure depth in checkpoint");
         break;
      }

      frame_t *chain[64];
      if (depth > 0) {
         if (p->tlab != NULL) {
            r_fail(r, "process %s is already suspended in a procedure",
                   istr(name));
            break;
         }

         p->tlab = tlab_acquire(ix->jit);
      }

      for (unsigned k = 0; k < depth && r->err == NULL; k++) {
         ident_t unit = r_name(r);
         if (r->err != NULL)
            break;

         jit_handle_t h = jit_lazy_compile(ix->jit, unit);
         const unit_t *cu = h == JIT_HANDLE_INVALID ? NULL
            : unit_lookup(ix, h);
         if (cu != NULL && cu->cls != U_SAME && cu->cls != U_COMPAT) {
            r_fail(r, "process %s is suspended in procedure %s which has "
                   "changed and had already run", istr(name), istr(unit));
            break;
         }

         frame_t *cf = xcalloc(sizeof(frame_t));
         if (h == JIT_HANDLE_INVALID
             || !jit_get_framelay(ix->jit, h, &cf->lay, true)
             || cf->lay.kind != MIR_UNIT_PROCEDURE) {
            r_fail(r, "procedure %s in the checkpoint does not exist",
                   istr(unit));
            free(cf);
            break;
         }

         cf->kind   = FRAME_PROCEDURE;
         cf->handle = h;
         cf->name   = ident_sprintf("%s/%u", istr(name), k);
         cf->size   = layout_size(&cf->lay);
         cf->proc   = p;
         cf->data   = tlab_alloc(p->tlab, cf->size);
         memset(cf->data, '\0', cf->size);
         *(int32_t *)((uint8_t *)cf->data + 2 * sizeof(void *) + 4) = h;

         chain[k] = cf;
         hash_put(ix->frameptr, cf->data, cf);
         shash_put(ix->framename, frame_key(FRAME_PROCEDURE, cf->name), cf);
         ix->xframes = xrealloc_array(ix->xframes, ix->nxframes + 1,
                                      sizeof(frame_t *));
         ix->xframes[ix->nxframes++] = cf;
      }

      if (r->err != NULL)
         break;

      for (unsigned k = 0; k < depth && r->err == NULL; k++) {
         uint8_t *fd = chain[k]->data;
         *(uint32_t *)(fd + 2 * sizeof(void *)) = r_u32(r);
         *(void **)(fd + sizeof(void *)) = k + 1 < depth ? chain[k + 1]->data
            : NULL;

         vctx_t c = { .ix = ix, .r = r, .unit = "procedure frame",
                      .var = "context" };
         const jit_vtype_t ctxtype = { .kind = VT_CONTEXT,
                                       .size = sizeof(void *) };
         if (!restore_value(&c, &ctxtype, fd))
            break;
      }

      if (r->err != NULL)
         break;

      if (depth > 0)
         *(void **)((uint8_t *)frame + sizeof(void *)) = chain[0]->data;

      if (!restore_frame_vars(ix, r, f))
         break;

      for (unsigned k = 0; k < depth && r->err == NULL; k++) {
         if (!restore_frame_vars(ix, r, chain[k]))
            break;
      }

      if (r->err == NULL && r->p != recend)
         r_fail(r, "process %s: the saved state has an unexpected size",
                istr(name));
   }

   return r->err == NULL;
}

static bool restore_frames(index_t *ix, rbuf_t *r)
{
   if (!r_tag(r, "FRMS"))
      return false;

   const uint32_t n = r_u32(r);
   if (r->err != NULL)
      return false;

   for (unsigned i = 0; i < n && r->err == NULL; i++) {
      const uint8_t kind = r_u8(r);
      ident_t name = r_name(r);
      const uint32_t reclen = r_u32(r);
      if (r->err != NULL || !r_ok(r, reclen))
         break;
      const uint8_t *recend = r->p + reclen;

      char *key = frame_key(kind, name);
      const frame_t *f = shash_get(ix->framename, key);
      free(key);
      if (f == NULL) {
         r_fail(r, "%s %s is in the checkpoint but not in the model",
                kind <= FRAME_PACKAGE ? frame_kind_name(kind) : "frame",
                istr(name));
         break;
      }

      if (!restore_frame_vars(ix, r, f))
         break;

      if (r->p != recend) {
         r_fail(r, "%s %s: the saved state has an unexpected size",
                frame_kind_name(f->kind), istr(name));
         break;
      }
   }

   return r->err == NULL;
}

static bool restore_heap(index_t *ix, rbuf_t *r)
{
   rt_model_t *m = ix->m;

   if (!r_tag(r, "HEAP"))
      return false;

   const uint32_t n = r_u32(r);
   if (r->err != NULL)
      return false;

   if (heap_size(m->eventq_heap) != 0) {
      r_fail(r, "the model has events scheduled before the restore");
      return false;
   }

   for (unsigned i = 0; i < n && r->err == NULL; i++) {
      const uint64_t key = r_u64(r);
      const uint8_t tag = r_u8(r);
      if (r->err != NULL)
         break;

      if (tag == EVENT_PROCESS) {
         ident_t name = r_name(r);
         rt_proc_t *p = r->err ? NULL : shash_get(ix->procname, istr(name));
         if (p == NULL) {
            r_fail(r, "a timeout is scheduled for process %s which does not "
                   "exist", istr(name));
            break;
         }
         heap_append_raw(m->eventq_heap, key, tag_pointer(p, EVENT_PROCESS));
      }
      else if (tag == EVENT_DRIVER) {
         ident_t path = r_name(r);
         const uint32_t off = r_u32(r);
         const uint32_t idx = r_u32(r);
         if (r->err != NULL)
            break;

         const sigent_t *e = shash_get(ix->sigpath, istr(path));
         rt_nexus_t *nx = e ? find_nexus(e->sig, off) : NULL;
         rt_source_t *src = nx && nx->n_sources > 0 ? &(nx->sources) : NULL;
         for (unsigned k = 0; k < idx && src; k++)
            src = src->chain_input;

         if (src == NULL) {
            r_fail(r, "a driver update is scheduled for %s+%u which does not "
                   "exist", istr(path), off);
            break;
         }

         heap_append_raw(m->eventq_heap, key, tag_pointer(src, EVENT_DRIVER));
      }
      else {
         r_fail(r, "unsupported event kind %d in checkpoint", tag);
         break;
      }
   }

   return r->err == NULL;
}

static void init_packages(index_t *ix, jit_t *jit, rbuf_t *r)
{
   if (!r_tag(r, "PKGS"))
      return;

   const uint32_t n = r_u32(r);
   for (unsigned i = 0; i < n && r->err == NULL; i++) {
      ident_t name = r_name(r);
      if (r->err != NULL)
         return;

      jit_handle_t h = jit_lazy_compile(jit, name);
      if (h == JIT_HANDLE_INVALID) {
         r_fail(r, "package %s in the checkpoint does not exist", istr(name));
         return;
      }

      jit_link(jit, h);
   }
}

typedef struct {
   rt_model_t *m;
   jit_t      *jit;
   rbuf_t     *r;
   char       *err;
   const uint8_t *payload;
   size_t      length;
   bool        allow;
   ckpt_report_t *rep;
   exec_info_t *exec;
} restore_arg_t;

static void restore_model_cb(void *arg)
{
   restore_arg_t *ra = arg;
   rt_model_t *m = ra->m;
   jit_t *jit = ra->jit;
   rbuf_t *r = ra->r;

   // The reset leaves every process queued to run its first iteration:
   // forget that, as the checkpoint says where each one is
   if (!deferq_empty(&m->driverq) || !deferq_empty(&m->next_driverq)
       || heap_size(m->driving_heap) > 0 || heap_size(m->effective_heap) > 0) {
      ra->err = xstrdup("the model has pending signal updates after reset");
      return;
   }

   if (!r_tag(r, "META"))
      goto fail;

   const uint64_t now = r_u64(r);
   const uint32_t iteration = r_u32(r);
   const uint64_t epoch = r_u64(r);
   unsigned counts[4], mask;
   for (int i = 0; i < 4; i++)
      counts[i] = r_u32(r);
   mask = r_u32(r);
   if (r->err != NULL)
      goto fail;

   shash_t *units = shash_new(256);
   analyse_units(ra->exec, jit, units, ra->rep);

   init_packages(NULL, jit, r);
   if (r->err != NULL)
      goto fail;

   index_t ix;
   index_build(&ix, m, jit);
   if (ix.err != NULL) {
      r_fail(r, "%s", ix.err);
      index_free(&ix);
      goto fail;
   }

   ix.allow = ra->allow;
   ix.rep   = ra->rep;
   ix.units = units;

   for (unsigned i = 0; i < ix.nprocs; i++) {
      ix.procs[i]->wakeable.pending = false;
      ix.procs[i]->wakeable.delayed = false;
   }

   hash_iter_t it = HASH_BEGIN;
   const char *tkey;
   void *tval;
   while (shash_iter(ix.transname, &it, &tkey, &tval))
      ((rt_transfer_t *)tval)->wakeable.pending = false;

   m->procq.count = 0;
   m->reschedq.count = 0;
   m->next_is_delta = false;

   const bool ok = restore_file_table(&ix, r) && restore_objects(&ix, r)
      && restore_triggers(&ix, r) && restore_layout(&ix, r)
      && restore_signals(&ix, r) && restore_processes(&ix, r)
      && restore_frames(&ix, r) && restore_heap(&ix, r);

   for (unsigned i = 0; i < ix.nrobjs; i++) {
      if (ix.rroots[i] != NULL)
         mptr_free(jit_get_mspace(jit), &(ix.rroots[i]));
   }

   index_free(&ix);
   {
      hash_iter_t uit = HASH_BEGIN;
      const char *ukey;
      void *uval;
      while (shash_iter(units, &uit, &ukey, &uval))
         free(uval);
      shash_free(units);
   }

   if (!ok)
      goto fail;

   m->now           = now;
   m->iteration     = iteration;
   m->trigger_epoch = epoch;
   set_vhdl_assert_state(counts, mask);
   return;

 fail:
   ra->err = xstrdup(r->err ? r->err : "restore failed");
}

////////////////////////////////////////////////////////////////////////////////
// Identity of the design and the program

struct _ckpt_id {
   char     version[64];
   uint64_t exe;
   uint64_t lib;
   uint64_t design;
};

static uint64_t fnv_update(uint64_t h, const void *data, size_t len)
{
   const uint8_t *p = data;
   for (size_t i = 0; i < len; i++)
      h = (h ^ p[i]) * 0x100000001b3ULL;
   return h;
}

static uint64_t fnv_file(const char *path, uint64_t h)
{
   FILE *f = fopen(path, "rb");
   if (f == NULL)
      return h ^ 0xdeadbeef;

   uint8_t buf[65536];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      h = fnv_update(h, buf, n);

   fclose(f);
   return h;
}

static int str_cmp_p(const void *a, const void *b)
{
   return strcmp(*(char *const *)a, *(char *const *)b);
}

static uint64_t hash_library(const char *dir)
{
   uint64_t h = 0xcbf29ce484222325ULL;

   DIR *d = opendir(dir);
   if (d == NULL)
      return h;

   char **names = NULL;
   unsigned n = 0;
   for (struct dirent *e; (e = readdir(d)); ) {
      if (e->d_name[0] == '.')
         continue;

      names = xrealloc_array(names, n + 1, sizeof(char *));
      names[n++] = xstrdup(e->d_name);
   }
   closedir(d);

   qsort(names, n, sizeof(char *), str_cmp_p);

   for (unsigned i = 0; i < n; i++) {
      char *path = xasprintf("%s/%s", dir, names[i]);
      struct stat st;
      if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
         h = fnv_update(h, names[i], strlen(names[i]));
         h = fnv_file(path, h);
      }
      free(path);
      free(names[i]);
   }
   free(names);

   return h;
}

static uint64_t hash_design(index_t *ix)
{
   uint64_t h = 0xcbf29ce484222325ULL;

   for (unsigned i = 0; i < ix->nsigs; i++) {
      rt_signal_t *s = ix->sigs[i].sig;
      h = fnv_update(h, ix->sigs[i].path, strlen(ix->sigs[i].path) + 1);
      h = fnv_update(h, &s->shared.size, sizeof(s->shared.size));
      h = fnv_update(h, &s->nexus.size, sizeof(s->nexus.size));
   }

   for (unsigned i = 0; i < ix->nprocs; i++) {
      const char *name = istr(ix->procs[i]->name);
      h = fnv_update(h, name, strlen(name) + 1);
   }

   for (unsigned i = 0; i < ix->nframes; i++) {
      const frame_t *f = &(ix->frames[i]);
      if (f->kind == FRAME_PACKAGE)
         continue;   // Initialised on demand so not part of the reset state

      const char *name = istr(f->name);
      const uint64_t lh = frame_layout_hash(f);
      h = fnv_update(h, name, strlen(name) + 1);
      h = fnv_update(h, &lh, sizeof(lh));
   }

   return h;
}

ckpt_id_t *checkpoint_identify(rt_model_t *m, jit_t *jit)
{
   ckpt_id_t *id = xcalloc(sizeof(ckpt_id_t));
   checked_sprintf(id->version, sizeof(id->version), "%s", PACKAGE_STRING);

   id->exe = fnv_file("/proc/self/exe", 0xcbf29ce484222325ULL);

   lib_t work = lib_work();
   id->lib = work ? hash_library(lib_path(work)) : 0;

   index_t ix;
   index_build(&ix, m, jit);
   id->design = hash_design(&ix);
   index_free(&ix);

   return id;
}

void checkpoint_free_id(ckpt_id_t *id)
{
   free(id);
}

static char *compare_ids(const ckpt_id_t *file, const ckpt_id_t *now,
                         bool allow)
{
   if (strcmp(file->version, now->version) != 0)
      return xasprintf("the checkpoint was written by %s but this is %s",
                       file->version, now->version);
   else if (file->exe != now->exe)
      return xstrdup("the checkpoint was written by a different build of nvc");
   else if (file->lib != now->lib && !allow)
      return xstrdup("the work library has changed since the checkpoint "
                     "was written: pass --allow-changes to restore into the "
                     "changed design, which reports whether the result is "
                     "exact or approximate");
   else if (file->design != now->design && file->lib == now->lib)
      return xstrdup("the elaborated design is not the one the checkpoint "
                     "was written from");
   else if (file->design != now->design && !allow)
      return xstrdup("the elaborated design is not the one the checkpoint "
                     "was written from: pass --allow-changes to restore "
                     "into a changed design");
   return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// Files

static const char *section_before(const uint8_t *base, size_t off)
{
   static const char *tags[] = {
      "META", "PKGS", "FILE", "OBJS", "TRIG", "LAYT", "SIGS", "PROC", "FRMS",
      "HEAP"
   };

   const char *found = "header";
   size_t best = 0;
   for (size_t i = 0; i + 4 <= off; i++) {
      for (int t = 0; t < ARRAY_LEN(tags); t++) {
         if (memcmp(base + i, tags[t], 4) == 0 && i >= best) {
            found = tags[t];
            best = i;
         }
      }
   }
   return found;
}

char *checkpoint_save(rt_model_t *m, jit_t *jit, const ckpt_id_t *id,
                      const char *path)
{
   index_t ix;
   index_build(&ix, m, jit);

   wbuf_t w;
   w_init(&w);

   char *err = serialise_model(&ix, &w, true);
   index_free(&ix);

   if (err != NULL) {
      w_free(&w);
      return err;
   }

   size_t blen;
   uint8_t *body = w_finish(&w, &blen);

   wbuf_t xt;
   memset(&xt, '\0', sizeof(xt));
   save_exec(jit, &xt);

   char *tmp = xasprintf("%s.tmp", path);
   FILE *f = fopen(tmp, "wb");
   if (f == NULL) {
      err = xasprintf("cannot create %s: %s", path, strerror(errno));
      goto out;
   }

   const uint32_t version = CKPT_VERSION;
   const uint64_t length = blen;
   const uint64_t hash = fnv_update(0xcbf29ce484222325ULL, body, blen);
   const uint64_t xlen = xt.n;
   const uint64_t xhash = fnv_update(0xcbf29ce484222325ULL, xt.d, xt.n);
   const uint32_t vlen = strlen(id->version);

   bool ok = fwrite(CKPT_MAGIC, 8, 1, f) == 1
      && fwrite(&version, 4, 1, f) == 1
      && fwrite(&vlen, 4, 1, f) == 1
      && fwrite(id->version, vlen, 1, f) == 1
      && fwrite(&id->exe, 8, 1, f) == 1
      && fwrite(&id->lib, 8, 1, f) == 1
      && fwrite(&id->design, 8, 1, f) == 1
      && fwrite(&length, 8, 1, f) == 1
      && fwrite(&hash, 8, 1, f) == 1
      && fwrite(body, blen, 1, f) == 1
      && fwrite(&xlen, 8, 1, f) == 1
      && fwrite(&xhash, 8, 1, f) == 1
      && fwrite(xt.d, xt.n, 1, f) == 1;

   ok = (fclose(f) == 0) && ok;

   if (!ok || rename(tmp, path) != 0) {
      err = xasprintf("cannot write %s: %s", path, strerror(errno));
      unlink(tmp);
   }

 out:
   free(tmp);
   free(body);
   free(xt.d);
   w_free(&w);
   return err;
}

char *checkpoint_restore(rt_model_t *m, jit_t *jit, const char *path,
                         bool allow, ckpt_report_t *rep)
{
   ckpt_report_t local = {};
   if (rep == NULL)
      rep = &local;

   FILE *f = fopen(path, "rb");
   if (f == NULL)
      return xasprintf("cannot open %s: %s", path, strerror(errno));

   fseek(f, 0, SEEK_END);
   const long fsize = ftell(f);
   fseek(f, 0, SEEK_SET);

   uint8_t *data = xmalloc(MAX(fsize, 1));
   const bool rd = fread(data, 1, fsize, f) == (size_t)fsize;
   fclose(f);

   char *err = NULL;
   if (!rd || fsize < 20 || memcmp(data, CKPT_MAGIC, 8) != 0) {
      free(data);
      return xasprintf("%s is not a checkpoint file", path);
   }

   rbuf_t r = { .p = data + 8, .end = data + fsize };

   const uint32_t version = r_u32(&r);
   if (r.err == NULL && version != CKPT_VERSION)
      r_fail(&r, "checkpoint format version %u is not supported", version);

   ckpt_id_t fid = {};
   const uint32_t vlen = r_u32(&r);
   const uint8_t *vstr = r_bytes(&r, vlen);
   if (vstr != NULL && vlen < sizeof(fid.version))
      memcpy(fid.version, vstr, vlen);
   fid.exe = r_u64(&r);
   fid.lib = r_u64(&r);
   fid.design = r_u64(&r);
   const uint64_t length = r_u64(&r);
   const uint64_t hash = r_u64(&r);

   if (r.err == NULL && length > (uint64_t)(r.end - r.p))
      r_fail(&r, "checkpoint file is truncated");

   exec_info_t exec = {};
   const uint8_t *payload = r.p;

   if (r.err == NULL) {
      if (fnv_update(0xcbf29ce484222325ULL, payload, length) != hash)
         r_fail(&r, "checkpoint file is corrupt (checksum mismatch)");

      rbuf_t t = { .p = payload + length, .end = r.end };
      const uint64_t xlen = r_u64(&t);
      const uint64_t xhash = r_u64(&t);
      const uint8_t *xb = r_bytes(&t, xlen);
      if (t.err != NULL || xb == NULL
          || fnv_update(0xcbf29ce484222325ULL, xb, xlen) != xhash
          || !parse_exec(xb, xlen, &exec))
         r_fail(&r, "checkpoint file is corrupt (code description)");
      free(t.err);
   }

   if (r.err != NULL) {
      err = xasprintf("%s: %s", path, r.err);
      free(r.err);
      free(data);
      return err;
   }

   ckpt_id_t *now = checkpoint_identify(m, jit);
   err = compare_ids(&fid, now, allow);
   const bool design_same = fid.design == now->design;
   free(now);
   if (err != NULL) {
      free(data);
      exec_free(&exec);
      return err;
   }

   rep->changed_design = !design_same;

   r.end = payload + length;
   if (!r_names_load(&r)) {
      err = xasprintf("%s: %s", path, r.err);
      goto out;
   }

   restore_arg_t ra = {
      .m = m, .jit = jit, .r = &r, .payload = payload, .length = length,
      .allow = allow, .rep = rep, .exec = &exec,
   };
   call_with_model(m, restore_model_cb, &ra);

   if (ra.err != NULL) {
      err = xasprintf("%s: %s", path, ra.err);
      free(ra.err);
      goto out;
   }

   // The verdict: exact only if every unit that changed had not run the
   // part that changed, nothing about the layout of the state differs and
   // the design is the same apart from that code
   unsigned nran = 0;
   for (unsigned i = 0; i < rep->nchanged; i++) {
      if (rep->executed[i]) {
         nran++;
         report_note(rep, "unit %s changed and had already run",
                     rep->changed[i]);
      }
   }

   if (rep->nchanged > 0 && !exec.tracked)
      report_note(rep, "the checkpoint does not record which code had run");
   if (!design_same)
      report_note(rep, "the elaborated design is not the same");

   rep->exact = design_same && !rep->layout_changed && nran == 0
      && (rep->nchanged == 0 || exec.tracked);

   // Serialise the restored model again: it must come out identical to
   // the file, otherwise something was not restored faithfully.  When
   // variables or signals were added or dropped the two are expected to
   // differ and only what was matched has been checked.
   if (rep->layout_changed)
      report_note(rep, "state of added or dropped variables or signals "
                  "is not covered by the verification of the restore");
   else {
      index_t ix;
      index_build(&ix, m, jit);
      wbuf_t w;
      w_init(&w);
      err = serialise_model(&ix, &w, false);
      index_free(&ix);

      size_t wlen = 0;
      uint8_t *wbody = err ? NULL : w_finish(&w, &wlen);

      if (err != NULL) {
         char *e2 = xasprintf("restored state cannot be verified: %s", err);
         free(err);
         err = e2;
      }
      else if (wlen != length || memcmp(wbody, payload, length) != 0) {
         size_t off = 0;
         while (off < wlen && off < length && wbody[off] == payload[off])
            off++;
         err = xasprintf("restore verification failed: the restored state "
                         "differs from the checkpoint at byte %zu in section "
                         "%s (saved %zu bytes, restored %zu)", off,
                         section_before(payload, off), (size_t)length, wlen);
      }

      free(wbody);
      w_free(&w);
   }

   rep->tracked = exec.tracked;

 out:
   free(r.names);
   free(r.err);
   free(data);
   exec_free(&exec);
   return err;
}

////////////////////////////////////////////////////////////////////////////////
// Command line support

typedef struct {
   jit_t     *jit;
   ckpt_id_t *id;
   char      *path;
   uint64_t   when;
   bool       done;
   bool       failed;
} sched_t;

static sched_t *the_sched;

void checkpoint_format_time(uint64_t t, char *buf, size_t len)
{
   static const struct { uint64_t div; const char *unit; } units[] = {
      { UINT64_C(1000000000000), "ms" }, { UINT64_C(1000000000), "us" },
      { UINT64_C(1000000), "ns" }, { UINT64_C(1000), "ps" }, { 1, "fs" },
   };

   int u = 0;
   while (u < 4 && t % units[u].div != 0)
      u++;

   checked_sprintf(buf, len, "%"PRIu64"%s", t / units[u].div, units[u].unit);
}

static void schedule_cb(rt_model_t *m, void *arg)
{
   sched_t *s = arg;

   // Phase callbacks run once so ask to be called at the next time step
   const uint64_t now = model_now(m, NULL);
   if (now < s->when) {
      model_set_phase_cb(m, END_TIME_STEP, schedule_cb, s);
      return;
   }
   else if (s->done)
      return;

   s->done = true;

   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   char *err = checkpoint_save(m, s->jit, s->id, s->path);

   clock_gettime(CLOCK_MONOTONIC, &t1);

   char tm[32];
   checkpoint_format_time(now, tm, sizeof(tm));

   if (err != NULL) {
      errorf("cannot write checkpoint %s at %s: %s", s->path, tm, err);
      free(err);
      s->failed = true;
      model_stop(m);
   }
   else {
      struct stat st;
      stat(s->path, &st);
      const double ms = (t1.tv_sec - t0.tv_sec) * 1e3
         + (t1.tv_nsec - t0.tv_nsec) / 1e6;
      notef("checkpoint %s written at %s (%lld bytes, %.1f ms)", s->path, tm,
            (long long)st.st_size, ms);
   }
}

void checkpoint_schedule(rt_model_t *m, jit_t *jit, const char *path,
                         uint64_t when)
{
   sched_t *s = xcalloc(sizeof(sched_t));
   s->jit  = jit;
   s->id   = checkpoint_identify(m, jit);
   s->path = xstrdup(path);
   s->when = when;

   the_sched = s;

   model_set_phase_cb(m, END_TIME_STEP, schedule_cb, s);
}

bool checkpoint_finish(rt_model_t *m)
{
   if (the_sched == NULL)
      return false;

   if (!the_sched->done && !the_sched->failed) {
      char tm[32], tm2[32];
      checkpoint_format_time(model_now(m, NULL), tm, sizeof(tm));
      checkpoint_format_time(the_sched->when, tm2, sizeof(tm2));
      errorf("simulation finished at %s before the checkpoint time %s: no "
             "checkpoint was written", tm, tm2);
      the_sched->failed = true;
   }

   return the_sched->failed;
}

////////////////////////////////////////////////////////////////////////////////
// Inventory

typedef struct {
   FILE      *out;
   mspace_t  *mspace;
   index_t   *ix;
} inv_ctx_t;

// Look at the value held by a variable to see where its pointers point
static const char *vt_runtime(const jit_vtype_t *vt, const uint8_t *p,
                              const frame_t *f, mspace_t *ms, index_t *ix)
{
   switch (vt->kind) {
   case VT_CARRAY:
      if (vt_class(vt->elem) == CLS_PLAIN || vt_class(vt->elem) == CLS_REF)
         return "ok";
      for (unsigned i = 0; i < vt->count; i++) {
         const char *r = vt_runtime(vt->elem, p + i * vt->elem->size, f, ms, ix);
         if (strcmp(r, "ok"))
            return r;
      }
      return "ok";

   case VT_RECORD:
      for (unsigned i = 0; i < vt->count; i++) {
         const char *r = vt_runtime(vt->fields[i].type,
                                    p + vt->fields[i].offset, f, ms, ix);
         if (strcmp(r, "ok"))
            return r;
      }
      return "ok";

   case VT_UARRAY:
   case VT_POINTER:
   case VT_ACCESS:
      {
         void *ptr = *(void **)p;
         if (vt->kind == VT_UARRAY && vt->elem->kind == VT_SIGNAL)
            return "ok";   // Signal reference
         else if (ptr == NULL)
            return "ok";
         else if (vt->kind == VT_ACCESS) {
            const jit_vtype_t *to = vt->elem;
            if (to != NULL && to->kind == VT_POINTER && to->elem != NULL)
               to = to->elem;
            if (to != NULL && heap_base(ix, ptr) == ptr
                && ((to->kind == VT_UARRAY && to->elem->kind != VT_SIGNAL
                     && vt_class(to->elem) == CLS_PLAIN)
                    || (to->kind != VT_UARRAY && vt_class(to) == CLS_PLAIN)))
               return "ok";   // Saved as a copy
            return "access value";
         }
         else if (find_range(ix, ptr) != NULL)
            return "ok";

         size_t sz;
         if (mspace_find(ms, ptr, &sz) != NULL) {
            if (vt->kind == VT_UARRAY && vt_class(vt->elem) == CLS_PLAIN)
               return "ok";   // Saved as a copy
            return vt->kind == VT_UARRAY ? "uarray data on heap"
               : "pointer to heap";
         }
         return vt->kind == VT_UARRAY ? "uarray data unidentified"
            : "pointer unidentified";
      }

   case VT_FILE:
      return "ok";   // Saved by name and position

   case VT_RESOLUTION:
      return "ok";

   default:
      return "ok";
   }
}

static void vt_str(const jit_vtype_t *vt, text_buf_t *tb)
{
   static const char *names[] = {
      "int", "offset", "real", "vec2", "vec4", "carray", "record", "uarray",
      "pointer", "access", "signal", "context", "file", "trigger",
      "resolution"
   };

   tb_cat(tb, names[vt->kind]);
   switch (vt->kind) {
   case VT_INT: case VT_OFFSET:
      tb_printf(tb, "%u", vt->size * 8);
      break;
   case VT_CARRAY:
      tb_printf(tb, "[%u]<", vt->count);
      vt_str(vt->elem, tb);
      tb_append(tb, '>');
      break;
   case VT_UARRAY: case VT_POINTER: case VT_ACCESS:
      tb_append(tb, '<');
      if (vt->elem != NULL)
         vt_str(vt->elem, tb);
      else
         tb_cat(tb, "opaque");
      tb_append(tb, '>');
      break;
   case VT_RECORD:
      tb_printf(tb, "{%u}", vt->count);
      break;
   default:
      break;
   }
}

static void inventory_frame(const frame_t *f, void *arg)
{
   inv_ctx_t *ctx = arg;
   static const char *kinds[] = { "scope", "process", "package" };

   fprintf(ctx->out, "FRAME\t%s\t%s\t%zu\n", kinds[f->kind], istr(f->name),
           f->size);

   LOCAL_TEXT_BUF tb = tb_new();
   for (unsigned i = 0; i < f->lay.nvars; i++) {
      const jit_var_t *v = &(f->lay.vars[i]);
      if (v->name == NULL)
         continue;

      tb_rewind(tb);
      vt_str(v->type, tb);

      const vt_class_t cls = vt_class(v->type);
      const char *run = vt_runtime(v->type, (uint8_t *)f->data + v->offset,
                                   f, ctx->mspace, ctx->ix);

      fprintf(ctx->out, "VAR\t%s\t%s\t%s\t%s\t%s\t%s\n",
              f->kind == FRAME_SCOPE ? "scope"
              : f->kind == FRAME_PROCESS ? "process" : "package",
              istr(f->name), istr(v->name), tb_get(tb), class_names[cls], run);
   }
}

typedef struct {
   unsigned nprops, nprocs, nstateless, ntlab, npcall, nscopes, nverilog;
} feat_t;

static void inventory_features(rt_scope_t *s, feat_t *ft)
{
   ft->nscopes++;
   ft->nprops += s->properties.count;
   if (s->kind == SCOPE_INSTANCE && tree_kind(s->where) == T_BLOCK) {
      tree_t hier = tree_decl(s->where, 0);
      if (tree_kind(hier) == T_HIER && tree_subkind(hier) == T_VERILOG)
         ft->nverilog++;
   }

   for (int i = 0; i < s->procs.count; i++) {
      rt_proc_t *p = s->procs.items[i];
      ft->nprocs++;
      if (*mptr_get(p->privdata) == NULL)
         ft->nstateless++;
      else if (*(void **)((uint8_t *)*mptr_get(p->privdata) + 8) != NULL)
         ft->npcall++;
      if (p->tlab != NULL)
         ft->ntlab++;
   }

   for (int i = 0; i < s->children.count; i++)
      inventory_features(s->children.items[i], ft);
}

void checkpoint_inventory(rt_model_t *m, jit_t *jit, FILE *out)
{
   index_t ix;
   index_build(&ix, m, jit);
   inv_ctx_t ctx = { .out = out, .mspace = jit_get_mspace(jit), .ix = &ix };

   fprintf(out, "BEGIN\t%s\n", istr(tree_ident(m->top)));

   feat_t ft = {};
   inventory_features(m->root, &ft);

   unsigned nsig = 0, nnexus = 0, npseudo = 0, nactive = 0, ntransfer = 0;
   unsigned nwatch = 0, nport = 0, ndriver = 0, ntrig = 0, nmulti = 0;
   for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
      nnexus++;
      if (n == &(n->signal->nexus))
         nsig++;
      if (n->n_sources > 1)
         nmulti++;

      for (rt_source_t *src = &(n->sources); n->n_sources > 0 && src;
           src = src->chain_input) {
         switch (src->tag) {
         case SOURCE_FORCING: case SOURCE_DEPOSIT: npseudo++; break;
         case SOURCE_PORT: nport++; break;
         case SOURCE_DRIVER: ndriver++; break;
         default: break;
         }
      }

      for (rt_source_t *o = n->outputs; o; o = o->chain_output) {
         if (o->tag == SOURCE_ACTIVE)
            nactive++;
      }

      void *pend = n->pending;
      if (pend == NULL)
         continue;
      else if (pointer_tag(pend) == 1) {
         rt_wakeable_t *w = untag_pointer(pend, rt_wakeable_t);
         ntransfer += (w->kind == W_TRANSFER);
         nwatch += (w->kind == W_WATCH);
      }
      else {
         rt_pending_t *pl = untag_pointer(pend, rt_pending_t);
         for (unsigned i = 0; i < pl->count; i++) {
            if (pl->wake[i] == NULL)
               continue;
            ntransfer += (pl->wake[i]->kind == W_TRANSFER);
            nwatch += (pl->wake[i]->kind == W_WATCH);
         }
      }
   }

   for (int i = 0; i < TRIGGER_TAB_SIZE; i++) {
      for (rt_trigger_t *t = m->triggertab[i]; t; t = t->chain)
         ntrig++;
   }

#define FEAT(name, v) fprintf(out, "FEAT\t%s\t%u\n", name, (unsigned)(v))
   FEAT("scopes", ft.nscopes);
   FEAT("verilog_scopes", ft.nverilog);
   FEAT("processes", ft.nprocs);
   FEAT("stateless_processes", ft.nstateless);
   FEAT("processes_suspended_in_procedure", ft.npcall);
   FEAT("processes_with_private_tlab", ft.ntlab);
   FEAT("psl_properties", ft.nprops);
   FEAT("signals", nsig);
   FEAT("nexuses", nnexus);
   FEAT("drivers", ndriver);
   FEAT("port_sources", nport);
   FEAT("multi_source_nexuses", nmulti);
   FEAT("pseudo_sources_force_deposit", npseudo);
   FEAT("active_outputs", nactive);
   FEAT("signal_transfers", ntransfer);
   FEAT("watches_registered", nwatch);
   FEAT("triggers", ntrig);
   FEAT("coverage", m->cover != NULL);
   FEAT("shuffle", m->shuffle);
#undef FEAT

   each_frame(m, jit, inventory_frame, &ctx);
   index_free(&ix);
}
