//
//  Copyright (C) 2011-2026  Nick Gasson
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

// Internals of the simulation kernel shared between model.c and the
// checkpoint code, which has to see the scheduler queues

#ifndef _RT_MODEL_PRIV_H
#define _RT_MODEL_PRIV_H

#include "util.h"
#include "array.h"
#include "hash.h"
#include "cov/cov-api.h"
#include "rt/defs.h"
#include "rt/heap.h"
#include "rt/structs.h"
#include "thread.h"

typedef struct _rt_callback rt_callback_t;
typedef struct _memblock memblock_t;

typedef struct _rt_callback {
   rt_event_fn_t  fn;
   void          *user;
   rt_callback_t *next;
} rt_callback_t;

typedef enum {
   EVENT_TIMEOUT,
   EVENT_DRIVER,
   EVENT_PROCESS,
   EVENT_PSEUDO,
} event_kind_t;

#define MEMBLOCK_ALIGN   64
#define MEMBLOCK_PAGE_SZ 0x800000
#define TRIGGER_TAB_SIZE 64
#define PENDING_MIN      4

#if ASAN_ENABLED
#define MEMBLOCK_REDZONE 16
#else
#define MEMBLOCK_REDZONE 0
#endif

typedef struct _memblock {
   memblock_t *chain;
   size_t      alloc;
   size_t      limit;
   uint8_t     data[];
} memblock_t;

STATIC_ASSERT(sizeof(memblock_t) <= MEMBLOCK_ALIGN);

typedef struct {
   waveform_t    *free_waveforms;
   tlab_t        *tlab;
   rt_wakeable_t *active_obj;
   rt_scope_t    *active_scope;
} __attribute__((aligned(64))) model_thread_t;

typedef void (*defer_fn_t)(rt_model_t *, void *);

typedef struct {
   defer_fn_t  fn;
   void       *arg;
} defer_task_t;

typedef struct {
   defer_task_t *tasks;
   unsigned      count;
   unsigned      max;
} deferq_t;

typedef struct _rt_model {
   tree_t             top;
   hash_t            *scopes;
   rt_scope_t        *root;
   mspace_t          *mspace;
   jit_t             *jit;
   rt_nexus_t        *nexuses;
   rt_nexus_t       **nexus_tail;
   delta_cycle_t      stop_delta;
   int                iteration;
   uint64_t           now;
   uint64_t           trigger_epoch;
   bool               can_create_delta;
   bool               next_is_delta;
   bool               force_stop;
   bool               blocking_update;
   unsigned           n_signals;
   heap_t            *eventq_heap;
   ihash_t           *res_memo;
   rt_watch_t        *watches;
   deferq_t           procq;
   deferq_t           next_procq;
   deferq_t           driverq;
   deferq_t           next_driverq;
   deferq_t           postponedq;
   deferq_t           triggerq;
   deferq_t           inactiveq;
   deferq_t           next_inactiveq;
   deferq_t           nonblockq;
   deferq_t           reschedq;
   heap_t            *driving_heap;
   heap_t            *effective_heap;
   rt_callback_t     *phase_cbs[END_OF_SIMULATION + 1];
   cover_data_t      *cover;
   nvc_rusage_t       ready_rusage;
   nvc_lock_t         memlock;
   memblock_t        *memblocks;
   model_thread_t    *threads[MAX_THREADS];
   signal_list_t      eventsigs;
   bool               shuffle;
   bool               liveness;
   rt_trigger_t      *triggertab[TRIGGER_TAB_SIZE];
   unsigned           ntriggers;
} rt_model_t;

// Entry points used by the checkpoint code to rebuild model state
rt_nexus_t *model_split_nexus(rt_model_t *m, rt_signal_t *s, int offset,
                              int count);
rt_source_t *model_add_source(rt_model_t *m, rt_nexus_t *n,
                              source_kind_t kind);
rt_value_t model_alloc_value(rt_model_t *m, rt_nexus_t *n);
waveform_t *model_alloc_waveform(rt_model_t *m);
uint8_t *model_value_ptr(rt_nexus_t *n, rt_value_t *v);
void model_sched_event(rt_model_t *m, void **pending, rt_wakeable_t *obj);

#endif   // _RT_MODEL_PRIV_H
