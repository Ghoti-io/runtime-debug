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
 * The Debug Adapter Protocol adapter's private declarations.
 *
 * The adapter translates and holds no debug logic: it maps ids and shapes
 * messages (AD-15). Everything it does to the program is a call on the model.
 */

#ifndef GHOTI_IO_GRDBG_SRC_DAP_DAP_INTERNAL_H
#define GHOTI_IO_GRDBG_SRC_DAP_DAP_INTERNAL_H

#include <ghoti.io/runtime-debug/macros.h>

#include <ghoti.io/runtime-debug/dap.h>

#include "../model/model_internal.h"

#include <ghoti.io/text/json.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Bytes reserved ahead of a message body for its `Content-Length`
 *   header, so the header and the body go out in one write. */
#define GRDBG_PREFIX 48u

/** @brief A growable output buffer on the debugger's allocator. */
typedef struct GRDBG_Buf {
  char * data;
  size_t length;
  size_t capacity;
  const GRDBG_Allocator * allocator;
  bool failed; ///< An append ran out of memory.
} GRDBG_Buf;

/** @brief A variablesReference: which scope of which frame. */
typedef struct GRDBG_Ref {
  size_t frame;
  size_t scope;
} GRDBG_Ref;

struct GRDBG_Dap {
  GRDBG_Debugger * debugger;
  GRDBG_Transport transport;
  const GRDBG_Allocator * allocator;
  GRDBG_Limits limits;

  /* Input: bytes read from the transport and not yet consumed. */
  uint8_t * in;
  size_t in_length;
  size_t in_capacity;
  size_t in_position;
  size_t pending_consume; ///< The message just returned, consumed on the next
                          ///< read.
  bool eof;

  int64_t out_seq; ///< The seq of the last message sent.

  bool lines_from_1;
  bool columns_from_1;

  GRDBG_Ref * refs;
  size_t ref_count;
  size_t ref_capacity;
  uint64_t generation; ///< The model's, when the refs were issued.
  uint64_t frame_base;  ///< Frame ids spent by earlier stops.
  size_t frame_span;    ///< Frames this stop has issued ids for.

  /* Session state. */
  bool closed;           ///< The client left (disconnect, end of input).
  GRDBG_Result failure;  ///< A framing or I/O error ended the session.
  int proceed;           ///< What the request just handled asks of the host:
                         ///< zero for nothing, else GRDBG_ServeResult + 1.
};

/** @brief A message being built. */
typedef struct GRDBG_Out {
  GRDBG_Buf buf;
  GTEXT_JSON_Writer * writer;
  bool failed;
  bool body_open;
  const char * problem; ///< Why a response became an error, if it did.
} GRDBG_Out;

/** @brief The parts of a request a handler needs. */
typedef struct GRDBG_Request {
  int64_t seq;
  const char * command;
  size_t command_length;
  const GTEXT_JSON_Value * arguments; ///< An object, or NULL.
} GRDBG_Request;

/* frame.c: the Content-Length framing. */

/** @brief Reads the next message. On success `*out_body` and `*out_length`
 *   name its bytes, valid until the next call; at end of input `*out_eof` is
 *   set and the body is empty. A broken frame is ::GRDBG_ERR_FORMAT or
 *   ::GRDBG_ERR_LIMIT; a failed transport ::GRDBG_ERR_IO. */
GRDBG_Result grdbg_dap_read_message(GRDBG_Dap * dap, const char ** out_body,
    size_t * out_length, bool * out_eof);

/** @brief Frames and writes a finished message, and frees it. */
GRDBG_Result grdbg_dap_send(GRDBG_Dap * dap, GRDBG_Out * out);

/* json.c: building messages. All the `out_*` writers are sticky: after one
 * fails, the rest do nothing and `out->failed` stays set. */

GRDBG_Result grdbg_out_begin(GRDBG_Dap * dap, GRDBG_Out * out);
void grdbg_out_discard(GRDBG_Out * out);
void grdbg_out_obj_begin(GRDBG_Out * out);
void grdbg_out_obj_end(GRDBG_Out * out);
void grdbg_out_arr_begin(GRDBG_Out * out);
void grdbg_out_arr_end(GRDBG_Out * out);
void grdbg_out_key(GRDBG_Out * out, const char * key);
void grdbg_out_string(GRDBG_Out * out, const char * text);
void grdbg_out_string_n(GRDBG_Out * out, const char * text, size_t length);
void grdbg_out_int(GRDBG_Out * out, int64_t value);
void grdbg_out_bool(GRDBG_Out * out, bool value);

/** @brief Begins a response up to and including `"command"`, plus `"body":{`
 *   when `with_body`. */
GRDBG_Result grdbg_response_begin(GRDBG_Dap * dap, const GRDBG_Request * request,
    bool with_body, GRDBG_Out * out);
/** @brief Ends and sends a response begun above. If the body could not be
 *   built, sends an error response instead. */
GRDBG_Result grdbg_response_end(
    GRDBG_Dap * dap, const GRDBG_Request * request, GRDBG_Out * out);
/** @brief Sends `success:false` with a message. */
GRDBG_Result grdbg_response_error(GRDBG_Dap * dap,
    const GRDBG_Request * request, const char * message);
/** @brief Answers a request with `success:false, "out of memory"` without
 *   allocating, from a buffer on the stack: what is sent when the allocation
 *   that would have built the answer is the one that failed. `command` is
 *   named only if it is made of letters, digits and underscores. A failed
 *   write is the result; success is ::GRDBG_OK, not the out-of-memory the
 *   caller is already reporting. */
GRDBG_Result grdbg_response_oom(GRDBG_Dap * dap, int64_t request_seq,
    const char * command, size_t command_length);
/** @brief Sends `success:true` with no body. */
GRDBG_Result grdbg_response_ok(
    GRDBG_Dap * dap, const GRDBG_Request * request);
/** @brief Begins an event, with `"body":{` when `with_body`. */
GRDBG_Result grdbg_event_begin(
    GRDBG_Dap * dap, const char * event, bool with_body, GRDBG_Out * out);
/** @brief Ends and sends an event. */
GRDBG_Result grdbg_event_end(GRDBG_Dap * dap, GRDBG_Out * out);

/** @brief Reads one request from a parsed message and handles it. Returns a
 *   result only for the session's own failures (I/O, memory); a bad request is
 *   answered, not returned. */
GRDBG_Result grdbg_dap_handle(GRDBG_Dap * dap, const GTEXT_JSON_Value * root);

/** @brief Parses a message body with the session's limits; NULL if it does not
 *   parse, with `*out_oom` set when the reason is memory. */
GTEXT_JSON_Value * grdbg_dap_parse(
    GRDBG_Dap * dap, const char * body, size_t length, bool * out_oom);

/** @brief Frees a parsed message. */
void grdbg_dap_free_json(GTEXT_JSON_Value * value);

#endif /* GHOTI_IO_GRDBG_SRC_DAP_DAP_INTERNAL_H */
