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
 * @file model.h
 * @stability stable
 *
 * The debug model: breakpoints, stepping and the state of a stop (AD-15).
 *
 * This is the whole of the debugger. It knows nothing of JSON or of any
 * protocol; the Debug Adapter Protocol in dap.h is a thin adapter over it, and
 * so would any other be. A context has at most one debugger, attached under a
 * cardinality-one key in the YIELD phase; it dies with its context.
 *
 * ## How it stays out of the way (AD-15, AD-20)
 *
 *  - It allocates only from its own allocator, never the context's, so being
 *    attached changes neither the memory a program is charged for nor its
 *    fuel (it charges none).
 *  - It reads frames only through the frame walk, and only while the context
 *    is at-poll or paused and held by the calling thread.
 *  - An attached debugger with nothing to do is *unarmed*: it defines its
 *    request kind and leaves it clear, so a poll with nothing pending is still
 *    one load and one branch. It *arms* by posting that kind through the
 *    context's own port only while it holds a breakpoint, a step or a pause
 *    request, and disarms when it holds none.
 *  - It votes ::GRCORE_VERDICT_PAUSE only at a poll that may pause to the host
 *    (::grcore_pollcall_pause_allowed), never inside a nested activation and
 *    never at a native's runtime poll, where core would turn the pause into
 *    an unwind and change the program.
 *
 * ## The granularity limit
 *
 * A breakpoint on a line fires where the engine polls at that line. An engine
 * that polls only at function entry and loop back-edges stops only there. This
 * is the contract an engine's host has to honour for line breakpoints to mean
 * anything, and it is stated in the design document rather than hidden.
 *
 * ## Resuming
 *
 * After a stop the engine continues after the poll that paused, as in
 * runtime-core's `pause_resume` example. The debugger does not suppress a stop
 * at the same poll identity after a resume, so a one-statement loop stops on
 * every iteration.
 *
 * Every string and id the debugger returns is its own and is valid until the
 * next resume: until the next ::grdbg_debugger_continue,
 * ::grdbg_debugger_step or ::grdbg_debugger_disarm, or the next poll the
 * debugger sees, or the next ::grdbg_debugger_stopped.
 *
 * ## Threads
 *
 * A debugger is used from its context's owner thread. Every function refuses
 * another thread with ::GRDBG_ERR_INVALID, and the debugger migrates with the
 * context when it is released and acquired.
 */

#ifndef GHOTI_IO_GRDBG_MODEL_H
#define GHOTI_IO_GRDBG_MODEL_H

#include <ghoti.io/runtime-debug/macros.h>

#include <ghoti.io/runtime-debug/allocator.h>
#include <ghoti.io/runtime-debug/core.h>
#include <ghoti.io/runtime-debug/limits.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/key.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A context's debugger. Opaque; owned by the context. */
typedef struct GRDBG_Debugger GRDBG_Debugger;

/** @brief Why the debugger stopped the program. */
typedef enum {
  GRDBG_STOP_NONE = 0,   ///< The debugger did not stop it.
  GRDBG_STOP_BREAKPOINT, ///< A breakpoint matched the innermost frame.
  GRDBG_STOP_STEP,       ///< A step was satisfied.
  GRDBG_STOP_PAUSE       ///< ::grdbg_debugger_request_pause asked for it.
} GRDBG_StopReason;

/** @brief The three steps. */
typedef enum {
  GRDBG_STEP_NONE = 0, ///< Not stepping.
  GRDBG_STEP_IN,       ///< Stop at the next poll that is somewhere else.
  GRDBG_STEP_OVER,     ///< ...without going into a call made on the way.
  GRDBG_STEP_OUT       ///< Stop in the caller.
} GRDBG_StepKind;

/** @brief What a scope is. */
typedef enum {
  GRDBG_SCOPE_LOCAL = 0, ///< A function's own variables.
  GRDBG_SCOPE_CLOSURE,   ///< Variables captured from an enclosing function.
  GRDBG_SCOPE_GLOBAL     ///< Module or global variables.
} GRDBG_ScopeKind;

/** @brief What a variable's value is, as the engine declares it. */
typedef enum {
  GRDBG_VALUE_RAW = 0, ///< Plain bits.
  GRDBG_VALUE_ENGINE   ///< An engine value, shown by the engine's inspector.
} GRDBG_ValueKind;

/** @brief One frame of the stack at a stop. */
typedef struct GRDBG_Frame {
  size_t depth;        ///< Zero for the innermost frame.
  const char * name;   ///< "frame N", N from 1; the abstract frame has no name.
  const char * file;   ///< The engine's name for where it is; NULL if none.
  int line;            ///< 1-based; zero if there is none.
  const char * engine; ///< The engine descriptor's name; may be NULL.
} GRDBG_Frame;

/** @brief One scope of a frame. */
typedef struct GRDBG_Scope {
  GRDBG_ScopeKind kind;  ///< What it is.
  const char * name;     ///< For display; never NULL.
  size_t variable_count; ///< How many variables it holds.
} GRDBG_Scope;

/** @brief One variable of a scope. */
typedef struct GRDBG_Variable {
  const char * name;    ///< For display; never NULL.
  const char * text;    ///< The engine's inspector's text; never NULL.
  GRDBG_ValueKind kind; ///< The slot kind.
} GRDBG_Variable;

/** @brief What ::grdbg_debugger_stopped says about the pause in progress. */
typedef struct GRDBG_Stop {
  GRDBG_StopReason reason;   ///< Why, or ::GRDBG_STOP_NONE if another key did.
  const uint64_t * hit_ids;  ///< The breakpoints that matched.
  size_t hit_count;          ///< How many.
  const char * other_key;    ///< The name of the first other key that paused
                             ///< the context; NULL if none, or if only the
                             ///< debugger's did.
} GRDBG_Stop;

/**
 * @brief The key every debugger is registered under: cardinality one, phase
 *   YIELD.
 *
 * A host reads a pause's keys with `grcore_context_pause_key` and compares
 * them with this to tell the debugger's stop from a budget's.
 *
 * @return A static key. Never NULL.
 */
GRDBG_API const GRCORE_Key * grdbg_debugger_key(void);

/**
 * @brief Attaches a debugger to a context, with the default allocator.
 *
 * @param context The context. The caller must own it, and it must not be
 *   running.
 * @param limits The caps; NULL is the defaults.
 * @param out_debugger Receives the debugger. Written only on success.
 * @return ::GRDBG_OK; ::GRDBG_ERR_INVALID for a NULL argument, a thread that
 *   does not own the context, a running context, or a context that already
 *   has a debugger (which is left intact); ::GRDBG_ERR_OOM. A failure leaves
 *   the context as it was, except that one unused request kind may stay
 *   registered when the last step is what failed.
 */
GRDBG_API GRDBG_Result grdbg_debugger_attach(GRCORE_Context * context,
    const GRDBG_Limits * limits, GRDBG_Debugger ** out_debugger);

/**
 * @brief ::grdbg_debugger_attach with an allocator of its own.
 *
 * @param context As above.
 * @param allocator The debugger's allocator; NULL is the default. It must
 *   outlive the context. It is not the context's.
 * @param limits As above.
 * @param out_debugger As above.
 * @return As above.
 */
GRDBG_API GRDBG_Result grdbg_debugger_attach_with_allocator(
    GRCORE_Context * context, const GRDBG_Allocator * allocator,
    const GRDBG_Limits * limits, GRDBG_Debugger ** out_debugger);

/**
 * @brief The debugger attached to a context.
 *
 * @param context The context.
 * @return The debugger; NULL for NULL, for a thread that does not own the
 *   context, and when there is none.
 */
GRDBG_API GRDBG_Debugger * grdbg_debugger_get(const GRCORE_Context * context);

/**
 * @brief The context a debugger is attached to.
 *
 * @param debugger The debugger.
 * @return The context; NULL for NULL.
 */
GRDBG_API GRCORE_Context * grdbg_debugger_context(
    const GRDBG_Debugger * debugger);

/**
 * @brief Replaces the whole set of line breakpoints of one source (the DAP
 *   semantics).
 *
 * A breakpoint is a (source, line) pair. The source string is compared byte
 * for byte with the file of the innermost frame's location. The ids returned
 * are unique within the debugger and never reused. Duplicate lines in one call
 * each get an id.
 *
 * @param debugger The debugger.
 * @param source The source, a NUL-terminated string.
 * @param lines The lines, 1-based, `count` of them; may be NULL when `count`
 *   is zero, which clears the source's breakpoints.
 * @param count How many.
 * @param out_ids Receives one id per line; may be NULL when `count` is zero.
 *   Written only on success.
 * @return ::GRDBG_OK; ::GRDBG_ERR_LIMIT if the debugger would hold more than
 *   `max_breakpoints` (nothing changes); ::GRDBG_ERR_INVALID for a NULL
 *   argument, a line below 1 or a non-owner; ::GRDBG_ERR_OOM. A failure
 *   changes nothing.
 */
GRDBG_API GRDBG_Result grdbg_debugger_set_breakpoints(
    GRDBG_Debugger * debugger, const char * source, const int * lines,
    size_t count, uint64_t * out_ids);

/**
 * @brief How many breakpoints the debugger holds.
 *
 * @param debugger The debugger.
 * @return The count; zero for NULL.
 */
GRDBG_API size_t grdbg_debugger_breakpoint_count(
    const GRDBG_Debugger * debugger);

/**
 * @brief Whether the debugger is armed: its request kind is posted, because
 *   it holds a breakpoint, a step or a pause request.
 *
 * @param debugger The debugger.
 * @return True if armed; false for NULL.
 */
GRDBG_API bool grdbg_debugger_armed(const GRDBG_Debugger * debugger);

/**
 * @brief Starts a step from the stop in progress.
 *
 * Stepping is measured on the abstract frame walk: depth is the number of
 * frames and a location is (file, line). *In* stops at the next poll whose
 * (location, depth) differs from the start. *Over* stops at the next poll with
 * depth not greater than the start and a different location, or with a smaller
 * depth. *Out* stops at the next poll with a smaller depth; with no outer frame
 * it runs to the end. A breakpoint reached on the way wins and is reported as
 * one; a pause of any other cause cancels the step (see
 * ::grdbg_debugger_stopped). This does not resume: the host resumes the
 * context.
 *
 * @param debugger The debugger.
 * @param kind The step.
 * @return ::GRDBG_OK; ::GRDBG_ERR_INVALID unless the context is paused or
 *   at-poll and held by the caller, or for a bad kind; ::GRDBG_ERR_OOM or
 *   ::GRDBG_ERR_INTERNAL if the request could not be posted (nothing changes).
 */
GRDBG_API GRDBG_Result grdbg_debugger_step(
    GRDBG_Debugger * debugger, GRDBG_StepKind kind);

/**
 * @brief Prepares to continue: cancels any step and forgets the stop.
 *
 * Breakpoints stay. This does not resume: the host resumes the context.
 *
 * @param debugger The debugger.
 * @return ::GRDBG_OK, or ::GRDBG_ERR_INVALID for NULL or a non-owner.
 */
GRDBG_API GRDBG_Result grdbg_debugger_continue(GRDBG_Debugger * debugger);

/**
 * @brief Asks for a stop at the next poll that may pause.
 *
 * To pause a guest that is running on another thread, the host posts
 * ::GRCORE_REQUEST_INTERRUPT itself; this is for the owner thread.
 *
 * @param debugger The debugger.
 * @return ::GRDBG_OK, ::GRDBG_ERR_INVALID or ::GRDBG_ERR_INTERNAL if the
 *   request could not be posted.
 */
GRDBG_API GRDBG_Result grdbg_debugger_request_pause(GRDBG_Debugger * debugger);

/**
 * @brief Clears every breakpoint, any step and any pause request, and stops
 *   the handler voting.
 *
 * This is the only way to detach: the debugger stays attached but does
 * nothing, and a later breakpoint arms it again.
 *
 * @param debugger The debugger.
 * @return ::GRDBG_OK, or ::GRDBG_ERR_INVALID for NULL or a non-owner.
 */
GRDBG_API GRDBG_Result grdbg_debugger_disarm(GRDBG_Debugger * debugger);

/**
 * @brief Begins a stop: reads the pause the context is in.
 *
 * Call it once at every pause. It forgets any earlier snapshot, and tells the
 * host why the context paused. If the debugger did not vote for this pause
 * (a fuel or time budget, an interrupt), any step in progress is cancelled,
 * because the program is no longer where the step started.
 *
 * @param debugger The debugger.
 * @param out_stop Receives the stop. Written only on success; its pointers
 *   are valid until the next resume.
 * @return ::GRDBG_OK, or ::GRDBG_ERR_INVALID unless the context is paused or
 *   at-poll and held by the caller.
 */
GRDBG_API GRDBG_Result grdbg_debugger_stopped(
    GRDBG_Debugger * debugger, GRDBG_Stop * out_stop);

/**
 * @brief How many frames the stack holds, and how many the snapshot records.
 *
 * The snapshot records at most `max_frames` of the innermost frames; the total
 * is counted all the same.
 *
 * @param debugger The debugger.
 * @param out_total Receives the number of frames on the stack; may be NULL.
 * @param out_available Receives how many ::grdbg_debugger_frame accepts; may
 *   be NULL. Both are written only on success.
 * @return ::GRDBG_OK; ::GRDBG_ERR_INVALID unless the context is paused or
 *   at-poll and held by the caller; ::GRDBG_ERR_OOM.
 */
GRDBG_API GRDBG_Result grdbg_debugger_frames(GRDBG_Debugger * debugger,
    size_t * out_total, size_t * out_available);

/**
 * @brief One recorded frame, innermost first. The frame ids a client sees are
 *   `index + 1`.
 *
 * @param debugger The debugger.
 * @param index Below the available count.
 * @param out_frame Receives the frame. Written only on success.
 * @return ::GRDBG_OK or ::GRDBG_ERR_INVALID (not readable, or an index that is
 *   not a frame of this stop) or ::GRDBG_ERR_OOM.
 */
GRDBG_API GRDBG_Result grdbg_debugger_frame(
    GRDBG_Debugger * debugger, size_t index, GRDBG_Frame * out_frame);

/**
 * @brief How many scopes a frame has.
 *
 * @param debugger The debugger.
 * @param frame The frame's index.
 * @param out_count Receives the count. Written only on success.
 * @return As ::grdbg_debugger_frame.
 */
GRDBG_API GRDBG_Result grdbg_debugger_scope_count(
    GRDBG_Debugger * debugger, size_t frame, size_t * out_count);

/**
 * @brief Describes one scope of a frame.
 *
 * @param debugger The debugger.
 * @param frame The frame's index.
 * @param scope The scope's index.
 * @param out_scope Receives the scope. Written only on success.
 * @return As ::grdbg_debugger_frame.
 */
GRDBG_API GRDBG_Result grdbg_debugger_scope(GRDBG_Debugger * debugger,
    size_t frame, size_t scope, GRDBG_Scope * out_scope);

/**
 * @brief Reads one variable of one scope.
 *
 * Structured expansion of a value is not offered: the engine descriptor has no
 * child interface. A variable is its name, the inspector's text and the slot
 * kind.
 *
 * @param debugger The debugger.
 * @param frame The frame's index.
 * @param scope The scope's index.
 * @param index The variable's index, below the scope's count.
 * @param out_variable Receives the variable. Written only on success.
 * @return As ::grdbg_debugger_frame.
 */
GRDBG_API GRDBG_Result grdbg_debugger_variable(GRDBG_Debugger * debugger,
    size_t frame, size_t scope, size_t index, GRDBG_Variable * out_variable);

/**
 * @brief Looks a variable up by name in a frame's scopes, innermost scope
 *   first. This is all `evaluate` does.
 *
 * @param debugger The debugger.
 * @param frame The frame's index.
 * @param name The name, a NUL-terminated string.
 * @param out_found Receives whether a scope has the name. Written only on
 *   success.
 * @param out_variable Receives the variable when it was found; may be NULL.
 *   Written only when `*out_found` is set.
 * @return ::GRDBG_OK (whether or not the name was found), or as
 *   ::grdbg_debugger_frame.
 */
GRDBG_API GRDBG_Result grdbg_debugger_find_variable(GRDBG_Debugger * debugger,
    size_t frame, const char * name, bool * out_found,
    GRDBG_Variable * out_variable);

/**
 * @brief A number that changes whenever what the debugger returned earlier
 *   stops being valid.
 *
 * An adapter that hands ids to a client compares this with the value it saw
 * when it issued them.
 *
 * @param debugger The debugger.
 * @return The generation; zero for NULL.
 */
GRDBG_API uint64_t grdbg_debugger_generation(const GRDBG_Debugger * debugger);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRDBG_MODEL_H */
