/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Runtime-debug.
 *
 * Ghoti.io Runtime-debug is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Runtime-debug is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The debugger's attachment to a context: its key, its attach and teardown,
 * arming, and the YIELD handler that decides whether to stop.
 *
 * One debugger per context, under a cardinality-one key in the YIELD phase
 * (AD-19). The key's destructor is the only way a debugger dies (AD-20). The
 * handler runs at a slow poll on the owner thread, walks the innermost frame
 * only (and the whole stack only to measure depth while a step is active), and
 * votes to pause when a breakpoint matches, a step is satisfied or a pause was
 * asked for. It never votes at a poll that cannot pause: core would turn the
 * pause into an unwind, which would change the program it is only looking at.
 */

#include <ghoti.io/runtime-debug/macros.h>

#include "model_internal.h"

#include <string.h>

void * grdbg_alloc(GRDBG_Debugger * d, size_t size) {
  return d->allocator->malloc_fn(d->allocator->ctx, size);
}

void grdbg_free(GRDBG_Debugger * d, void * pointer) {
  if (pointer != NULL) {
    d->allocator->free_fn(d->allocator->ctx, pointer);
  }
}

bool grdbg_owned(const GRDBG_Debugger * d) {
  return d != NULL && grcore_context_is_owner(d->context);
}

bool grdbg_readable(const GRDBG_Debugger * d) {
  return d != NULL && grcore_context_guest_state_readable(d->context);
}

bool grdbg_wants_arming(const GRDBG_Debugger * d) {
  return d->bp_count > 0 || d->step != GRDBG_STEP_NONE || d->pause_requested;
}

GRDBG_Result grdbg_arm_for(GRDBG_Debugger * d, bool want) {
  if (want && !d->armed) {
    GRCORE_Result r = grcore_port_post(d->port, d->kind);
    if (r != GRCORE_OK) {
      return grdbg_from_core(r) == GRDBG_ERR_INVALID ? GRDBG_ERR_INTERNAL
                                                      : grdbg_from_core(r);
    }
    d->armed = true;
  } else if (!want && d->armed) {
    if (grcore_context_clear_request(d->context, d->kind) == GRCORE_OK) {
      d->armed = false;
    }
  }
  return GRDBG_OK;
}

void grdbg_forget_stop(GRDBG_Debugger * d) {
  d->stop_reason = GRDBG_STOP_NONE;
  d->hit_count = 0;
}

/* The innermost frame's place, and the stack depth when a step needs it. */
typedef struct {
  const char * file;
  int line;
  size_t depth;
  bool valid; ///< The stack was read and has a frame.
} Place;

static void read_place(GRCORE_Context * context, bool want_depth, Place * out) {
  out->file = NULL;
  out->line = 0;
  out->depth = 0;
  out->valid = false;
  GRCORE_FrameWalk walk;
  if (grcore_frame_walk_begin(context, &walk) != GRCORE_OK) {
    return;
  }
  GRCORE_AbstractFrame frame;
  if (!grcore_frame_walk_next(&walk, &frame)) {
    return;
  }
  out->file = frame.location.file;
  out->line = frame.location.line;
  out->depth = 1;
  out->valid = true;
  if (want_depth) {
    while (grcore_frame_walk_next(&walk, &frame)) {
      out->depth++;
    }
  }
}

static bool same_place(const char * file_a, int line_a, const char * file_b,
    int line_b) {
  if (line_a != line_b) {
    return false;
  }
  if (file_a == file_b) {
    return true;
  }
  return file_a != NULL && file_b != NULL && strcmp(file_a, file_b) == 0;
}

/* Whether the step the debugger started is satisfied at `place`. */
static bool step_satisfied(const GRDBG_Debugger * d, const Place * place) {
  bool moved = !same_place(
      place->file, place->line, d->step_file, d->step_line);
  switch (d->step) {
    case GRDBG_STEP_IN:
      return moved || place->depth != d->step_depth;
    case GRDBG_STEP_OVER:
      return place->depth < d->step_depth ||
          (place->depth <= d->step_depth && moved);
    case GRDBG_STEP_OUT:
      return place->depth < d->step_depth;
    case GRDBG_STEP_NONE:
      break;
  }
  return false;
}

static void yield_poll(
    GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
  GRDBG_Debugger * d = value;
  /* This is a new poll: whatever was read at the last stop is stale. */
  grdbg_invalidate(d);
  if (!grdbg_wants_arming(d)) {
    return;
  }
  /* Core refuses a pause at the runtime poll and inside a nested activation
   * by turning it into a limit unwind, which would end the program this
   * handler only means to look at. Core cannot tell a handler which poll it is
   * in, so it says whether a pause is allowed (and the nesting count is asked
   * as well: a pause refused there is the same mistake). */
  if (!grcore_pollcall_pause_allowed(call) ||
      grcore_context_nested_depth(context) > 0) {
    return;
  }
  Place place;
  read_place(context, d->step != GRDBG_STEP_NONE, &place);

  size_t hits = 0;
  if (place.file != NULL) {
    for (size_t i = 0; i < d->bp_count; i++) {
      if (d->bps[i].line == place.line &&
          strcmp(d->bps[i].source, place.file) == 0) {
        /* The array holds at least as many slots as there are breakpoints. */
        d->hits[hits++] = d->bps[i].id;
      }
    }
  }

  GRDBG_StopReason reason = GRDBG_STOP_NONE;
  if (hits > 0) {
    reason = GRDBG_STOP_BREAKPOINT; /* a breakpoint wins over a step */
    d->step = GRDBG_STEP_NONE;
  } else if (d->step != GRDBG_STEP_NONE && place.valid &&
      step_satisfied(d, &place)) {
    /* A place that could not be read is not evaluated: depth zero would
     * satisfy an over or an out that nothing has finished. */
    reason = GRDBG_STOP_STEP;
    d->step = GRDBG_STEP_NONE;
  } else if (d->pause_requested) {
    reason = GRDBG_STOP_PAUSE;
  }
  if (reason == GRDBG_STOP_NONE) {
    return;
  }
  d->pause_requested = false;
  d->stop_reason = reason;
  d->hit_count = reason == GRDBG_STOP_BREAKPOINT ? hits : 0;
  /* A step that finished may leave nothing to do; clearing the kind cannot
   * fail in a way that matters here, and the next call retries. */
  (void)grdbg_arm_for(d, grdbg_wants_arming(d));
  grcore_pollcall_vote(call, GRCORE_VERDICT_PAUSE);
}

static void key_destroy(GRCORE_Context * context, void * value) {
  (void)context;
  GRDBG_Debugger * d = value;
  grdbg_invalidate(d);
  grdbg_free_breakpoints(d);
  grdbg_free(d, d->bps);
  grdbg_free(d, d->hits);
  grdbg_free(d, d->frames);
  grcore_port_release(d->port);
  d->allocator->free_fn(d->allocator->ctx, d);
}

const GRCORE_Key grdbg_key = {"runtime-debug", GRCORE_CARDINALITY_ONE,
    GRCORE_PHASE_YIELD, key_destroy, yield_poll, NULL, NULL, NULL};

const GRCORE_Key * grdbg_debugger_key(void) {
  return &grdbg_key;
}

GRDBG_Result grdbg_debugger_attach_with_allocator(GRCORE_Context * context,
    const GRDBG_Allocator * allocator, const GRDBG_Limits * limits,
    GRDBG_Debugger ** out_debugger) {
  if (context == NULL || out_debugger == NULL ||
      !grcore_context_is_owner(context) ||
      grcore_context_slot(context, &grdbg_key) != NULL) {
    return GRDBG_ERR_INVALID;
  }
  if (allocator == NULL) {
    allocator = grdbg_allocator_default();
  }
  GRDBG_Debugger * d = allocator->calloc_fn(allocator->ctx, 1, sizeof *d);
  if (d == NULL) {
    return GRDBG_ERR_OOM;
  }
  d->context = context;
  d->allocator = allocator;
  d->next_id = 1;
  d->generation = 1;
  grdbg_limits_resolve(limits, &d->limits);

  GRCORE_Result r = grcore_context_port(context, &d->port);
  if (r == GRCORE_OK) {
    r = grcore_context_request_kind(context, &grdbg_key, &d->kind);
    if (r != GRCORE_OK) {
      grcore_port_release(d->port);
    }
  }
  if (r == GRCORE_OK) {
    r = grcore_context_register(context, &grdbg_key, d);
    if (r != GRCORE_OK) {
      grcore_port_release(d->port);
    }
  }
  if (r != GRCORE_OK) {
    allocator->free_fn(allocator->ctx, d);
    return grdbg_from_core(r);
  }
  *out_debugger = d;
  return GRDBG_OK;
}

GRDBG_Result grdbg_debugger_attach(GRCORE_Context * context,
    const GRDBG_Limits * limits, GRDBG_Debugger ** out_debugger) {
  return grdbg_debugger_attach_with_allocator(
      context, NULL, limits, out_debugger);
}

GRDBG_Debugger * grdbg_debugger_get(const GRCORE_Context * context) {
  if (context == NULL || !grcore_context_is_owner(context)) {
    return NULL;
  }
  return grcore_context_slot(context, &grdbg_key);
}

GRCORE_Context * grdbg_debugger_context(const GRDBG_Debugger * debugger) {
  return debugger == NULL ? NULL : debugger->context;
}

const GRDBG_Allocator * grdbg_debugger_allocator(const GRDBG_Debugger * debugger) {
  return debugger->allocator;
}

bool grdbg_debugger_armed(const GRDBG_Debugger * debugger) {
  return grdbg_owned(debugger) && debugger->armed;
}

uint64_t grdbg_debugger_generation(const GRDBG_Debugger * debugger) {
  return grdbg_owned(debugger) ? debugger->generation : 0;
}
