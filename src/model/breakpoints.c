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
 * Line breakpoints, per context, keyed by (source string, line).
 *
 * A set replaces everything a source had (the protocol's semantics), assigns
 * fresh ids, and either happens whole or not at all: the new table is built
 * beside the old one, the debugger is armed if it must be, and only then is
 * anything swapped, so a failed allocation or a refused post changes nothing.
 */

#include <ghoti.io/runtime-debug/macros.h>

#include "model_internal.h"

#include <string.h>

static char * copy_string(GRDBG_Debugger * d, const char * text) {
  size_t length = strlen(text);
  char * copy = grdbg_alloc(d, length + 1);
  if (copy != NULL) {
    memcpy(copy, text, length + 1);
  }
  return copy;
}

void grdbg_free_breakpoints(GRDBG_Debugger * d) {
  for (size_t i = 0; i < d->bp_count; i++) {
    grdbg_free(d, d->bps[i].source);
  }
  d->bp_count = 0;
}

size_t grdbg_debugger_breakpoint_count(const GRDBG_Debugger * debugger) {
  return grdbg_owned(debugger) ? debugger->bp_count : 0;
}

GRDBG_Result grdbg_debugger_set_breakpoints(GRDBG_Debugger * d,
    const char * source, const int * lines, size_t count, uint64_t * out_ids) {
  if (d == NULL || source == NULL || (count > 0 && (lines == NULL ||
                                                       out_ids == NULL)) ||
      !grdbg_owned(d)) {
    return GRDBG_ERR_INVALID;
  }
  for (size_t i = 0; i < count; i++) {
    if (lines[i] < 1) {
      return GRDBG_ERR_INVALID;
    }
  }
  size_t keep = 0;
  for (size_t i = 0; i < d->bp_count; i++) {
    if (strcmp(d->bps[i].source, source) != 0) {
      keep++;
    }
  }
  if (count > d->limits.max_breakpoints ||
      keep > d->limits.max_breakpoints - count) {
    return GRDBG_ERR_LIMIT;
  }
  size_t total = keep + count;

  /* Build the new table beside the old one. */
  GRDBG_Breakpoint * table = NULL;
  if (total > 0) {
    table = grdbg_alloc(d, total * sizeof *table);
    if (table == NULL) {
      return GRDBG_ERR_OOM;
    }
  }
  uint64_t * hits = NULL;
  if (total > d->hit_capacity) {
    hits = grdbg_alloc(d, total * sizeof *hits);
    if (hits == NULL) {
      grdbg_free(d, table);
      return GRDBG_ERR_OOM;
    }
  }
  size_t n = 0;
  for (size_t i = 0; i < d->bp_count; i++) {
    if (strcmp(d->bps[i].source, source) != 0) {
      table[n++] = d->bps[i]; /* moved: the old table gives it up on commit */
    }
  }
  size_t made = 0;
  for (; made < count; made++) {
    char * copy = copy_string(d, source);
    if (copy == NULL) {
      break;
    }
    table[n + made].id = d->next_id + made;
    table[n + made].source = copy;
    table[n + made].line = lines[made];
  }
  if (made < count) {
    for (size_t i = 0; i < made; i++) {
      grdbg_free(d, table[n + i].source);
    }
    grdbg_free(d, table);
    grdbg_free(d, hits);
    return GRDBG_ERR_OOM;
  }

  /* Arm before committing: posting can fail, and then nothing has changed. */
  bool want = total > 0 || d->step != GRDBG_STEP_NONE || d->pause_requested;
  GRDBG_Result armed = grdbg_arm_for(d, want);
  if (armed != GRDBG_OK) {
    for (size_t i = 0; i < count; i++) {
      grdbg_free(d, table[n + i].source);
    }
    grdbg_free(d, table);
    grdbg_free(d, hits);
    return armed;
  }

  /* Commit. The old entries of this source are released; the rest moved. */
  for (size_t i = 0; i < d->bp_count; i++) {
    if (strcmp(d->bps[i].source, source) == 0) {
      grdbg_free(d, d->bps[i].source);
    }
  }
  grdbg_free(d, d->bps);
  d->bps = table;
  d->bp_count = total;
  d->bp_capacity = total;
  if (hits != NULL) {
    grdbg_free(d, d->hits);
    d->hits = hits;
    d->hit_capacity = total;
  }
  for (size_t i = 0; i < count; i++) {
    out_ids[i] = table[n + i].id;
  }
  d->next_id += count;
  return GRDBG_OK;
}
