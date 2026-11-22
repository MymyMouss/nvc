//
//  Copyright (C) 2022-2024  Nick Gasson
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

#ifndef _JIT_H
#define _JIT_H

#include "prim.h"
#include "diag.h"
#include "util.h"

typedef int32_t jit_handle_t;
#define JIT_HANDLE_INVALID 0xffffffff

#define TLAB_SIZE (64 * 1024)

// The code generator knows the layout of this struct
typedef struct _tlab {
   mspace_t *mspace;
   uint32_t  alloc;
   uint32_t  limit;
   char      data[0];
} tlab_t;

typedef union {
   int64_t  integer;
   void    *pointer;
   double   real;
} jit_scalar_t;

STATIC_ASSERT(sizeof(jit_scalar_t) == 8);

typedef struct {
   void *(*init)(jit_t *);
   void (*cgen)(jit_t *, jit_handle_t, void *);
   void (*cleanup)(void *);
} jit_plugin_t;

typedef struct {
   loc_t     loc;
   object_t *object;
   ident_t   symbol;
} jit_frame_t;

typedef struct {
   size_t      count;
   jit_frame_t frames[0];
} jit_stack_trace_t;

typedef void (*jit_irq_fn_t)(jit_t *, void *);

// Typed description of the variables in a unit's persistent frame: this
// is what lets a checkpoint save and restore a frame by variable name
// instead of copying raw memory which may contain pointers
typedef enum {
   VT_INT, VT_OFFSET, VT_REAL, VT_VEC2, VT_VEC4, VT_CARRAY, VT_RECORD,
   VT_UARRAY, VT_POINTER, VT_ACCESS, VT_SIGNAL, VT_CONTEXT, VT_FILE,
   VT_TRIGGER, VT_RESOLUTION,
} jit_vt_kind_t;

typedef struct _jit_vtype jit_vtype_t;

typedef struct {
   uint32_t           offset;
   const jit_vtype_t *type;
} jit_vfield_t;

struct _jit_vtype {
   jit_vt_kind_t       kind;
   uint32_t            size;     // Bytes occupied inside the frame
   uint32_t            align;
   uint32_t            count;    // Array length, field count or dimensions
   const jit_vtype_t  *elem;     // Element or pointed-to type
   jit_vfield_t       *fields;   // Record only
};

typedef struct {
   ident_t            name;      // NULL for temporaries (not in frame)
   ptrdiff_t          offset;
   const jit_vtype_t *type;
} jit_var_t;

typedef struct {
   unsigned         nvars;
   const jit_var_t *vars;
   unsigned         kind;       // mir_unit_kind_t
   unsigned         hdrsz;      // Bytes before the first variable
} jit_framelay_t;

// Returns false if the unit has no persistent frame
bool jit_get_framelay(jit_t *j, jit_handle_t handle, jit_framelay_t *out,
                      bool compile);
bool jit_get_cpool(jit_t *j, jit_handle_t handle, const void **base,
                   size_t *size, bool compile);
unsigned jit_handle_count(jit_t *j);

// Which instructions of each function have run, for checkpoints.  Set the
// flag before any function is compiled.
extern bool jit_exec_tracking;

#define JIT_NO_LABEL UINT32_MAX

typedef enum {
   JIT_FLOW_NEXT,      // Continues with the next instruction
   JIT_FLOW_JUMP,      // Always goes to its label
   JIT_FLOW_BRANCH,    // Goes to its label or to the next instruction
   JIT_FLOW_END,       // Does not continue
} jit_flow_t;

typedef struct {
   unsigned       nirs;
   uint64_t      *hash;       // Of each instruction without its branch target
   uint32_t      *label;      // Branch target of each, or JIT_NO_LABEL
   uint8_t       *kind;       // JIT_FLOW_* of each: free all three with free()
   bool           unsupported;   // Control flow that is not understood
   const uint8_t *exec;       // NULL if not tracked
   uint64_t       cpool_hash;
   size_t         cpool_size;
   bool           restart_top;
   int            restart_state;
   const uint32_t *resume;    // States a suspended unit can resume at
   unsigned       nresume;
   bool           has_next_tier;
} jit_code_t;

bool jit_get_code(jit_t *j, jit_handle_t handle, jit_code_t *out);
void jit_set_exec(jit_t *j, jit_handle_t handle, const uint8_t *bits,
                  unsigned n, bool all);
void *jit_get_privdata(jit_t *j, jit_handle_t handle);
void jit_set_privdata(jit_t *j, jit_handle_t handle, void *ptr);

jit_t *jit_new(unit_registry_t *ur, mir_context_t *mc);
void jit_free(jit_t *j);
jit_handle_t jit_compile(jit_t *j, ident_t name);
jit_handle_t jit_lazy_compile(jit_t *j, ident_t name);
jit_handle_t jit_assemble(jit_t *j, ident_t name, const char *text);
void *jit_link(jit_t *j, jit_handle_t handle);
void *jit_get_frame_var(jit_t *j, jit_handle_t handle, void *p, ident_t name);
void jit_set_silent(jit_t *j, bool silent);
mspace_t *jit_get_mspace(jit_t *j);
bool jit_exit_status(jit_t *j, int *status);
void jit_reset_exit_status(jit_t *j);
void jit_add_tier(jit_t *j, int threshold, const jit_plugin_t *plugin);
ident_t jit_get_name(jit_t *j, jit_handle_t handle);
object_t *jit_get_object(jit_t *j, jit_handle_t handle);
void jit_register_native_plugin(jit_t *j);
void jit_register_llvm_plugin(jit_t *j);
void jit_interrupt(jit_t *j, jit_irq_fn_t fn, void *ctx);
void jit_check_interrupt(jit_t *j);
void jit_reset(jit_t *j);
bool jit_is_shutdown(jit_t *j);

tlab_t *tlab_acquire(jit_t *j);
void tlab_release(jit_t *j, tlab_t *t);

void *jit_mspace_alloc(size_t size) RETURNS_NONNULL;
void *tlab_alloc(tlab_t *t, size_t size) RETURNS_NONNULL;
jit_stack_trace_t *jit_stack_trace(void);
jit_t *jit_for_thread(void);

typedef void *(*thunk_result_fn_t)(jit_scalar_t *, void *);

bool jit_try_call(jit_t *j, jit_handle_t handle, jit_scalar_t *result, ...);
jit_scalar_t jit_call(jit_t *j, jit_handle_t handle, ...);
void *jit_call_thunk(jit_t *j, vcode_unit_t unit, void *context,
                     thunk_result_fn_t fn, void *arg);
void *jit_call_thunk2(jit_t *j, mir_unit_t *mu, void *context,
                      thunk_result_fn_t fn, void *arg);
bool jit_fastcall(jit_t *j, jit_handle_t handle, jit_scalar_t *result,
                  jit_scalar_t p1, jit_scalar_t p2, tlab_t *tlab);
bool jit_vfastcall(jit_t *j, jit_handle_t handle, const jit_scalar_t *inargs,
                   unsigned nargs, jit_scalar_t *results, unsigned nresults,
                   tlab_t *tlab);
bool jit_call_closure(jit_t *j, ffi_closure_t *closure, jit_scalar_t *result,
                      jit_scalar_t arg, tlab_t *tlab);

tlab_t jit_null_tlab(jit_t *j);

__attribute__((format(printf, 3, 4)))
void jit_msg(const loc_t *where, diag_level_t level, const char *fmt, ...);

__attribute__((noreturn))
void jit_abort(void);

__attribute__((noreturn))
void jit_abort_with_status(int code);

#endif   // _JIT_H
