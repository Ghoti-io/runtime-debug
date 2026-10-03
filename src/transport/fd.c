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
 * A transport over two POSIX file descriptors (AD-13).
 *
 * The library never listens, binds or accepts: the host opens whatever it
 * likes and hands over the descriptors. A socket is written with
 * `MSG_NOSIGNAL`, so a client that went away is an error return and not a
 * signal; the library changes no signal disposition of the host's.
 */

/* S_ISSOCK, MSG_NOSIGNAL and read/write are POSIX, and -std=c17 hides them. */
#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/runtime-debug/macros.h>

#include "transport_internal.h"

#ifndef _WIN32

#include <errno.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  GRDBG_TransportBase base;
  int in_fd;
  int out_fd;
  bool out_is_socket;
} FdTransport;

static GRDBG_Result fd_read(
    void * user, void * buffer, size_t capacity, size_t * out_read) {
  FdTransport * t = user;
  for (;;) {
    ssize_t n = read(t->in_fd, buffer, capacity);
    if (n >= 0) {
      *out_read = (size_t)n;
      return GRDBG_OK;
    }
    if (errno != EINTR) {
      return GRDBG_ERR_IO;
    }
  }
}

static GRDBG_Result fd_write(void * user, const void * buffer, size_t length) {
  FdTransport * t = user;
  const char * p = buffer;
  while (length > 0) {
    ssize_t n = t->out_is_socket
        ? send(t->out_fd, p, length, MSG_NOSIGNAL)
        : write(t->out_fd, p, length);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return GRDBG_ERR_IO;
    }
    if (n == 0) {
      return GRDBG_ERR_IO; /* a write that moves nothing will not finish */
    }
    p += n;
    length -= (size_t)n;
  }
  return GRDBG_OK;
}

static void fd_release(GRDBG_TransportBase * base) {
  (void)base;
}

GRDBG_Result grdbg_transport_create_fd(int in_fd, int out_fd,
    const GRDBG_Allocator * allocator, GRDBG_Transport ** out_transport) {
  if (in_fd < 0 || out_fd < 0 || out_transport == NULL) {
    return GRDBG_ERR_INVALID;
  }
  if (allocator == NULL) {
    allocator = grdbg_allocator_default();
  }
  FdTransport * t = allocator->calloc_fn(allocator->ctx, 1, sizeof *t);
  if (t == NULL) {
    return GRDBG_ERR_OOM;
  }
  struct stat st;
  t->in_fd = in_fd;
  t->out_fd = out_fd;
  t->out_is_socket = fstat(out_fd, &st) == 0 && S_ISSOCK(st.st_mode);
  t->base.allocator = allocator;
  t->base.release = fd_release;
  t->base.pub.user = t;
  t->base.pub.read = fd_read;
  t->base.pub.write = fd_write;
  t->base.pub.close = NULL;
  *out_transport = &t->base.pub;
  return GRDBG_OK;
}

#else /* _WIN32 */

/* TODO(windows): descriptors are a POSIX idea here; a Windows host binds its
 * own handles through a GRDBG_Transport of its own. See
 * notes/suite/WINDOWS-TODO.md. */
GRDBG_Result grdbg_transport_create_fd(int in_fd, int out_fd,
    const GRDBG_Allocator * allocator, GRDBG_Transport ** out_transport) {
  (void)in_fd;
  (void)out_fd;
  (void)allocator;
  (void)out_transport;
  return GRDBG_ERR_UNSUPPORTED;
}

#endif
