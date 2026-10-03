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
 * @file transport.h
 * @stability stable
 *
 * The transport: how debug protocol bytes reach a client (AD-13).
 *
 * A transport is a CONVENTIONS.md section 5 stream pair, and the host binds
 * it. The library never listens, binds or accepts: whoever embeds the
 * debugger decides whether the other end is stdio, a socket it accepted, a
 * pipe or a test's buffer, and hands over the two directions as callbacks.
 * Two ready-made ones are provided, over POSIX file descriptors and over a
 * memory buffer.
 *
 * A transport is used from one thread at a time: the context's owner thread,
 * because all protocol I/O runs there (AD-6).
 */

#ifndef GHOTI_IO_GRDBG_TRANSPORT_H
#define GHOTI_IO_GRDBG_TRANSPORT_H

#include <ghoti.io/runtime-debug/macros.h>

#include <ghoti.io/runtime-debug/allocator.h>
#include <ghoti.io/runtime-debug/core.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A stream pair, bound by the host.
 *
 * `read` and `write` are required. A short read is not an error: the library
 * asks again. `write` must take all of `length` bytes or fail; a transport
 * over something that writes short (a socket) loops inside the callback, as
 * the descriptor transport does.
 */
typedef struct GRDBG_Transport {
  void * user; ///< Passed to every callback.
  /** Reads up to `capacity` bytes into `buffer` and stores the count in
   *  `*out_read`. A count of zero is end of input. Returns ::GRDBG_OK or
   *  ::GRDBG_ERR_IO. */
  GRDBG_Result (*read)(
      void * user, void * buffer, size_t capacity, size_t * out_read);
  /** Writes all `length` bytes. Returns ::GRDBG_OK or ::GRDBG_ERR_IO. */
  GRDBG_Result (*write)(void * user, const void * buffer, size_t length);
  /** Called once by ::grdbg_dap_destroy, if not NULL. It releases whatever the
   *  host tied to the stream (a connection); the transports created here
   *  have nothing to close, since the host owns its descriptors. */
  GRDBG_Result (*close)(void * user);
} GRDBG_Transport;

/**
 * @brief A transport over two POSIX file descriptors.
 *
 * Standard input and output are 0 and 1; a connected socket is the same
 * descriptor twice. A partial `write(2)` is continued and `EINTR` retried. On
 * a socket the library sends with `MSG_NOSIGNAL`, so a client that went away
 * is ::GRDBG_ERR_IO and not a `SIGPIPE`; on a pipe the host's signal
 * disposition applies, so a host that wants an error there ignores `SIGPIPE`.
 * The descriptors must be blocking: a read or write that would block
 * (`EAGAIN`) is ::GRDBG_ERR_IO and ends the session. The descriptors stay the
 * host's: nothing here opens, closes or changes them.
 *
 * @param in_fd Where requests are read from.
 * @param out_fd Where responses and events are written to.
 * @param allocator The allocator for the transport object; NULL is the default.
 * @param out_transport Receives the transport. Written only on success; free
 *   it with ::grdbg_transport_destroy after the session using it is destroyed.
 * @return ::GRDBG_OK, ::GRDBG_ERR_INVALID for a negative descriptor or a NULL
 *   output, ::GRDBG_ERR_UNSUPPORTED on Windows, or ::GRDBG_ERR_OOM.
 */
GRDBG_API GRDBG_Result grdbg_transport_create_fd(int in_fd, int out_fd,
    const GRDBG_Allocator * allocator, GRDBG_Transport ** out_transport);

/**
 * @brief A transport over a memory buffer: reads come from a copy of `input`,
 *   and writes are collected, for tests and the fuzzer.
 *
 * @param input The bytes the client "sends"; NULL with a length of zero is an
 *   empty input.
 * @param length How many.
 * @param allocator The allocator; NULL is the default.
 * @param out_transport Receives the transport. Written only on success.
 * @return ::GRDBG_OK, ::GRDBG_ERR_INVALID or ::GRDBG_ERR_OOM.
 */
GRDBG_API GRDBG_Result grdbg_transport_create_memory(const void * input,
    size_t length, const GRDBG_Allocator * allocator,
    GRDBG_Transport ** out_transport);

/**
 * @brief What a memory transport has been written so far.
 *
 * @param transport A transport made by ::grdbg_transport_create_memory.
 * @param out_data Receives the bytes, valid until the next write or destroy.
 * @param out_length Receives how many. Both are written only on success.
 * @return ::GRDBG_OK, or ::GRDBG_ERR_INVALID for NULL or any other transport.
 */
GRDBG_API GRDBG_Result grdbg_transport_memory_output(
    const GRDBG_Transport * transport, const uint8_t ** out_data,
    size_t * out_length);

/**
 * @brief Frees a transport made by one of the functions above.
 *
 * @param transport The transport; NULL is ignored. Do not pass a struct the
 *   host filled in itself.
 */
GRDBG_API void grdbg_transport_destroy(GRDBG_Transport * transport);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRDBG_TRANSPORT_H */
