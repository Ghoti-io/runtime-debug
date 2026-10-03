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
 * @file core.h
 * @stability stable
 *
 * Result codes and the version this build reports.
 */

#ifndef GHOTI_IO_GRDBG_CORE_H
#define GHOTI_IO_GRDBG_CORE_H

#include <ghoti.io/runtime-debug/allocator.h>
#include <ghoti.io/runtime-debug/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of an operation.
 *
 * Zero is success.  ::GRDBG_RESULT_COUNT closes the enum so a test can check
 * the string table is complete.
 *
 * This is the suite's fixed vocabulary, numbered as in every other library
 * and as in runtime-core's ::GRCORE_Result up to ::GRDBG_ERR_INTERNAL. It has
 * no ERR_GUEST: a debugger has no guest of its own. A framing error in the
 * debug protocol is ::GRDBG_ERR_FORMAT (the bytes are not a message) or
 * ::GRDBG_ERR_LIMIT (a stated cap in ::GRDBG_Limits was exceeded), and a
 * transport that failed is ::GRDBG_ERR_IO.
 */
typedef enum {
  GRDBG_OK = 0,          ///< The operation succeeded.
  GRDBG_ERR_IO,          ///< A read, write, or seek failed.
  GRDBG_ERR_FORMAT,      ///< Well-formed bytes, but not a format handled here.
  GRDBG_ERR_UNSUPPORTED, ///< The format is known and the feature is not
                          ///< implemented.
  GRDBG_ERR_LIMIT,       ///< A stated cap was exceeded.
  GRDBG_ERR_CORRUPT,     ///< The bytes are not a valid encoding of this
                          ///< format.
  GRDBG_ERR_OOM,         ///< The allocator returned NULL.
  GRDBG_ERR_INVALID,     ///< A caller-supplied argument is wrong.
  GRDBG_ERR_INTERNAL,    ///< The library's own invariant failed.
  GRDBG_RESULT_COUNT
} GRDBG_Result;

/**
 * @brief Static description of a result code.
 *
 * The string is never NULL, never allocated, and never contains caller data.
 *
 * @param result The result code, including values outside the enum.
 * @return A static string.
 */
GRDBG_API const char * grdbg_result_string(GRDBG_Result result);

/**
 * @brief This build's version, as the string the Makefile generated.
 *
 * @return A static string, never NULL.  "0.0.0", "0.0.0-dev" when BRANCH
 *   was overridden, and with "-debug" appended for a BUILD=debug build
 *   ("0.0.0-debug", "0.0.0-dev-debug").
 */
GRDBG_API const char * grdbg_version_string(void);

/**
 * @brief This build's version, packed as ::GRDBG_MAKE_VERSION packs it.
 *
 * @return `(major << 16) | (minor << 8) | patch`.
 */
GRDBG_API unsigned grdbg_version_number(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRDBG_CORE_H */
