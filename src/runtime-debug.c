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
 * Library-wide entry points: the version this build reports.
 */

#include <ghoti.io/runtime-debug/macros.h>

#include <ghoti.io/runtime-debug/core.h>
#include <ghoti.io/runtime-debug/libver.h>

const char * grdbg_version_string(void) {
  return GRDBG_VERSION_STRING;
}

unsigned grdbg_version_number(void) {
  return GRDBG_VERSION_NUMBER;
}
