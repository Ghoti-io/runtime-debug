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
 * @file dap.h
 * @stability stable
 *
 * A Debug Adapter Protocol adapter over the debug model (AD-15, AD-13).
 *
 * The adapter implements the Debug Adapter Protocol 1.71 over a transport the
 * host binds. It is deliberately thin: it translates messages, maps ids, and
 * holds no debug logic beyond that. Everything it does to the program it does
 * through ::GRDBG_Debugger.
 *
 * ## Messages
 *
 * `Content-Length: N\r\n\r\n` followed by N bytes of UTF-8 JSON. A header block
 * too long, a missing, non-numeric or negative `Content-Length`, or a length
 * over the limit ends the session with ::GRDBG_ERR_FORMAT or
 * ::GRDBG_ERR_LIMIT: the stream cannot be resynchronised once its framing is
 * lost, and guessing where the next message starts would be worse than
 * stopping. Well-framed bytes that are not a request are a different thing:
 * they get an error response when a `seq` can be read, and are dropped when it
 * cannot. The JSON is read by `text`'s parser with a depth and size limit;
 * nothing is parsed by hand.
 *
 * ## The host loop
 *
 * There is no I/O thread. DAP I/O runs on the owner thread, while the context
 * is stopped, and reading guest state from another thread is never offered
 * (AD-6). The host's loop is:
 *
 *  1. ::grdbg_dap_serve until it returns ::GRDBG_SERVE_RESUME for the client's
 *     `configurationDone` (breakpoints are set before it);
 *  2. run the context;
 *  3. at every pause, ::grdbg_dap_notify_stopped, then ::grdbg_dap_serve, then
 *     resume, or end the run if serve says to;
 *  4. when the run finishes, ::grdbg_dap_notify_finished, and then
 *     ::grdbg_dap_serve once more, because the client's `disconnect` comes
 *     after `terminated` and wants an answer.
 *
 * `examples/dap_session.c` is that loop. A pause of a running guest needs the
 * host to post `GRCORE_REQUEST_INTERRUPT` itself.
 *
 * The session's objects hold the debugger: destroy the adapter before the
 * context is destroyed.
 */

#ifndef GHOTI_IO_GRDBG_DAP_H
#define GHOTI_IO_GRDBG_DAP_H

#include <ghoti.io/runtime-debug/macros.h>

#include <ghoti.io/runtime-debug/core.h>
#include <ghoti.io/runtime-debug/limits.h>
#include <ghoti.io/runtime-debug/model.h>
#include <ghoti.io/runtime-debug/transport.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A DAP session. Opaque. */
typedef struct GRDBG_Dap GRDBG_Dap;

/** @brief What a client asked the host to do when ::grdbg_dap_serve returns. */
typedef enum {
  GRDBG_SERVE_RESUME = 0, ///< `configurationDone`, `continue`, `next`,
                          ///< `stepIn` or `stepOut` (the step is armed).
  GRDBG_SERVE_TERMINATE,  ///< `terminate`, or `disconnect` with
                          ///< `terminateDebuggee`: end the program.
  GRDBG_SERVE_DETACH      ///< Any other `disconnect`, or end of input: the
                          ///< debugger is disarmed, so resume the run free.
} GRDBG_ServeResult;

/**
 * @brief Creates a session.
 *
 * The transport is copied; its `user` must outlive the session. The session
 * allocates from the debugger's allocator.
 *
 * @param debugger The debugger the session drives.
 * @param transport The stream pair; `read` and `write` are required.
 * @param limits The caps; NULL is the defaults.
 * @param out_dap Receives the session. Written only on success.
 * @return ::GRDBG_OK, ::GRDBG_ERR_INVALID for a NULL argument or a transport
 *   without `read` and `write`, or ::GRDBG_ERR_OOM.
 */
GRDBG_API GRDBG_Result grdbg_dap_create(GRDBG_Debugger * debugger,
    const GRDBG_Transport * transport, const GRDBG_Limits * limits,
    GRDBG_Dap ** out_dap);

/**
 * @brief Destroys a session, calling the transport's `close` if it has one.
 *
 * The transport object itself is the host's to destroy.
 *
 * @param dap The session; NULL is ignored.
 */
GRDBG_API void grdbg_dap_destroy(GRDBG_Dap * dap);

/**
 * @brief Reads and answers requests until one asks the host to proceed.
 *
 * Valid while the context is paused, and before the first run until
 * `configurationDone`. Requests that need a stopped program answer with
 * `success:false` otherwise. Every response is written before the call
 * returns. End of input disarms the debugger and returns
 * ::GRDBG_SERVE_DETACH.
 *
 * @param dap The session.
 * @param out_result Receives what the client asked for. Written only when this
 *   returns ::GRDBG_OK.
 * @return ::GRDBG_OK; ::GRDBG_ERR_FORMAT or ::GRDBG_ERR_LIMIT for broken
 *   framing; ::GRDBG_ERR_IO for a failed transport; ::GRDBG_ERR_OOM;
 *   ::GRDBG_ERR_INVALID for NULL or a call from a thread that does not own the
 *   context. After ::GRDBG_ERR_OOM the request being handled is lost and the
 *   session carries on; the client has been answered all the same, with
 *   `success: false` and the message "out of memory", built without
 *   allocating (when the request could not even be parsed, the seq and command
 *   in that answer are read off its text; a message that is not a request, with
 *   neither, is not answered). Only a response already written whole is not
 *   written twice. After a framing or
 *   I/O error the session is over, every later
 *   call returns the same error, and the debugger has been disarmed so the host
 *   can resume the run free.
 */
GRDBG_API GRDBG_Result grdbg_dap_serve(
    GRDBG_Dap * dap, GRDBG_ServeResult * out_result);

/**
 * @brief Emits the `stopped` event for the pause the context is in.
 *
 * The reason comes from the context's pause keys and the model's stop reason:
 * `breakpoint` with `hitBreakpointIds`, `step`, or `pause`. A pause with no
 * debugger key is `pause` with a `description` naming the core key that caused
 * it, such as `paused by fuel`. Call it once at every pause (it also starts the
 * model's stop: see ::grdbg_debugger_stopped). It writes nothing after the
 * client disconnected.
 *
 * @param dap The session.
 * @return ::GRDBG_OK; ::GRDBG_ERR_INVALID if the context is not paused or
 *   at-poll and held by the caller; ::GRDBG_ERR_IO; ::GRDBG_ERR_OOM.
 */
GRDBG_API GRDBG_Result grdbg_dap_notify_stopped(GRDBG_Dap * dap);

/**
 * @brief Emits `exited` and then `terminated`, when the host reports that the
 *   run finished.
 *
 * @param dap The session.
 * @param exit_code The program's exit code.
 * @return ::GRDBG_OK, ::GRDBG_ERR_IO or ::GRDBG_ERR_OOM.
 */
GRDBG_API GRDBG_Result grdbg_dap_notify_finished(GRDBG_Dap * dap, int exit_code);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRDBG_DAP_H */
