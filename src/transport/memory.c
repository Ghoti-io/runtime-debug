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
 * A transport over a memory buffer: input is a copy of the bytes the client
 * "sends", and output is collected. For tests and the fuzzer.
 *
 * It also destroys any transport this library made, since all of them share a
 * first part.
 */

#include <ghoti.io/runtime-debug/macros.h>

#include "transport_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  GRDBG_TransportBase base;
  uint8_t * input;
  size_t input_length;
  size_t input_position;
  uint8_t * output;
  size_t output_length;
  size_t output_capacity;
} MemoryTransport;

static GRDBG_Result memory_read(
    void * user, void * buffer, size_t capacity, size_t * out_read) {
  MemoryTransport * t = user;
  size_t left = t->input_length - t->input_position;
  size_t n = capacity < left ? capacity : left;
  if (n > 0) {
    memcpy(buffer, t->input + t->input_position, n);
  }
  t->input_position += n;
  *out_read = n;
  return GRDBG_OK;
}

static GRDBG_Result memory_write(
    void * user, const void * buffer, size_t length) {
  MemoryTransport * t = user;
  const GRDBG_Allocator * a = t->base.allocator;
  if (length > SIZE_MAX - t->output_length) {
    return GRDBG_ERR_IO;
  }
  size_t need = t->output_length + length;
  if (need > t->output_capacity) {
    size_t capacity = t->output_capacity == 0 ? 1024 : t->output_capacity;
    while (capacity < need) {
      if (capacity > SIZE_MAX / 2) {
        return GRDBG_ERR_IO;
      }
      capacity *= 2;
    }
    uint8_t * grown = a->realloc_fn(a->ctx, t->output, capacity);
    if (grown == NULL) {
      return GRDBG_ERR_IO;
    }
    t->output = grown;
    t->output_capacity = capacity;
  }
  if (length > 0) {
    memcpy(t->output + t->output_length, buffer, length);
  }
  t->output_length = need;
  return GRDBG_OK;
}

static void memory_release(GRDBG_TransportBase * base) {
  void * whole = base; /* the base is the first part of the transport */
  MemoryTransport * t = whole;
  const GRDBG_Allocator * a = base->allocator;
  if (t->input != NULL) {
    a->free_fn(a->ctx, t->input);
  }
  if (t->output != NULL) {
    a->free_fn(a->ctx, t->output);
  }
}

bool grdbg_transport_valid(const GRDBG_Transport * transport) {
  /* The first member is read to learn how much of the rest exists, so a
   * transport is judged by that alone (see grcore_key_valid in runtime-core). A
   * size above this header's is a transport from a newer header, which is
   * accepted. */
  return transport != NULL && transport->size >= GRDBG_TRANSPORT_MIN_SIZE &&
      transport->size % _Alignof(GRDBG_Transport) == 0;
}

GRDBG_Result grdbg_transport_create_memory(const void * input, size_t length,
    const GRDBG_Allocator * allocator, GRDBG_Transport ** out_transport) {
  if (out_transport == NULL || (input == NULL && length > 0)) {
    return GRDBG_ERR_INVALID;
  }
  if (allocator == NULL) {
    allocator = grdbg_allocator_default();
  }
  MemoryTransport * t = allocator->calloc_fn(allocator->ctx, 1, sizeof *t);
  if (t == NULL) {
    return GRDBG_ERR_OOM;
  }
  if (length > 0) {
    t->input = allocator->malloc_fn(allocator->ctx, length);
    if (t->input == NULL) {
      allocator->free_fn(allocator->ctx, t);
      return GRDBG_ERR_OOM;
    }
    memcpy(t->input, input, length);
  }
  t->input_length = length;
  t->base.allocator = allocator;
  t->base.release = memory_release;
  t->base.pub.size = sizeof t->base.pub;
  t->base.pub.user = t;
  t->base.pub.read = memory_read;
  t->base.pub.write = memory_write;
  t->base.pub.close = NULL;
  *out_transport = &t->base.pub;
  return GRDBG_OK;
}

GRDBG_Result grdbg_transport_memory_output(const GRDBG_Transport * transport,
    const uint8_t ** out_data, size_t * out_length) {
  if (!grdbg_transport_valid(transport) || out_data == NULL ||
      out_length == NULL || transport->read != memory_read) {
    return GRDBG_ERR_INVALID;
  }
  const MemoryTransport * t = transport->user;
  *out_data = t->output;
  *out_length = t->output_length;
  return GRDBG_OK;
}

void grdbg_transport_destroy(GRDBG_Transport * transport) {
  if (transport == NULL) {
    return;
  }
  /* `user` is the whole transport, whose first part is the base. */
  GRDBG_TransportBase * base = transport->user;
  const GRDBG_Allocator * allocator = base->allocator;
  base->release(base);
  allocator->free_fn(allocator->ctx, base);
}
