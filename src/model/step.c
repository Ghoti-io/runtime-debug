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
 * What the host and an adapter ask of the model between stops: step, continue,
 * pause, disarm, and the start of a stop.
 *
 * None of these resumes the context: only the host calls `resume` (AD-20). They
 * arm or disarm the debugger and forget what the last stop recorded.
 */

#include <ghoti.io/runtime-debug/macros.h>

#include "model_internal.h"

GRDBG_Result grdbg_debugger_step(GRDBG_Debugger * d, GRDBG_StepKind kind) {
  if (d == NULL || !grdbg_owned(d) ||
      (kind != GRDBG_STEP_IN && kind != GRDBG_STEP_OVER &&
          kind != GRDBG_STEP_OUT)) {
    return GRDBG_ERR_INVALID;
  }
  GRDBG_Result r = grdbg_snapshot_ensure(d);
  if (r != GRDBG_OK) {
    return r;
  }
  /* The start of the step, read before anything is forgotten. */
  const char * file = NULL;
  int line = 0;
  if (d->frame_count > 0) {
    file = d->frames[0].frame.location.file;
    line = d->frames[0].frame.location.line;
  }
  size_t depth = d->frame_total;
  r = grdbg_arm_for(d, true);
  if (r != GRDBG_OK) {
    return r;
  }
  d->step = kind;
  d->step_file = file;
  d->step_line = line;
  d->step_depth = depth;
  grdbg_forget_stop(d);
  grdbg_invalidate(d);
  return GRDBG_OK;
}

GRDBG_Result grdbg_debugger_continue(GRDBG_Debugger * d) {
  if (d == NULL || !grdbg_owned(d)) {
    return GRDBG_ERR_INVALID;
  }
  d->step = GRDBG_STEP_NONE;
  d->pause_requested = false;
  grdbg_forget_stop(d);
  grdbg_invalidate(d);
  return grdbg_arm_for(d, grdbg_wants_arming(d));
}

GRDBG_Result grdbg_debugger_request_pause(GRDBG_Debugger * d) {
  if (d == NULL || !grdbg_owned(d)) {
    return GRDBG_ERR_INVALID;
  }
  GRDBG_Result r = grdbg_arm_for(d, true);
  if (r != GRDBG_OK) {
    return r;
  }
  d->pause_requested = true;
  return GRDBG_OK;
}

GRDBG_Result grdbg_debugger_disarm(GRDBG_Debugger * d) {
  if (d == NULL || !grdbg_owned(d)) {
    return GRDBG_ERR_INVALID;
  }
  grdbg_free_breakpoints(d);
  d->step = GRDBG_STEP_NONE;
  d->pause_requested = false;
  grdbg_forget_stop(d);
  grdbg_invalidate(d);
  return grdbg_arm_for(d, false);
}

GRDBG_Result grdbg_debugger_stopped(GRDBG_Debugger * d, GRDBG_Stop * out) {
  if (d == NULL || out == NULL || !grdbg_owned(d) || !grdbg_readable(d)) {
    return GRDBG_ERR_INVALID;
  }
  /* A new stop begins: whatever was recorded is the last stop's. */
  grdbg_invalidate(d);
  /* Whether the debugger voted for this pause is what the pause's keys say,
   * not anything remembered: a host that resumed without telling the debugger
   * must not have the next pause, a budget's, read as the old breakpoint. */
  bool ours = false;
  const char * other = NULL;
  size_t keys = grcore_context_pause_key_count(d->context);
  for (size_t i = 0; i < keys; i++) {
    const GRCORE_Key * key = grcore_context_pause_key(d->context, i);
    if (key == &grdbg_key) {
      ours = true;
    } else if (key != NULL && other == NULL) {
      other = key->name != NULL ? key->name : "unnamed key";
    }
  }
  if (!ours) {
    /* Another key paused the context (a budget, an interrupt). The program is
     * not where the step started, so the step ends here. */
    d->step = GRDBG_STEP_NONE;
    d->pause_requested = false;
    d->stop_reason = GRDBG_STOP_NONE;
    d->hit_count = 0;
    (void)grdbg_arm_for(d, grdbg_wants_arming(d));
  }
  out->reason = ours ? d->stop_reason : GRDBG_STOP_NONE;
  out->hit_ids = d->hits;
  out->hit_count = d->hit_count;
  out->other_key = other;
  return GRDBG_OK;
}
