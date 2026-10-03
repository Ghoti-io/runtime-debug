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
 * The limits (the debugger's only configuration, AD-13) and the result mapping
 * from runtime-core.
 */

#include <ghoti.io/runtime-debug/macros.h>

#include "model_internal.h"

#define GRDBG_DEFAULT_HEADER_BYTES 1024u
#define GRDBG_DEFAULT_MESSAGE_BYTES (4u * 1024u * 1024u)
#define GRDBG_DEFAULT_JSON_DEPTH 64u
#define GRDBG_DEFAULT_BREAKPOINTS 1024u
#define GRDBG_DEFAULT_FRAMES 1000u
#define GRDBG_DEFAULT_VARIABLES 1000u

void grdbg_limits_default(GRDBG_Limits * limits) {
  if (limits == NULL) {
    return;
  }
  limits->max_header_bytes = GRDBG_DEFAULT_HEADER_BYTES;
  limits->max_message_bytes = GRDBG_DEFAULT_MESSAGE_BYTES;
  limits->max_json_depth = GRDBG_DEFAULT_JSON_DEPTH;
  limits->max_breakpoints = GRDBG_DEFAULT_BREAKPOINTS;
  limits->max_frames = GRDBG_DEFAULT_FRAMES;
  limits->max_variables = GRDBG_DEFAULT_VARIABLES;
}

void grdbg_limits_resolve(const GRDBG_Limits * in, GRDBG_Limits * out) {
  grdbg_limits_default(out);
  if (in == NULL) {
    return;
  }
  if (in->max_header_bytes != 0) {
    out->max_header_bytes = in->max_header_bytes;
  }
  if (in->max_message_bytes != 0) {
    out->max_message_bytes = in->max_message_bytes;
  }
  if (in->max_json_depth != 0) {
    out->max_json_depth = in->max_json_depth;
  }
  if (in->max_breakpoints != 0) {
    out->max_breakpoints = in->max_breakpoints;
  }
  if (in->max_frames != 0) {
    out->max_frames = in->max_frames;
  }
  if (in->max_variables != 0) {
    out->max_variables = in->max_variables;
  }
}

GRDBG_Result grdbg_from_core(GRCORE_Result result) {
  switch (result) {
    case GRCORE_OK:
      return GRDBG_OK;
    case GRCORE_ERR_IO:
      return GRDBG_ERR_IO;
    case GRCORE_ERR_FORMAT:
      return GRDBG_ERR_FORMAT;
    case GRCORE_ERR_UNSUPPORTED:
      return GRDBG_ERR_UNSUPPORTED;
    case GRCORE_ERR_LIMIT:
      return GRDBG_ERR_LIMIT;
    case GRCORE_ERR_CORRUPT:
      return GRDBG_ERR_CORRUPT;
    case GRCORE_ERR_OOM:
      return GRDBG_ERR_OOM;
    case GRCORE_ERR_INVALID:
      return GRDBG_ERR_INVALID;
    default:
      /* ERR_GUEST is the guest's, not the debugger's, and cannot come from a
       * call the debugger makes. */
      return GRDBG_ERR_INTERNAL;
  }
}
