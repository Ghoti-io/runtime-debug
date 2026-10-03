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
 * @file limits.h
 * @stability stable
 *
 * The caps on what the debugger and its protocol adapter will accept or send.
 *
 * The debugger has no options object. Its one piece of configuration is this
 * struct, because the Debug Adapter Protocol reader is a parser of untrusted
 * input and CONVENTIONS.md section 5 gives every parser a `Limits` with a
 * `_default()` (AD-13): a cap is an input to a pure function of the bytes,
 * not a property of a long-lived attachment that will keep growing fields.
 */

#ifndef GHOTI_IO_GRDBG_LIMITS_H
#define GHOTI_IO_GRDBG_LIMITS_H

#include <ghoti.io/runtime-debug/macros.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The caps. A field of zero means that field's default, and a `NULL`
 *   `GRDBG_Limits *` anywhere in this library means all defaults.
 *
 * `max_header_bytes` is held between 32 bytes and 64 KiB (a header block must
 * hold a `Content-Length`) and `max_message_bytes` between 1 byte and 1 GiB (a
 * cap past that is none, and could not be added to a header without wrapping).
 *
 * A cap that is exceeded is reported, never worked around by truncating: a
 * breakpoint set past `max_breakpoints` is ::GRDBG_ERR_LIMIT and changes
 * nothing, and a framing cap ends the session with ::GRDBG_ERR_LIMIT. The one
 * exception is a list a protocol lets the client page through (frames and
 * variables), where the response is cut at the cap and says how many there
 * were in all.
 */
typedef struct GRDBG_Limits {
  size_t max_header_bytes;  ///< The header block of a message, terminator
                            ///< included. Default 1024.
  size_t max_message_bytes; ///< The body of a message (`Content-Length`).
                            ///< Default 4 MiB.
  size_t max_json_depth;    ///< Nesting depth of a message's JSON. Default 64.
  size_t max_breakpoints;   ///< Breakpoints held by one debugger. Default 1024.
  size_t max_frames;        ///< Frames a stop records and a `stackTrace`
                            ///< returns; the rest are counted. Default 1000.
  size_t max_variables;     ///< Variables one `variables` response returns.
                            ///< Default 1000.
} GRDBG_Limits;

/**
 * @brief Fills `limits` with the defaults.
 *
 * @param limits The struct to initialise; NULL is ignored.
 */
GRDBG_API void grdbg_limits_default(GRDBG_Limits * limits);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRDBG_LIMITS_H */
