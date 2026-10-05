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
 * The session: creation, the serve loop and the events the host asks for.
 *
 * There is no I/O thread. Protocol I/O runs on the context's owner thread
 * while the context is stopped, so the adapter never reads guest state from
 * another thread (AD-6, AD-20). The rejected alternatives, a reader thread
 * with a command queue and a debugger that polls the transport at every armed
 * poll, are in documentation/design.md.
 *
 * A framing or I/O error ends the session for good: the debugger is disarmed,
 * so the host can resume the run free, and every later call returns the same
 * error. The client leaving (end of input or a disconnect) is not an error: it
 * is a detach, and later events are quietly not sent.
 */

#include <ghoti.io/runtime-debug/macros.h>

#include "dap_internal.h"

#include <stdio.h>
#include <string.h>

GRDBG_Result grdbg_dap_create(GRDBG_Debugger * debugger,
    const GRDBG_Transport * transport, const GRDBG_Limits * limits,
    GRDBG_Dap ** out_dap) {
  if (debugger == NULL || !grdbg_transport_valid(transport) ||
      out_dap == NULL || transport->read == NULL || transport->write == NULL ||
      !grcore_context_is_owner(grdbg_debugger_context(debugger))) {
    return GRDBG_ERR_INVALID;
  }
  const GRDBG_Allocator * allocator = grdbg_debugger_allocator(debugger);
  GRDBG_Dap * dap = allocator->calloc_fn(allocator->ctx, 1, sizeof *dap);
  if (dap == NULL) {
    return GRDBG_ERR_OOM;
  }
  dap->debugger = debugger;
  /* The copy is this library's own full-size struct: the members the host's
   * size covers, and absent ones zero, so that every member can be read
   * without asking the host's size again. */
  memset(&dap->transport, 0, sizeof dap->transport);
  memcpy(&dap->transport, transport,
      transport->size < sizeof dap->transport ? transport->size
                                              : sizeof dap->transport);
  dap->transport.size = sizeof dap->transport;
  dap->allocator = allocator;
  grdbg_limits_resolve(limits, &dap->limits);
  dap->lines_from_1 = true;
  dap->columns_from_1 = true;
  *out_dap = dap;
  return GRDBG_OK;
}

void grdbg_dap_destroy(GRDBG_Dap * dap) {
  if (dap == NULL) {
    return;
  }
  if (dap->transport.close != NULL) {
    (void)dap->transport.close(dap->transport.user);
  }
  const GRDBG_Allocator * a = dap->allocator;
  a->free_fn(a->ctx, dap->in);
  a->free_fn(a->ctx, dap->refs);
  a->free_fn(a->ctx, dap);
}

/* Ends the session on an error the host must act on. */
static GRDBG_Result fail(GRDBG_Dap * dap, GRDBG_Result why) {
  dap->failure = why;
  dap->closed = true;
  (void)grdbg_debugger_disarm(dap->debugger);
  return why;
}

/* The seq and the command of a request that could not be parsed, read off the
 * text: the first "seq" key followed by an integer, and the first "command"
 * key followed by a string with no escape in it. Anything else leaves the
 * defaults (seq 0, no command). */
static const char * find_key(const char * body, size_t length, const char * key) {
  size_t n = strlen(key);
  for (size_t i = 0; i + n + 2 < length; i++) {
    if (body[i] == '"' && memcmp(body + i + 1, key, n) == 0 && body[i + 1 + n] == '"') {
      size_t j = i + n + 2;
      while (j < length && (body[j] == ' ' || body[j] == '\t')) {
        j++;
      }
      if (j < length && body[j] == ':') {
        j++;
        while (j < length && (body[j] == ' ' || body[j] == '\t')) {
          j++;
        }
        return body + j;
      }
    }
  }
  return NULL;
}

static void scan_request(const char * body, size_t length, int64_t * seq,
    const char ** command, size_t * command_length) {
  const char * at = find_key(body, length, "seq");
  if (at != NULL) {
    const char * end = body + length;
    bool negative = at < end && *at == '-';
    if (negative) {
      at++;
    }
    int64_t value = 0;
    int digits = 0;
    while (at < end && *at >= '0' && *at <= '9' && digits < 18) {
      value = value * 10 + (*at - '0');
      at++;
      digits++;
    }
    if (digits > 0) {
      *seq = negative ? -value : value;
    }
  }
  at = find_key(body, length, "command");
  if (at != NULL && at < body + length && *at == '"') {
    const char * start = at + 1;
    const char * end = start;
    while (end < body + length && *end != '"' && *end != '\\') {
      end++;
    }
    if (end < body + length && *end == '"') {
      *command = start;
      *command_length = (size_t)(end - start);
    }
  }
}

GRDBG_Result grdbg_dap_serve(GRDBG_Dap * dap, GRDBG_ServeResult * out_result) {
  if (dap == NULL || out_result == NULL ||
      !grcore_context_is_owner(grdbg_debugger_context(dap->debugger))) {
    return GRDBG_ERR_INVALID;
  }
  if (dap->failure != GRDBG_OK) {
    return dap->failure;
  }
  if (dap->closed) {
    *out_result = GRDBG_SERVE_DETACH;
    return GRDBG_OK;
  }
  for (;;) {
    dap->proceed = 0;
    const char * body;
    size_t length;
    bool eof = false;
    GRDBG_Result r = grdbg_dap_read_message(dap, &body, &length, &eof);
    if (r != GRDBG_OK) {
      return r == GRDBG_ERR_OOM ? r : fail(dap, r);
    }
    if (eof) {
      /* The client closed the stream: let the program run free. */
      dap->closed = true;
      (void)grdbg_debugger_disarm(dap->debugger);
      *out_result = GRDBG_SERVE_DETACH;
      return GRDBG_OK;
    }
    bool out_of_memory = false;
    GTEXT_JSON_Value * root = grdbg_dap_parse(dap, body, length, &out_of_memory);
    if (root == NULL) {
      if (out_of_memory) {
        /* The request is lost and the session goes on, but its client is
         * waiting for an answer, so it gets one: the message could not be
         * parsed, so its seq and command are read off the text as plainly as
         * can be done, and the answer is built without allocating. */
        int64_t seq = 0;
        const char * command = "";
        size_t command_length = 0;
        scan_request(body, length, &seq, &command, &command_length);
        if (seq != 0 || command_length != 0) {
          /* (A message with neither is not a request and has nothing to be
           * answered, as grdbg_dap_handle decides for one it can parse.) */
          (void)grdbg_response_oom(dap, seq, command, command_length);
        }
        return GRDBG_ERR_OOM;
      }
      continue; /* well framed, but not JSON: nothing to answer to */
    }
    r = grdbg_dap_handle(dap, root);
    grdbg_dap_free_json(root);
    if (r == GRDBG_ERR_OOM && dap->proceed != 0) {
      r = GRDBG_OK; /* the model has moved on, so the host must; the response is lost */
    }
    if (r == GRDBG_ERR_OOM) {
      return r;
    }
    if (r != GRDBG_OK) {
      return fail(dap, r);
    }
    if (dap->proceed != 0) {
      *out_result = (GRDBG_ServeResult)(dap->proceed - 1);
      return GRDBG_OK;
    }
  }
}

GRDBG_Result grdbg_dap_notify_stopped(GRDBG_Dap * dap) {
  if (dap == NULL ||
      !grcore_context_is_owner(grdbg_debugger_context(dap->debugger))) {
    return GRDBG_ERR_INVALID;
  }
  GRDBG_Stop stop;
  GRDBG_Result r = grdbg_debugger_stopped(dap->debugger, &stop);
  if (r != GRDBG_OK) {
    return r;
  }
  if (dap->failure != GRDBG_OK) {
    return dap->failure;
  }
  if (dap->closed) {
    return GRDBG_OK;
  }
  GRDBG_Out out;
  r = grdbg_event_begin(dap, "stopped", true, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  char description[96];
  const char * reason = "pause";
  description[0] = '\0';
  switch (stop.reason) {
    case GRDBG_STOP_BREAKPOINT:
      reason = "breakpoint";
      break;
    case GRDBG_STOP_STEP:
      reason = "step";
      break;
    case GRDBG_STOP_PAUSE:
      break;
    case GRDBG_STOP_NONE:
      /* Another key paused the context. Say which, by the name it gave. */
      snprintf(description, sizeof description, "paused by %s",
          stop.other_key != NULL ? stop.other_key : "the host");
      break;
  }
  grdbg_out_key(&out, "reason");
  grdbg_out_string(&out, reason);
  if (description[0] != '\0') {
    grdbg_out_key(&out, "description");
    grdbg_out_string(&out, description);
  }
  grdbg_out_key(&out, "threadId");
  grdbg_out_int(&out, 1);
  grdbg_out_key(&out, "allThreadsStopped");
  grdbg_out_bool(&out, true);
  if (stop.reason == GRDBG_STOP_BREAKPOINT) {
    grdbg_out_key(&out, "hitBreakpointIds");
    grdbg_out_arr_begin(&out);
    for (size_t i = 0; i < stop.hit_count; i++) {
      grdbg_out_int(&out, (int64_t)stop.hit_ids[i]);
    }
    grdbg_out_arr_end(&out);
  }
  r = grdbg_event_end(dap, &out);
  if (r != GRDBG_OK && r != GRDBG_ERR_OOM) {
    return fail(dap, r);
  }
  return r;
}

GRDBG_Result grdbg_dap_notify_finished(GRDBG_Dap * dap, int exit_code) {
  if (dap == NULL ||
      !grcore_context_is_owner(grdbg_debugger_context(dap->debugger))) {
    return GRDBG_ERR_INVALID;
  }
  if (dap->failure != GRDBG_OK) {
    return dap->failure;
  }
  if (dap->closed) {
    return GRDBG_OK;
  }
  GRDBG_Out out;
  GRDBG_Result r = grdbg_event_begin(dap, "exited", true, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  grdbg_out_key(&out, "exitCode");
  grdbg_out_int(&out, exit_code);
  r = grdbg_event_end(dap, &out);
  if (r == GRDBG_OK) {
    r = grdbg_event_begin(dap, "terminated", false, &out);
    if (r == GRDBG_OK) {
      r = grdbg_event_end(dap, &out);
    }
  }
  if (r != GRDBG_OK && r != GRDBG_ERR_OOM) {
    return fail(dap, r);
  }
  return r;
}
