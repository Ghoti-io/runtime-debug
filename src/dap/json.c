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
 * Messages in and out, through `text`'s JSON.
 *
 * Nothing is parsed or escaped by hand. A message is built with the streaming
 * writer into a buffer on the debugger's allocator, with room reserved ahead of
 * the body so the `Content-Length` header can be put in front of it without a
 * copy. Every writer call here is sticky: once one fails the rest do nothing,
 * and the failure is read once, when the message is sent.
 */

#include <ghoti.io/runtime-debug/macros.h>

#include "dap_internal.h"

#include <stdio.h>
#include <string.h>

static int buf_write(void * user, const char * bytes, size_t length) {
  GRDBG_Buf * b = user;
  if (b->failed) {
    return 1;
  }
  if (length > SIZE_MAX - b->length) {
    b->failed = true;
    return 1;
  }
  size_t need = b->length + length;
  if (need > b->capacity) {
    size_t capacity = b->capacity == 0 ? 512 : b->capacity;
    while (capacity < need) {
      if (capacity > SIZE_MAX / 2) {
        b->failed = true;
        return 1;
      }
      capacity *= 2;
    }
    char * grown = b->allocator->realloc_fn(b->allocator->ctx, b->data, capacity);
    if (grown == NULL) {
      b->failed = true;
      return 1;
    }
    b->data = grown;
    b->capacity = capacity;
  }
  memcpy(b->data + b->length, bytes, length);
  b->length = need;
  return 0;
}

GRDBG_Result grdbg_out_begin(GRDBG_Dap * dap, GRDBG_Out * out) {
  memset(out, 0, sizeof *out);
  out->buf.allocator = dap->allocator;
  static const char reserved[GRDBG_PREFIX] = {0};
  if (buf_write(&out->buf, reserved, GRDBG_PREFIX) != 0) {
    grdbg_out_discard(out);
    return GRDBG_ERR_OOM;
  }
  GTEXT_JSON_Write_Options options = gtext_json_write_options_default();
  options.allocator = dap->allocator;
  GTEXT_JSON_Sink sink = {buf_write, &out->buf};
  out->writer = gtext_json_writer_new(sink, &options);
  if (out->writer == NULL) {
    grdbg_out_discard(out);
    return GRDBG_ERR_OOM;
  }
  return GRDBG_OK;
}

void grdbg_out_discard(GRDBG_Out * out) {
  if (out->writer != NULL) {
    gtext_json_writer_free(out->writer);
    out->writer = NULL;
  }
  if (out->buf.data != NULL) {
    out->buf.allocator->free_fn(out->buf.allocator->ctx, out->buf.data);
    out->buf.data = NULL;
  }
  out->buf.length = 0;
  out->buf.capacity = 0;
}

#define STEP(out, call)                                                        \
  do {                                                                         \
    if (!(out)->failed && (call) != GTEXT_JSON_OK) {                           \
      (out)->failed = true;                                                    \
    }                                                                          \
  } while (0)

void grdbg_out_obj_begin(GRDBG_Out * out) {
  STEP(out, gtext_json_writer_object_begin(out->writer));
}

void grdbg_out_obj_end(GRDBG_Out * out) {
  STEP(out, gtext_json_writer_object_end(out->writer));
}

void grdbg_out_arr_begin(GRDBG_Out * out) {
  STEP(out, gtext_json_writer_array_begin(out->writer));
}

void grdbg_out_arr_end(GRDBG_Out * out) {
  STEP(out, gtext_json_writer_array_end(out->writer));
}

void grdbg_out_key(GRDBG_Out * out, const char * key) {
  STEP(out, gtext_json_writer_key(out->writer, key, strlen(key)));
}

/* The length of the well-formed UTF-8 sequence at `s` (of `n` bytes left), or
 * zero if what is there is not one: a stray continuation byte, an overlong
 * form, a surrogate, a code point past U+10FFFF, or a sequence cut short. */
static size_t utf8_sequence(const unsigned char * s, size_t n) {
  if (s[0] < 0x80) {
    return 1;
  }
  size_t want;
  uint32_t code;
  uint32_t least;
  if (s[0] >= 0xC2 && s[0] <= 0xDF) {
    want = 2;
    code = s[0] & 0x1Fu;
    least = 0x80;
  } else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
    want = 3;
    code = s[0] & 0x0Fu;
    least = 0x800;
  } else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
    want = 4;
    code = s[0] & 0x07u;
    least = 0x10000;
  } else {
    return 0;
  }
  if (n < want) {
    return 0;
  }
  for (size_t i = 1; i < want; i++) {
    if ((s[i] & 0xC0u) != 0x80u) {
      return 0;
    }
    code = (code << 6) | (s[i] & 0x3Fu);
  }
  if (code < least || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
    return 0;
  }
  return want;
}

/* A string from the engine can hold any bytes: a file name or an inspected
 * value is the engine's to choose, and JSON cannot carry what is not UTF-8. The
 * writer refuses it, which would turn a whole response into an error over one
 * name. So the bytes that are not UTF-8 go out as U+FFFD, and the rest as they
 * are. */
void grdbg_out_string_n(GRDBG_Out * out, const char * text, size_t length) {
  const unsigned char * s = (const unsigned char *)text;
  size_t bad = 0;
  for (size_t i = 0; i < length;) {
    size_t n = utf8_sequence(s + i, length - i);
    if (n == 0) {
      bad++;
      i++;
    } else {
      i += n;
    }
  }
  if (bad == 0) {
    STEP(out, gtext_json_writer_string(out->writer, text, length));
    return;
  }
  if (out->failed) {
    return;
  }
  char * clean = out->buf.allocator->malloc_fn(
      out->buf.allocator->ctx, length + bad * 2 + 1);
  if (clean == NULL) {
    out->failed = true;
    return;
  }
  size_t used = 0;
  for (size_t i = 0; i < length;) {
    size_t n = utf8_sequence(s + i, length - i);
    if (n == 0) {
      memcpy(clean + used, "\xEF\xBF\xBD", 3);
      used += 3;
      i++;
    } else {
      memcpy(clean + used, s + i, n);
      used += n;
      i += n;
    }
  }
  STEP(out, gtext_json_writer_string(out->writer, clean, used));
  out->buf.allocator->free_fn(out->buf.allocator->ctx, clean);
}

void grdbg_out_string(GRDBG_Out * out, const char * text) {
  grdbg_out_string_n(out, text, strlen(text));
}

void grdbg_out_int(GRDBG_Out * out, int64_t value) {
  STEP(out, gtext_json_writer_number_i64(out->writer, (long long)value));
}

void grdbg_out_bool(GRDBG_Out * out, bool value) {
  STEP(out, gtext_json_writer_bool(out->writer, value));
}

/* The next seq. It is only spent when the message is sent. */
static int64_t next_seq(const GRDBG_Dap * dap) {
  return dap->out_seq + 1;
}

GRDBG_Result grdbg_response_begin(GRDBG_Dap * dap, const GRDBG_Request * request,
    bool with_body, GRDBG_Out * out) {
  GRDBG_Result r = grdbg_out_begin(dap, out);
  if (r != GRDBG_OK) {
    return r;
  }
  grdbg_out_obj_begin(out);
  grdbg_out_key(out, "seq");
  grdbg_out_int(out, next_seq(dap));
  grdbg_out_key(out, "type");
  grdbg_out_string(out, "response");
  grdbg_out_key(out, "request_seq");
  grdbg_out_int(out, request->seq);
  grdbg_out_key(out, "success");
  grdbg_out_bool(out, true);
  grdbg_out_key(out, "command");
  grdbg_out_string_n(out, request->command, request->command_length);
  if (with_body) {
    grdbg_out_key(out, "body");
    grdbg_out_obj_begin(out);
    out->body_open = true;
  }
  return GRDBG_OK;
}

GRDBG_Result grdbg_response_error(GRDBG_Dap * dap,
    const GRDBG_Request * request, const char * message) {
  GRDBG_Out out;
  GRDBG_Result r = grdbg_out_begin(dap, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  grdbg_out_obj_begin(&out);
  grdbg_out_key(&out, "seq");
  grdbg_out_int(&out, next_seq(dap));
  grdbg_out_key(&out, "type");
  grdbg_out_string(&out, "response");
  grdbg_out_key(&out, "request_seq");
  grdbg_out_int(&out, request->seq);
  grdbg_out_key(&out, "success");
  grdbg_out_bool(&out, false);
  grdbg_out_key(&out, "command");
  grdbg_out_string_n(&out, request->command, request->command_length);
  grdbg_out_key(&out, "message");
  grdbg_out_string(&out, message);
  grdbg_out_obj_end(&out);
  return grdbg_dap_send(dap, &out);
}

GRDBG_Result grdbg_response_oom(GRDBG_Dap * dap, int64_t request_seq,
    const char * command, size_t command_length) {
  /* No allocation: the message is built in a buffer on the stack, because the
   * session's memory is what has just run out. The command is copied only if
   * it is plain, so that nothing needs escaping. */
  char name[64];
  size_t n = 0;
  for (size_t i = 0; i < command_length && n + 1 < sizeof name; i++) {
    char c = command[i];
    bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_';
    if (!plain) {
      n = 0;
      break;
    }
    name[n++] = c;
  }
  name[n] = '\0';
  char frame[GRDBG_PREFIX + 256];
  char * body = frame + GRDBG_PREFIX;
  int length = snprintf(body, sizeof frame - GRDBG_PREFIX,
      "{\"seq\":%lld,\"type\":\"response\",\"request_seq\":%lld,"
      "\"success\":false,\"command\":\"%s\",\"message\":\"out of memory\"}",
      (long long)next_seq(dap), (long long)request_seq, name);
  if (length < 0 || (size_t)length >= sizeof frame - GRDBG_PREFIX) {
    return GRDBG_ERR_INTERNAL;
  }
  char header[GRDBG_PREFIX];
  int h = snprintf(header, sizeof header, "Content-Length: %d\r\n\r\n", length);
  if (h < 0 || (size_t)h >= sizeof header) {
    return GRDBG_ERR_INTERNAL;
  }
  char * start = body - h;
  memcpy(start, header, (size_t)h);
  GRDBG_Result r = dap->transport.write(dap->transport.user, start, (size_t)h + (size_t)length);
  if (r != GRDBG_OK) {
    return r == GRDBG_ERR_OOM ? r : GRDBG_ERR_IO;
  }
  dap->out_seq++;
  return GRDBG_OK;
}

GRDBG_Result grdbg_response_end(
    GRDBG_Dap * dap, const GRDBG_Request * request, GRDBG_Out * out) {
  if (out->body_open) {
    grdbg_out_obj_end(out);
  }
  grdbg_out_obj_end(out);
  if (out->failed || out->buf.failed) {
    /* The body could not be built: say so, in the one way that needs no body.
     * If that cannot be built either, the session's memory is gone and the
     * caller gets the out-of-memory result. */
    const char * problem =
        out->problem != NULL ? out->problem : "out of memory";
    grdbg_out_discard(out);
    return grdbg_response_error(dap, request, problem);
  }
  return grdbg_dap_send(dap, out);
}

GRDBG_Result grdbg_response_ok(
    GRDBG_Dap * dap, const GRDBG_Request * request) {
  GRDBG_Out out;
  GRDBG_Result r = grdbg_response_begin(dap, request, false, &out);
  if (r != GRDBG_OK) {
    return r;
  }
  return grdbg_response_end(dap, request, &out);
}

GRDBG_Result grdbg_event_begin(
    GRDBG_Dap * dap, const char * event, bool with_body, GRDBG_Out * out) {
  GRDBG_Result r = grdbg_out_begin(dap, out);
  if (r != GRDBG_OK) {
    return r;
  }
  grdbg_out_obj_begin(out);
  grdbg_out_key(out, "seq");
  grdbg_out_int(out, next_seq(dap));
  grdbg_out_key(out, "type");
  grdbg_out_string(out, "event");
  grdbg_out_key(out, "event");
  grdbg_out_string(out, event);
  if (with_body) {
    grdbg_out_key(out, "body");
    grdbg_out_obj_begin(out);
    out->body_open = true;
  }
  return GRDBG_OK;
}

GRDBG_Result grdbg_event_end(GRDBG_Dap * dap, GRDBG_Out * out) {
  if (out->body_open) {
    grdbg_out_obj_end(out);
  }
  grdbg_out_obj_end(out);
  return grdbg_dap_send(dap, out);
}

GTEXT_JSON_Value * grdbg_dap_parse(
    GRDBG_Dap * dap, const char * body, size_t length, bool * out_oom) {
  GTEXT_JSON_Parse_Options options = gtext_json_parse_options_default();
  options.allocator = dap->allocator;
  options.max_depth = dap->limits.max_json_depth;
  options.max_total_bytes = dap->limits.max_message_bytes;
  options.parse_int64 = true;
  options.parse_uint64 = false;
  options.preserve_number_lexeme = true;
  GTEXT_JSON_Error error;
  memset(&error, 0, sizeof error);
  GTEXT_JSON_Value * value = gtext_json_parse(body, length, &options, &error);
  /* A parse that ran out of memory is not a message that was not JSON: the
   * first is the session's trouble and the second is the client's. */
  *out_oom = value == NULL && error.code == GTEXT_JSON_E_OOM;
  gtext_json_error_free(&error);
  return value;
}

void grdbg_dap_free_json(GTEXT_JSON_Value * value) {
  gtext_json_free(value);
}
