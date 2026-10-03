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
 * The framing of the Debug Adapter Protocol: `Content-Length: N\r\n\r\n` and
 * then N bytes.
 *
 * Reading is buffered, because a transport may hand over half a message or
 * three, and every cap is checked before the bytes it governs are read: a
 * header block past `max_header_bytes` and a length past `max_message_bytes`
 * end the session without reading further. Once the framing is lost it cannot
 * be recovered, because there is no marker to resynchronise on, so a framing
 * error ends the session rather than guessing where the next message begins.
 */

#include <ghoti.io/runtime-debug/macros.h>

#include "dap_internal.h"

#include <stdio.h>
#include <string.h>

#define READ_CHUNK 4096u

/* Makes room for at least `extra` more bytes after the data, moving the data
 * down first so the buffer does not grow for want of a consumed prefix. */
static GRDBG_Result make_room(GRDBG_Dap * dap, size_t extra) {
  if (dap->in_position > 0) {
    size_t left = dap->in_length - dap->in_position;
    if (left > 0) {
      memmove(dap->in, dap->in + dap->in_position, left);
    }
    dap->in_length = left;
    dap->in_position = 0;
  }
  if (extra > SIZE_MAX - dap->in_length) {
    return GRDBG_ERR_LIMIT;
  }
  size_t need = dap->in_length + extra;
  if (need <= dap->in_capacity) {
    return GRDBG_OK;
  }
  size_t capacity = dap->in_capacity == 0 ? READ_CHUNK : dap->in_capacity;
  while (capacity < need) {
    if (capacity > SIZE_MAX / 2) {
      return GRDBG_ERR_LIMIT;
    }
    capacity *= 2;
  }
  uint8_t * grown =
      dap->allocator->realloc_fn(dap->allocator->ctx, dap->in, capacity);
  if (grown == NULL) {
    return GRDBG_ERR_OOM;
  }
  dap->in = grown;
  dap->in_capacity = capacity;
  return GRDBG_OK;
}

/* Reads once from the transport, at most `want` bytes. Sets eof on zero. */
static GRDBG_Result fill(GRDBG_Dap * dap, size_t want) {
  GRDBG_Result r = make_room(dap, want);
  if (r != GRDBG_OK) {
    return r;
  }
  size_t n = 0;
  r = dap->transport.read(dap->transport.user, dap->in + dap->in_length,
      want < dap->in_capacity - dap->in_length ? want
                                               : dap->in_capacity - dap->in_length,
      &n);
  if (r != GRDBG_OK) {
    return r == GRDBG_ERR_OOM ? r : GRDBG_ERR_IO;
  }
  if (n > dap->in_capacity - dap->in_length) {
    return GRDBG_ERR_IO; /* a transport that reports more than it was given */
  }
  if (n == 0) {
    dap->eof = true;
  }
  dap->in_length += n;
  return GRDBG_OK;
}

/* The offset in [data, data + length) one past the first CRLFCRLF, or zero. */
static size_t find_terminator(const uint8_t * data, size_t length) {
  for (size_t i = 0; i + 4 <= length; i++) {
    if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' &&
        data[i + 3] == '\n') {
      return i + 4;
    }
  }
  return 0;
}

static bool name_is_content_length(const char * line, size_t name_length) {
  static const char wanted[] = "content-length";
  if (name_length != sizeof wanted - 1) {
    return false;
  }
  for (size_t i = 0; i < name_length; i++) {
    char c = line[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    if (c != wanted[i]) {
      return false;
    }
  }
  return true;
}

/* The Content-Length in a header block, or the reason there is none. Only
 * digits are a length: a sign, a space inside, a suffix or an empty value is
 * not one, and neither is a repeated header, which one of two lengths would
 * have to win without anyone knowing which. */
static GRDBG_Result parse_length(const char * block, size_t length,
    size_t max_message, size_t * out_length) {
  bool seen = false;
  size_t value = 0;
  size_t i = 0;
  while (i < length) {
    size_t end = i;
    while (end < length && !(block[end] == '\r' && end + 1 < length &&
                               block[end + 1] == '\n')) {
      end++;
    }
    if (end > length) {
      end = length;
    }
    size_t line_length = end - i;
    const char * line = block + i;
    const char * colon = memchr(line, ':', line_length);
    if (line_length > 0) {
      if (colon == NULL) {
        return GRDBG_ERR_FORMAT;
      }
      if (name_is_content_length(line, (size_t)(colon - line))) {
        if (seen) {
          return GRDBG_ERR_FORMAT;
        }
        seen = true;
        const char * v = colon + 1;
        const char * stop = line + line_length;
        while (v < stop && (*v == ' ' || *v == '\t')) {
          v++;
        }
        while (stop > v && (stop[-1] == ' ' || stop[-1] == '\t')) {
          stop--;
        }
        if (v == stop) {
          return GRDBG_ERR_FORMAT;
        }
        value = 0;
        for (; v < stop; v++) {
          if (*v < '0' || *v > '9') {
            return GRDBG_ERR_FORMAT;
          }
          size_t digit = (size_t)(*v - '0');
          if (value > (SIZE_MAX - digit) / 10) {
            return GRDBG_ERR_LIMIT;
          }
          value = value * 10 + digit;
        }
      }
    }
    i = end + 2;
  }
  if (!seen) {
    return GRDBG_ERR_FORMAT;
  }
  if (value > max_message) {
    return GRDBG_ERR_LIMIT;
  }
  *out_length = value;
  return GRDBG_OK;
}

GRDBG_Result grdbg_dap_read_message(GRDBG_Dap * dap, const char ** out_body,
    size_t * out_length, bool * out_eof) {
  /* The message returned last time is done with. */
  dap->in_position += dap->pending_consume;
  dap->pending_consume = 0;

  size_t header = 0;
  for (;;) {
    size_t available = dap->in_length - dap->in_position;
    /* No buffer yet is no bytes, and a null pointer plus zero is not a pointer
     * arithmetic C allows. */
    header = available == 0 ? 0 : find_terminator(dap->in + dap->in_position, available);
    if (header != 0) {
      break;
    }
    if (available >= dap->limits.max_header_bytes) {
      return GRDBG_ERR_LIMIT; /* no terminator inside the allowance */
    }
    if (dap->eof) {
      *out_eof = true;
      return GRDBG_OK;
    }
    /* Never past the header allowance, so a stream that will not end its
     * header costs the allowance and no more. */
    size_t allowance = dap->limits.max_header_bytes - available;
    GRDBG_Result r = fill(dap, allowance < READ_CHUNK ? allowance : READ_CHUNK);
    if (r != GRDBG_OK) {
      return r;
    }
  }
  if (header > dap->limits.max_header_bytes) {
    return GRDBG_ERR_LIMIT;
  }
  size_t body_length = 0;
  GRDBG_Result r = parse_length(
      (const char *)dap->in + dap->in_position, header - 4,
      dap->limits.max_message_bytes, &body_length);
  if (r != GRDBG_OK) {
    return r;
  }
  /* Everything needed is `header + body_length` bytes from the position. */
  while (dap->in_length - dap->in_position < header + body_length) {
    if (dap->eof) {
      *out_eof = true; /* the client left in the middle of a message */
      return GRDBG_OK;
    }
    size_t missing = header + body_length - (dap->in_length - dap->in_position);
    r = fill(dap, missing);
    if (r != GRDBG_OK) {
      return r;
    }
  }
  *out_body = (const char *)dap->in + dap->in_position + header;
  *out_length = body_length;
  dap->pending_consume = header + body_length;
  *out_eof = false;
  return GRDBG_OK;
}

GRDBG_Result grdbg_dap_send(GRDBG_Dap * dap, GRDBG_Out * out) {
  GRDBG_Result result = GRDBG_OK;
  if (out->failed || out->buf.failed ||
      gtext_json_writer_finish(out->writer, NULL) != GTEXT_JSON_OK) {
    result = GRDBG_ERR_OOM;
  } else {
    size_t body = out->buf.length - GRDBG_PREFIX;
    char header[GRDBG_PREFIX];
    int n = snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", body);
    if (n < 0 || (size_t)n >= sizeof header) {
      result = GRDBG_ERR_INTERNAL;
    } else {
      char * start = out->buf.data + GRDBG_PREFIX - (size_t)n;
      memcpy(start, header, (size_t)n);
      dap->out_seq++;
      result = dap->transport.write(
          dap->transport.user, start, (size_t)n + body);
      if (result != GRDBG_OK) {
        result = result == GRDBG_ERR_OOM ? result : GRDBG_ERR_IO;
      }
    }
  }
  grdbg_out_discard(out);
  return result;
}
