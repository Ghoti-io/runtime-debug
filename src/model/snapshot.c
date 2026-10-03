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
 * The state of a stop: frames recorded from the abstract frame walk, and
 * scopes and variables read on demand through the engine's descriptor.
 *
 * Reading is allowed only while the context is paused or at-poll and held by
 * the calling thread (AD-20). The frames are copied by value, innermost first,
 * up to `max_frames`; the total is counted all the same. Scopes and variables
 * are not recorded: they are read when asked, and the strings they produce live
 * in an arena that is freed, with the frames, at the next resume. A recorded
 * frame is checked against the stack before it is used, so a snapshot that has
 * outlived its stop is refused instead of misread.
 */

#include <ghoti.io/runtime-debug/macros.h>

#include "model_internal.h"

#include <stdio.h>
#include <string.h>

/* A displayed value is cut here. An inspector can return an arbitrarily long
 * text, and a debugger shows a value, it does not dump one. */
#define VALUE_TEXT_MAX 8192u
#define CHUNK_MIN 2048u

void grdbg_invalidate(GRDBG_Debugger * d) {
  d->generation++;
  if (!d->have_snapshot && d->chunks == NULL) {
    return;
  }
  d->have_snapshot = false;
  d->frame_count = 0;
  d->frame_total = 0;
  while (d->chunks != NULL) {
    GRDBG_Chunk * next = d->chunks->next;
    grdbg_free(d, d->chunks);
    d->chunks = next;
  }
}

static void * arena_alloc(GRDBG_Debugger * d, size_t size) {
  size = (size + 7u) & ~(size_t)7u;
  GRDBG_Chunk * chunk = d->chunks;
  if (chunk == NULL || chunk->capacity - chunk->used < size) {
    size_t capacity = size > CHUNK_MIN ? size : CHUNK_MIN;
    chunk = grdbg_alloc(d, sizeof *chunk + capacity);
    if (chunk == NULL) {
      return NULL;
    }
    chunk->capacity = capacity;
    chunk->used = 0;
    chunk->next = d->chunks;
    d->chunks = chunk;
  }
  void * p = (char *)(chunk + 1) + chunk->used;
  chunk->used += size;
  return p;
}

char * grdbg_arena_string(
    GRDBG_Debugger * d, const char * text, size_t length) {
  char * copy = arena_alloc(d, length + 1);
  if (copy != NULL) {
    if (length > 0) {
      memcpy(copy, text, length);
    }
    copy[length] = '\0';
  }
  return copy;
}

GRDBG_Result grdbg_snapshot_ensure(GRDBG_Debugger * d) {
  if (!grdbg_owned(d) || !grdbg_readable(d)) {
    return GRDBG_ERR_INVALID;
  }
  if (d->have_snapshot) {
    return GRDBG_OK;
  }
  GRCORE_FrameWalk walk;
  if (grcore_frame_walk_begin(d->context, &walk) != GRCORE_OK) {
    return GRDBG_ERR_INVALID;
  }
  d->frame_count = 0;
  d->frame_total = 0;
  GRCORE_AbstractFrame frame;
  while (grcore_frame_walk_next(&walk, &frame)) {
    if (d->frame_total < d->limits.max_frames) {
      if (d->frame_count == d->frame_capacity) {
        size_t capacity = d->frame_capacity == 0 ? 16 : d->frame_capacity * 2;
        if (capacity > d->limits.max_frames) {
          capacity = d->limits.max_frames;
        }
        GRDBG_SnapFrame * grown = grdbg_alloc(d, capacity * sizeof *grown);
        if (grown == NULL) {
          d->frame_count = 0;
          d->frame_total = 0;
          return GRDBG_ERR_OOM;
        }
        if (d->frame_count > 0) {
          memcpy(grown, d->frames, d->frame_count * sizeof *grown);
        }
        grdbg_free(d, d->frames);
        d->frames = grown;
        d->frame_capacity = capacity;
      }
      GRDBG_SnapFrame * slot = &d->frames[d->frame_count++];
      slot->frame = frame;
      snprintf(slot->name, sizeof slot->name, "frame %zu", d->frame_total + 1);
    }
    d->frame_total++;
  }
  d->have_snapshot = true;
  return GRDBG_OK;
}

/* A recorded frame, after checking that it is still a frame of this stack
 * where the stop left it. */
static GRDBG_Result frame_at(
    GRDBG_Debugger * d, size_t index, GRDBG_SnapFrame ** out) {
  if (d == NULL) {
    return GRDBG_ERR_INVALID;
  }
  GRDBG_Result r = grdbg_snapshot_ensure(d);
  if (r != GRDBG_OK) {
    return r;
  }
  if (index >= d->frame_count) {
    return GRDBG_ERR_INVALID;
  }
  GRDBG_SnapFrame * f = &d->frames[index];
  const GRCORE_Stack * stack = grcore_context_stack(d->context);
  GRCORE_PollIdentity now;
  if (stack == NULL || !grcore_stack_frame_valid(stack, f->frame.frame) ||
      grcore_stack_identity(stack, f->frame.frame, &now) != GRCORE_OK ||
      now.function != f->frame.identity.function ||
      now.offset != f->frame.identity.offset) {
    return GRDBG_ERR_INVALID;
  }
  *out = f;
  return GRDBG_OK;
}

GRDBG_Result grdbg_debugger_frames(GRDBG_Debugger * d, size_t * out_total,
    size_t * out_available) {
  if (d == NULL) {
    return GRDBG_ERR_INVALID;
  }
  GRDBG_Result r = grdbg_snapshot_ensure(d);
  if (r != GRDBG_OK) {
    return r;
  }
  if (out_total != NULL) {
    *out_total = d->frame_total;
  }
  if (out_available != NULL) {
    *out_available = d->frame_count;
  }
  return GRDBG_OK;
}

GRDBG_Result grdbg_debugger_frame(
    GRDBG_Debugger * d, size_t index, GRDBG_Frame * out) {
  GRDBG_SnapFrame * f;
  if (out == NULL) {
    return GRDBG_ERR_INVALID;
  }
  GRDBG_Result r = frame_at(d, index, &f);
  if (r != GRDBG_OK) {
    return r;
  }
  out->depth = f->frame.depth;
  out->name = f->name;
  out->file = f->frame.location.file;
  out->line = f->frame.location.line;
  out->engine = f->frame.descriptor != NULL ? f->frame.descriptor->name : NULL;
  return GRDBG_OK;
}

GRDBG_Result grdbg_debugger_scope_count(
    GRDBG_Debugger * d, size_t frame, size_t * out_count) {
  GRDBG_SnapFrame * f;
  if (out_count == NULL) {
    return GRDBG_ERR_INVALID;
  }
  GRDBG_Result r = frame_at(d, frame, &f);
  if (r != GRDBG_OK) {
    return r;
  }
  *out_count = grcore_frame_scope_count(&f->frame);
  return GRDBG_OK;
}

static GRDBG_ScopeKind scope_kind(GRCORE_ScopeKind kind) {
  switch (kind) {
    case GRCORE_SCOPE_CLOSURE:
      return GRDBG_SCOPE_CLOSURE;
    case GRCORE_SCOPE_GLOBAL:
      return GRDBG_SCOPE_GLOBAL;
    case GRCORE_SCOPE_LOCAL:
      break;
  }
  return GRDBG_SCOPE_LOCAL;
}

GRDBG_Result grdbg_debugger_scope(GRDBG_Debugger * d, size_t frame,
    size_t scope, GRDBG_Scope * out) {
  GRDBG_SnapFrame * f;
  if (out == NULL) {
    return GRDBG_ERR_INVALID;
  }
  GRDBG_Result r = frame_at(d, frame, &f);
  if (r != GRDBG_OK) {
    return r;
  }
  GRCORE_ScopeInfo info;
  if (grcore_frame_scope(&f->frame, scope, &info) != GRCORE_OK) {
    return GRDBG_ERR_INVALID;
  }
  const char * name = info.name != NULL ? info.name : "";
  char * copy = grdbg_arena_string(d, name, strlen(name));
  if (copy == NULL) {
    return GRDBG_ERR_OOM;
  }
  out->kind = scope_kind(info.kind);
  out->name = copy;
  out->variable_count = info.variable_count;
  return GRDBG_OK;
}

/* The engine's text for a value, cut at VALUE_TEXT_MAX, in the arena. */
static GRDBG_Result value_text(GRDBG_Debugger * d, const GRCORE_AbstractFrame * f,
    const GRCORE_Variable * v, const char ** out) {
  char small[256];
  size_t length = 0;
  if (grcore_engine_inspect(d->context, f->engine, v->kind, v->value, small,
          sizeof small, &length) != GRCORE_OK) {
    return GRDBG_ERR_INVALID;
  }
  if (length < sizeof small) {
    char * copy = grdbg_arena_string(d, small, length);
    if (copy == NULL) {
      return GRDBG_ERR_OOM;
    }
    *out = copy;
    return GRDBG_OK;
  }
  size_t keep = length < VALUE_TEXT_MAX ? length : VALUE_TEXT_MAX;
  char * big = grdbg_alloc(d, length + 1);
  if (big == NULL) {
    return GRDBG_ERR_OOM;
  }
  size_t again = 0;
  GRDBG_Result r = GRDBG_OK;
  if (grcore_engine_inspect(d->context, f->engine, v->kind, v->value, big,
          length + 1, &again) != GRCORE_OK) {
    r = GRDBG_ERR_INVALID;
  } else {
    if (again < keep) {
      keep = again;
    }
    char * copy = grdbg_arena_string(d, big, keep);
    if (copy == NULL) {
      r = GRDBG_ERR_OOM;
    } else {
      *out = copy;
    }
  }
  grdbg_free(d, big);
  return r;
}

static GRDBG_Result fill_variable(GRDBG_Debugger * d,
    const GRCORE_AbstractFrame * f, const GRCORE_Variable * v,
    GRDBG_Variable * out) {
  const char * name = v->name != NULL ? v->name : "";
  char * name_copy = grdbg_arena_string(d, name, strlen(name));
  if (name_copy == NULL) {
    return GRDBG_ERR_OOM;
  }
  const char * text;
  GRDBG_Result r = value_text(d, f, v, &text);
  if (r != GRDBG_OK) {
    return r;
  }
  out->name = name_copy;
  out->text = text;
  out->kind = v->kind == GRCORE_SLOT_VALUE ? GRDBG_VALUE_ENGINE : GRDBG_VALUE_RAW;
  return GRDBG_OK;
}

GRDBG_Result grdbg_debugger_variable(GRDBG_Debugger * d, size_t frame,
    size_t scope, size_t index, GRDBG_Variable * out) {
  GRDBG_SnapFrame * f;
  if (out == NULL) {
    return GRDBG_ERR_INVALID;
  }
  GRDBG_Result r = frame_at(d, frame, &f);
  if (r != GRDBG_OK) {
    return r;
  }
  GRCORE_Variable v;
  if (grcore_frame_variable(&f->frame, scope, index, &v) != GRCORE_OK) {
    return GRDBG_ERR_INVALID;
  }
  return fill_variable(d, &f->frame, &v, out);
}

GRDBG_Result grdbg_debugger_find_variable(GRDBG_Debugger * d, size_t frame,
    const char * name, bool * out_found, GRDBG_Variable * out) {
  GRDBG_SnapFrame * f;
  if (name == NULL || out_found == NULL) {
    return GRDBG_ERR_INVALID;
  }
  GRDBG_Result r = frame_at(d, frame, &f);
  if (r != GRDBG_OK) {
    return r;
  }
  size_t scopes = grcore_frame_scope_count(&f->frame);
  for (size_t s = 0; s < scopes; s++) {
    GRCORE_ScopeInfo info;
    if (grcore_frame_scope(&f->frame, s, &info) != GRCORE_OK) {
      continue;
    }
    for (size_t i = 0; i < info.variable_count; i++) {
      GRCORE_Variable v;
      if (grcore_frame_variable(&f->frame, s, i, &v) != GRCORE_OK ||
          v.name == NULL || strcmp(v.name, name) != 0) {
        continue;
      }
      if (out != NULL) {
        r = fill_variable(d, &f->frame, &v, out);
        if (r != GRDBG_OK) {
          return r;
        }
      }
      *out_found = true;
      return GRDBG_OK;
    }
  }
  *out_found = false;
  return GRDBG_OK;
}
