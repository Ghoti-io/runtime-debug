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
 * Result strings.
 *
 * The string table is indexed by the enum. A new result that is not added
 * here falls through to "Unknown error", and the unit test that walks
 * GRDBG_RESULT_COUNT rejects that.
 */

#include <ghoti.io/runtime-debug/macros.h>

#include <ghoti.io/runtime-debug/core.h>

const char * grdbg_result_string(GRDBG_Result result) {
  switch (result) {
    case GRDBG_OK:
      return "No error";
    case GRDBG_ERR_IO:
      return "Input/output error";
    case GRDBG_ERR_FORMAT:
      return "Format not recognised";
    case GRDBG_ERR_UNSUPPORTED:
      return "Unsupported feature";
    case GRDBG_ERR_LIMIT:
      return "Limit exceeded";
    case GRDBG_ERR_CORRUPT:
      return "Corrupt input";
    case GRDBG_ERR_OOM:
      return "Out of memory";
    case GRDBG_ERR_INVALID:
      return "Invalid argument";
    case GRDBG_ERR_INTERNAL:
      return "Internal error";
    case GRDBG_RESULT_COUNT:
      break;
  }
  return "Unknown error";
}
