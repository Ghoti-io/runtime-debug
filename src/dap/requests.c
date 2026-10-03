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
 * The requests: each is read, turned into a call on the model, and answered.
 *
 * The adapter holds no debug logic beyond id mapping and the shapes of
 * messages (AD-15). A frame id is the model's frame index plus one; a
 * variablesReference names a (frame, scope) pair and is issued by `scopes`
 * and forgotten when the model says what it returned earlier is stale. Lines
 * are converted at the edge: the model is 1-based.
 *
 * Every response is written before the request that asked for it is
 * considered handled, and a request that asks the host to proceed records that
 * in `dap->proceed` only after its response is out.
 */

#include <ghoti.io/runtime-debug/macros.h>

#include "dap_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define MAX_REFS (1u << 20)

typedef GRDBG_Result (*Handler)(GRDBG_Dap *, const GRDBG_Request *);

/* ---- arguments ---------------------------------------------------------- */

static const GTEXT_JSON_Value * arg(const GRDBG_Request * rq, const char * key) {
  return gtext_json_object_get(rq->arguments, key, strlen(key));
}

/* 1 if present and an integer, 0 if absent, -1 if present and not one. */
static int arg_int(const GRDBG_Request * rq, const char * key, int64_t * out) {
  const GTEXT_JSON_Value * v = arg(rq, key);
  if (v == NULL) {
    return 0;
  }
  return gtext_json_get_i64(v, out) == GTEXT_JSON_OK ? 1 : -1;
}

static int arg_bool(const GRDBG_Request * rq, const char * key, bool * out) {
  const GTEXT_JSON_Value * v = arg(rq, key);
  if (v == NULL) {
    return 0;
  }
  return gtext_json_get_bool(v, out) == GTEXT_JSON_OK ? 1 : -1;
}

/* A string argument, copied and terminated on the session's allocator. NULL
 * with *bad set for a string that is not usable (wrong type, or holding a
 * NUL); NULL with *bad clear for absent or out of memory. */
static char * arg_string(GRDBG_Dap * dap, const GTEXT_JSON_Value * v, bool * bad,
    bool * oom) {
  *bad = false;
  *oom = false;
  if (v == NULL) {
    return NULL;
  }
  const char * s;
  size_t n;
  if (gtext_json_get_string(v, &s, &n) != GTEXT_JSON_OK ||
      memchr(s, '\0', n) != NULL) {
    *bad = true;
    return NULL;
  }
  char * copy = dap->allocator->malloc_fn(dap->allocator->ctx, n + 1);
  if (copy == NULL) {
    *oom = true;
    return NULL;
  }
  memcpy(copy, s, n);
  copy[n] = '\0';
  return copy;
}

static void free_string(GRDBG_Dap * dap, char * s) {
  if (s != NULL) {
    dap->allocator->free_fn(dap->allocator->ctx, s);
  }
}

static int64_t to_client_line(const GRDBG_Dap * dap, int line) {
  return dap->lines_from_1 ? line : (int64_t)line - 1;
}

/* A client line to the model's, or zero if there is none. */
static int to_model_line(const GRDBG_Dap * dap, int64_t line) {
  int64_t model = dap->lines_from_1 ? line : line + 1;
  return model >= 1 && model <= INT_MAX ? (int)model : 0;
}

static int64_t client_column(const GRDBG_Dap * dap) {
  return dap->columns_from_1 ? 1 : 0;
}

/* ---- ids ---------------------------------------------------------------- */

/* Forgets what the model says is gone. When the generation has moved, the
 * variablesReferences are forgotten, and the frame ids move on: ids are numbered
 * from 1 and never reused, so the stop that follows starts above every id the
 * earlier one could have issued, and a stale id is unknown, not a wrong frame.
 * `available` is how many frames this stop has, which the ids it issues span. */
static void sync_ids(GRDBG_Dap * dap, size_t available) {
  uint64_t now = grdbg_debugger_generation(dap->debugger);
  if (now != dap->generation) {
    /* (The first stop spends nothing: ids start at 1.) */
    if (dap->generation != 0) {
      dap->frame_base += dap->frame_span > 0 ? dap->frame_span : 1;
    }
    dap->ref_count = 0;
    dap->generation = now;
    dap->frame_span = 0;
  }
  if (available > dap->frame_span) {
    dap->frame_span = available;
  }
}

/* The variablesReference for (frame, scope), issued if it is new. Zero if the
 * table is full or memory is short. */
static size_t ref_for(GRDBG_Dap * dap, size_t frame, size_t scope) {
  for (size_t i = 0; i < dap->ref_count; i++) {
    if (dap->refs[i].frame == frame && dap->refs[i].scope == scope) {
      return i + 1;
    }
  }
  if (dap->ref_count >= MAX_REFS) {
    return 0;
  }
  if (dap->ref_count == dap->ref_capacity) {
    size_t capacity = dap->ref_capacity == 0 ? 8 : dap->ref_capacity * 2;
    GRDBG_Ref * grown = dap->allocator->realloc_fn(
        dap->allocator->ctx, dap->refs, capacity * sizeof *grown);
    if (grown == NULL) {
      return 0;
    }
    dap->refs = grown;
    dap->ref_capacity = capacity;
  }
  dap->refs[dap->ref_count].frame = frame;
  dap->refs[dap->ref_count].scope = scope;
  return ++dap->ref_count;
}

/* ---- common replies ----------------------------------------------------- */

static GRDBG_Result not_stopped(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  return grdbg_response_error(dap, rq, "the program is not stopped");
}

/* Whether the program is stopped, as the model sees it. */
static bool stopped(GRDBG_Dap * dap) {
  size_t total;
  return grdbg_debugger_frames(dap->debugger, &total, NULL) == GRDBG_OK;
}

/* The frame id of an argument, as a model index, or SIZE_MAX if there is none
 * (the caller says whether absent is an error). */
static GRDBG_Result frame_arg(GRDBG_Dap * dap, const GRDBG_Request * rq,
    const char * key, bool required, size_t * out_index, bool * out_replied) {
  *out_replied = false;
  int64_t id = 0;
  int have = arg_int(rq, key, &id);
  size_t total, available;
  if (grdbg_debugger_frames(dap->debugger, &total, &available) != GRDBG_OK) {
    *out_replied = true;
    return not_stopped(dap, rq);
  }
  if (have == 0 && !required) {
    *out_index = 0;
    return GRDBG_OK;
  }
  sync_ids(dap, available);
  if (have != 1 || id < 1 || (uint64_t)id <= dap->frame_base ||
      (uint64_t)id - dap->frame_base > available) {
    *out_replied = true;
    return grdbg_response_error(dap, rq, "unknown frame id");
  }
  *out_index = (size_t)((uint64_t)id - dap->frame_base) - 1;
  return GRDBG_OK;
}

static const char * kind_name(GRDBG_ValueKind kind) {
  return kind == GRDBG_VALUE_ENGINE ? "value" : "raw";
}

/* ---- the requests ------------------------------------------------------- */

static GRDBG_Result do_initialize(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  bool flag;
  if (arg_bool(rq, "linesStartAt1", &flag) == 1) {
    dap->lines_from_1 = flag;
  }
  if (arg_bool(rq, "columnsStartAt1", &flag) == 1) {
    dap->columns_from_1 = flag;
  }
  GRDBG_Out out;
  GRDBG_Result r = grdbg_response_begin(dap, rq, true, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  grdbg_out_key(&out, "supportsConfigurationDoneRequest");
  grdbg_out_bool(&out, true);
  r = grdbg_response_end(dap, rq, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  r = grdbg_event_begin(dap, "initialized", false, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  return grdbg_event_end(dap, &out);
}

/* launch and attach: the host already started the context. */
static GRDBG_Result do_accept(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  return grdbg_response_ok(dap, rq);
}

static GRDBG_Result do_set_breakpoints(
    GRDBG_Dap * dap, const GRDBG_Request * rq) {
  const GTEXT_JSON_Value * source = arg(rq, "source");
  bool bad, oom;
  char * path = arg_string(dap,
      source == NULL ? NULL : gtext_json_object_get(source, "path", 4), &bad,
      &oom);
  if (oom) {
    return GRDBG_ERR_OOM;
  }
  if (path == NULL) {
    return grdbg_response_error(
        dap, rq, "setBreakpoints needs a source with a string path");
  }

  /* The list: `breakpoints` objects with a line, or the older `lines`. */
  const GTEXT_JSON_Value * list = arg(rq, "breakpoints");
  bool objects = list != NULL;
  if (list == NULL) {
    list = arg(rq, "lines");
  }
  size_t count = 0;
  if (list != NULL) {
    if (gtext_json_typeof(list) != GTEXT_JSON_ARRAY) {
      free_string(dap, path);
      return grdbg_response_error(dap, rq, "breakpoints must be an array");
    }
    count = gtext_json_array_size(list);
  }
  if (count > dap->limits.max_breakpoints) {
    free_string(dap, path);
    return grdbg_response_error(dap, rq, "too many breakpoints");
  }
  int * lines = NULL;
  uint64_t * ids = NULL;
  if (count > 0) {
    lines = dap->allocator->malloc_fn(dap->allocator->ctx, count * sizeof *lines);
    ids = dap->allocator->malloc_fn(dap->allocator->ctx, count * sizeof *ids);
    if (lines == NULL || ids == NULL) {
      dap->allocator->free_fn(dap->allocator->ctx, lines);
      dap->allocator->free_fn(dap->allocator->ctx, ids);
      free_string(dap, path);
      return GRDBG_ERR_OOM;
    }
  }
  GRDBG_Result result = GRDBG_OK;
  const char * problem = NULL;
  for (size_t i = 0; i < count && problem == NULL; i++) {
    const GTEXT_JSON_Value * item = gtext_json_array_get(list, i);
    const GTEXT_JSON_Value * line_value = objects
        ? gtext_json_object_get(item, "line", 4)
        : item;
    int64_t line;
    if (line_value == NULL || gtext_json_get_i64(line_value, &line) != GTEXT_JSON_OK) {
      problem = "a breakpoint needs an integer line";
    } else if ((lines[i] = to_model_line(dap, line)) == 0) {
      problem = "a breakpoint line is out of range";
    }
  }
  if (problem == NULL) {
    GRDBG_Result set = grdbg_debugger_set_breakpoints(
        dap->debugger, path, lines, count, ids);
    if (set == GRDBG_ERR_LIMIT) {
      problem = "too many breakpoints";
    } else if (set == GRDBG_ERR_OOM) {
      result = GRDBG_ERR_OOM;
    } else if (set != GRDBG_OK) {
      problem = "the breakpoints could not be set";
    }
  }
  if (problem != NULL) {
    result = grdbg_response_error(dap, rq, problem);
  } else if (result == GRDBG_OK) {
    GRDBG_Out out;
    result = grdbg_response_begin(dap, rq, true, &out);
    if (result == GRDBG_OK) {
      grdbg_out_key(&out, "breakpoints");
      grdbg_out_arr_begin(&out);
      for (size_t i = 0; i < count; i++) {
        grdbg_out_obj_begin(&out);
        grdbg_out_key(&out, "id");
        grdbg_out_int(&out, (int64_t)ids[i]);
        grdbg_out_key(&out, "verified");
        grdbg_out_bool(&out, true);
        grdbg_out_key(&out, "line");
        grdbg_out_int(&out, to_client_line(dap, lines[i]));
        grdbg_out_obj_end(&out);
      }
      grdbg_out_arr_end(&out);
      result = grdbg_response_end(dap, rq, &out);
    }
  }
  dap->allocator->free_fn(dap->allocator->ctx, lines);
  dap->allocator->free_fn(dap->allocator->ctx, ids);
  free_string(dap, path);
  return result;
}

static GRDBG_Result do_set_exception_breakpoints(
    GRDBG_Dap * dap, const GRDBG_Request * rq) {
  return grdbg_response_ok(dap, rq);
}

static GRDBG_Result do_configuration_done(
    GRDBG_Dap * dap, const GRDBG_Request * rq) {
  GRDBG_Result r = grdbg_response_ok(dap, rq);
  if (r == GRDBG_OK) {
    dap->proceed = (int)GRDBG_SERVE_RESUME + 1;
  }
  return r;
}

static GRDBG_Result do_threads(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  GRDBG_Out out;
  GRDBG_Result r = grdbg_response_begin(dap, rq, true, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  grdbg_out_key(&out, "threads");
  grdbg_out_arr_begin(&out);
  grdbg_out_obj_begin(&out);
  grdbg_out_key(&out, "id");
  grdbg_out_int(&out, 1);
  grdbg_out_key(&out, "name");
  grdbg_out_string(&out, "main");
  grdbg_out_obj_end(&out);
  grdbg_out_arr_end(&out);
  return grdbg_response_end(dap, rq, &out);
}

static GRDBG_Result do_stack_trace(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  size_t total, available;
  if (grdbg_debugger_frames(dap->debugger, &total, &available) != GRDBG_OK) {
    return not_stopped(dap, rq);
  }
  sync_ids(dap, available);
  int64_t start = 0, levels = 0;
  if (arg_int(rq, "startFrame", &start) == -1 || start < 0 ||
      arg_int(rq, "levels", &levels) == -1 || levels < 0) {
    return grdbg_response_error(dap, rq, "startFrame and levels must be integers of at least zero");
  }
  size_t first = (uint64_t)start > available ? available : (size_t)start;
  size_t last = available;
  if (levels > 0 && (uint64_t)levels < last - first) {
    last = first + (size_t)levels;
  }
  if (last - first > dap->limits.max_frames) {
    last = first + dap->limits.max_frames;
  }
  GRDBG_Out out;
  GRDBG_Result r = grdbg_response_begin(dap, rq, true, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  grdbg_out_key(&out, "stackFrames");
  grdbg_out_arr_begin(&out);
  for (size_t i = first; i < last; i++) {
    GRDBG_Frame frame;
    if (grdbg_debugger_frame(dap->debugger, i, &frame) != GRDBG_OK) {
      out.problem = "a frame could not be read";
      out.failed = true;
      break;
    }
    grdbg_out_obj_begin(&out);
    grdbg_out_key(&out, "id");
    grdbg_out_int(&out, (int64_t)(dap->frame_base + i + 1));
    grdbg_out_key(&out, "name");
    grdbg_out_string(&out, frame.name);
    if (frame.file != NULL) {
      grdbg_out_key(&out, "source");
      grdbg_out_obj_begin(&out);
      grdbg_out_key(&out, "name");
      grdbg_out_string(&out, frame.file);
      grdbg_out_key(&out, "path");
      grdbg_out_string(&out, frame.file);
      grdbg_out_obj_end(&out);
    }
    grdbg_out_key(&out, "line");
    grdbg_out_int(&out, frame.file != NULL ? to_client_line(dap, frame.line) : 0);
    grdbg_out_key(&out, "column");
    grdbg_out_int(&out, client_column(dap));
    grdbg_out_obj_end(&out);
  }
  grdbg_out_arr_end(&out);
  grdbg_out_key(&out, "totalFrames");
  grdbg_out_int(&out, (int64_t)total);
  return grdbg_response_end(dap, rq, &out);
}

static const char * default_scope_name(GRDBG_ScopeKind kind) {
  switch (kind) {
    case GRDBG_SCOPE_CLOSURE:
      return "Closure";
    case GRDBG_SCOPE_GLOBAL:
      return "Globals";
    case GRDBG_SCOPE_LOCAL:
      break;
  }
  return "Locals";
}

static GRDBG_Result do_scopes(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  size_t frame;
  bool replied;
  GRDBG_Result r = frame_arg(dap, rq, "frameId", true, &frame, &replied);
  if (replied || r != GRDBG_OK) {
    return r;
  }
  size_t count;
  if (grdbg_debugger_scope_count(dap->debugger, frame, &count) != GRDBG_OK) {
    return grdbg_response_error(dap, rq, "the frame could not be read");
  }
  GRDBG_Out out;
  r = grdbg_response_begin(dap, rq, true, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  grdbg_out_key(&out, "scopes");
  grdbg_out_arr_begin(&out);
  for (size_t i = 0; i < count; i++) {
    GRDBG_Scope scope;
    if (grdbg_debugger_scope(dap->debugger, frame, i, &scope) != GRDBG_OK) {
      out.problem = "a scope could not be read";
      out.failed = true;
      break;
    }
    size_t ref = ref_for(dap, frame, i);
    if (ref == 0) {
      out.problem = "too many scopes have been issued for this stop";
      out.failed = true;
      break;
    }
    grdbg_out_obj_begin(&out);
    grdbg_out_key(&out, "name");
    grdbg_out_string(&out, scope.name[0] != '\0' ? scope.name
                                                  : default_scope_name(scope.kind));
    if (scope.kind == GRDBG_SCOPE_LOCAL) {
      grdbg_out_key(&out, "presentationHint");
      grdbg_out_string(&out, "locals");
    }
    grdbg_out_key(&out, "variablesReference");
    grdbg_out_int(&out, (int64_t)ref);
    grdbg_out_key(&out, "namedVariables");
    grdbg_out_int(&out, (int64_t)scope.variable_count);
    grdbg_out_key(&out, "expensive");
    grdbg_out_bool(&out, false);
    grdbg_out_obj_end(&out);
  }
  grdbg_out_arr_end(&out);
  return grdbg_response_end(dap, rq, &out);
}

static GRDBG_Result do_variables(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  if (!stopped(dap)) {
    return not_stopped(dap, rq);
  }
  {
    size_t total, available;
    if (grdbg_debugger_frames(dap->debugger, &total, &available) == GRDBG_OK) {
      sync_ids(dap, available);
    }
  }
  int64_t ref = 0, start = 0, count = 0;
  if (arg_int(rq, "variablesReference", &ref) != 1 || ref < 1 ||
      (uint64_t)ref > dap->ref_count) {
    return grdbg_response_error(dap, rq, "unknown variablesReference");
  }
  if (arg_int(rq, "start", &start) == -1 || start < 0 ||
      arg_int(rq, "count", &count) == -1 || count < 0) {
    return grdbg_response_error(dap, rq, "start and count must be integers of at least zero");
  }
  GRDBG_Ref entry = dap->refs[ref - 1];
  GRDBG_Scope scope;
  if (grdbg_debugger_scope(dap->debugger, entry.frame, entry.scope, &scope) != GRDBG_OK) {
    return grdbg_response_error(dap, rq, "the scope could not be read");
  }
  size_t first = (uint64_t)start > scope.variable_count ? scope.variable_count
                                                        : (size_t)start;
  size_t last = scope.variable_count;
  if (count > 0 && (uint64_t)count < last - first) {
    last = first + (size_t)count;
  }
  if (last - first > dap->limits.max_variables) {
    last = first + dap->limits.max_variables;
  }
  GRDBG_Out out;
  GRDBG_Result r = grdbg_response_begin(dap, rq, true, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  grdbg_out_key(&out, "variables");
  grdbg_out_arr_begin(&out);
  for (size_t i = first; i < last; i++) {
    GRDBG_Variable v;
    if (grdbg_debugger_variable(dap->debugger, entry.frame, entry.scope, i, &v) != GRDBG_OK) {
      out.problem = "a variable could not be read";
      out.failed = true;
      break;
    }
    grdbg_out_obj_begin(&out);
    grdbg_out_key(&out, "name");
    grdbg_out_string(&out, v.name);
    grdbg_out_key(&out, "value");
    grdbg_out_string(&out, v.text);
    grdbg_out_key(&out, "type");
    grdbg_out_string(&out, kind_name(v.kind));
    grdbg_out_key(&out, "variablesReference");
    grdbg_out_int(&out, 0);
    grdbg_out_obj_end(&out);
  }
  grdbg_out_arr_end(&out);
  return grdbg_response_end(dap, rq, &out);
}

static GRDBG_Result do_evaluate(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  size_t frame;
  bool replied;
  GRDBG_Result r = frame_arg(dap, rq, "frameId", false, &frame, &replied);
  if (replied || r != GRDBG_OK) {
    return r;
  }
  bool bad, oom;
  char * name = arg_string(dap, arg(rq, "expression"), &bad, &oom);
  if (oom) {
    return GRDBG_ERR_OOM;
  }
  bool found = false;
  GRDBG_Variable v;
  if (name != NULL) {
    GRDBG_Result looked = grdbg_debugger_find_variable(
        dap->debugger, frame, name, &found, &v);
    free_string(dap, name);
    if (looked == GRDBG_ERR_OOM) {
      return looked;
    }
    if (looked != GRDBG_OK) {
      return grdbg_response_error(dap, rq, "the frame could not be read");
    }
  }
  if (!found) {
    return grdbg_response_error(
        dap, rq, "evaluation of expressions is not supported");
  }
  GRDBG_Out out;
  r = grdbg_response_begin(dap, rq, true, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  grdbg_out_key(&out, "result");
  grdbg_out_string(&out, v.text);
  grdbg_out_key(&out, "type");
  grdbg_out_string(&out, kind_name(v.kind));
  grdbg_out_key(&out, "variablesReference");
  grdbg_out_int(&out, 0);
  return grdbg_response_end(dap, rq, &out);
}

static GRDBG_Result do_continue(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  if (!stopped(dap)) {
    return not_stopped(dap, rq);
  }
  if (grdbg_debugger_continue(dap->debugger) != GRDBG_OK) {
    return grdbg_response_error(dap, rq, "the program could not be continued");
  }
  /* The model is already resumed, so the host must be told to resume whether
   * or not the response gets out. */
  dap->proceed = (int)GRDBG_SERVE_RESUME + 1;
  GRDBG_Out out;
  GRDBG_Result r = grdbg_response_begin(dap, rq, true, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  grdbg_out_key(&out, "allThreadsContinued");
  grdbg_out_bool(&out, true);
  return grdbg_response_end(dap, rq, &out);
}

static GRDBG_Result do_step(
    GRDBG_Dap * dap, const GRDBG_Request * rq, GRDBG_StepKind kind) {
  if (!stopped(dap)) {
    return not_stopped(dap, rq);
  }
  GRDBG_Result s = grdbg_debugger_step(dap->debugger, kind);
  if (s == GRDBG_ERR_OOM) {
    return s;
  }
  if (s != GRDBG_OK) {
    return grdbg_response_error(dap, rq, "the step could not be started");
  }
  dap->proceed = (int)GRDBG_SERVE_RESUME + 1; /* the step is armed: see do_continue */
  return grdbg_response_ok(dap, rq);
}

static GRDBG_Result do_next(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  return do_step(dap, rq, GRDBG_STEP_OVER);
}

static GRDBG_Result do_step_in(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  return do_step(dap, rq, GRDBG_STEP_IN);
}

static GRDBG_Result do_step_out(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  return do_step(dap, rq, GRDBG_STEP_OUT);
}

static GRDBG_Result do_pause(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  if (!stopped(dap)) {
    return grdbg_response_error(dap, rq,
        "the program is running: pausing it needs the host to post an interrupt");
  }
  return grdbg_response_ok(dap, rq);
}

static GRDBG_Result do_terminate(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  GRDBG_Result r = grdbg_response_ok(dap, rq);
  if (r == GRDBG_OK) {
    dap->proceed = (int)GRDBG_SERVE_TERMINATE + 1;
  }
  return r;
}

static GRDBG_Result do_disconnect(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  bool terminate = false;
  (void)arg_bool(rq, "terminateDebuggee", &terminate);
  GRDBG_Result r = grdbg_response_ok(dap, rq);
  if (r != GRDBG_OK) {
    return r;
  }
  dap->closed = true;
  if (terminate) {
    dap->proceed = (int)GRDBG_SERVE_TERMINATE + 1;
  } else {
    (void)grdbg_debugger_disarm(dap->debugger);
    dap->proceed = (int)GRDBG_SERVE_DETACH + 1;
  }
  return GRDBG_OK;
}

/* ---- dispatch ----------------------------------------------------------- */

static const struct {
  const char * name;
  Handler handler;
} requests[] = {
    {"initialize", do_initialize},
    {"launch", do_accept},
    {"attach", do_accept},
    {"setBreakpoints", do_set_breakpoints},
    {"setExceptionBreakpoints", do_set_exception_breakpoints},
    {"configurationDone", do_configuration_done},
    {"threads", do_threads},
    {"stackTrace", do_stack_trace},
    {"scopes", do_scopes},
    {"variables", do_variables},
    {"evaluate", do_evaluate},
    {"continue", do_continue},
    {"next", do_next},
    {"stepIn", do_step_in},
    {"stepOut", do_step_out},
    {"pause", do_pause},
    {"terminate", do_terminate},
    {"disconnect", do_disconnect},
};

static GRDBG_Result unsupported(GRDBG_Dap * dap, const GRDBG_Request * rq) {
  static const char prefix[] = "unsupported request: ";
  size_t size = sizeof prefix + rq->command_length;
  char * message = dap->allocator->malloc_fn(dap->allocator->ctx, size);
  if (message == NULL) {
    return grdbg_response_error(dap, rq, "unsupported request");
  }
  memcpy(message, prefix, sizeof prefix - 1);
  memcpy(message + sizeof prefix - 1, rq->command, rq->command_length);
  message[size - 1] = '\0';
  GRDBG_Result r = grdbg_response_error(dap, rq, message);
  dap->allocator->free_fn(dap->allocator->ctx, message);
  return r;
}

GRDBG_Result grdbg_dap_handle(GRDBG_Dap * dap, const GTEXT_JSON_Value * root) {
  if (gtext_json_typeof(root) != GTEXT_JSON_OBJECT) {
    return GRDBG_OK; /* nothing to answer to */
  }
  GRDBG_Request rq;
  rq.command = "";
  rq.command_length = 0;
  rq.arguments = NULL;
  rq.seq = 0;

  /* What a request is, as far as the bytes say: a string `command` makes an
   * object one, and a `seq` it can be answered by. An object with neither has
   * nothing to answer to and is dropped; an object with a `seq` but no
   * command gets an error response; one with a command but no readable `seq`
   * is answered with request_seq 0, because it is plainly a request and its
   * client should hear that it was not understood. */
  int64_t seq;
  const GTEXT_JSON_Value * seq_value = gtext_json_object_get(root, "seq", 3);
  bool has_seq = seq_value != NULL && gtext_json_get_i64(seq_value, &seq) == GTEXT_JSON_OK;
  const GTEXT_JSON_Value * command = gtext_json_object_get(root, "command", 7);
  bool has_command = command != NULL &&
      gtext_json_get_string(command, &rq.command, &rq.command_length) == GTEXT_JSON_OK;
  if (!has_command) {
    rq.command = "";
    rq.command_length = 0;
  }
  if (!has_seq && !has_command) {
    return GRDBG_OK;
  }
  rq.seq = has_seq ? seq : 0;

  /* An absent `type` is taken as a request; a present one must say so. */
  const GTEXT_JSON_Value * type = gtext_json_object_get(root, "type", 4);
  if (type != NULL) {
    const char * type_text;
    size_t type_length;
    if (gtext_json_get_string(type, &type_text, &type_length) != GTEXT_JSON_OK ||
        type_length != 7 || memcmp(type_text, "request", 7) != 0) {
      return grdbg_response_error(dap, &rq, "not a request");
    }
  }
  if (!has_command) {
    return grdbg_response_error(dap, &rq, "the request has no command");
  }
  const GTEXT_JSON_Value * arguments = gtext_json_object_get(root, "arguments", 9);
  if (arguments != NULL && gtext_json_typeof(arguments) == GTEXT_JSON_OBJECT) {
    rq.arguments = arguments;
  }
  for (size_t i = 0; i < sizeof requests / sizeof requests[0]; i++) {
    size_t n = strlen(requests[i].name);
    if (n == rq.command_length && memcmp(requests[i].name, rq.command, n) == 0) {
      return requests[i].handler(dap, &rq);
    }
  }
  return unsupported(dap, &rq);
}
