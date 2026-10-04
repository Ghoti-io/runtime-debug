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
 * The debug model's private declarations.
 *
 * One struct per attached debugger, and the helpers its source files share.
 * Nothing here knows of JSON or of any protocol (AD-15).
 */

#ifndef GHOTI_IO_GRDBG_SRC_MODEL_MODEL_INTERNAL_H
#define GHOTI_IO_GRDBG_SRC_MODEL_MODEL_INTERNAL_H

#include <ghoti.io/runtime-debug/macros.h>

#include <ghoti.io/runtime-debug/model.h>

#include <ghoti.io/runtime-core/runtime-core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief One line breakpoint. It owns its copy of the source string. */
typedef struct GRDBG_Breakpoint {
  uint64_t id;   ///< Unique within the debugger; never reused.
  char * source; ///< Owned.
  int line;      ///< 1-based.
} GRDBG_Breakpoint;

/** @brief A block of the arena that holds a stop's strings. */
typedef struct GRDBG_Chunk {
  struct GRDBG_Chunk * next;
  size_t used;
  size_t capacity;
} GRDBG_Chunk;

/** @brief A recorded frame: the abstract frame, copied by value, and its name. */
typedef struct GRDBG_SnapFrame {
  GRCORE_AbstractFrame frame;
  char name[24];
} GRDBG_SnapFrame;

struct GRDBG_Debugger {
  GRCORE_Context * context;
  const GRDBG_Allocator * allocator;
  GRDBG_Limits limits;

  GRCORE_Port * port;         ///< Retained; the debugger's way to arm.
  GRCORE_RequestKind kind;    ///< The debugger's own request kind.
  bool armed;                 ///< The kind is posted.

  GRDBG_Breakpoint * bps;
  size_t bp_count;
  size_t bp_capacity;
  uint64_t next_id;
  uint64_t * hits;            ///< Ids matched at the stop; capacity >= bp_count.
  size_t hit_capacity;
  size_t hit_count;

  GRDBG_StepKind step;
  size_t step_depth;
  const char * step_file;     ///< The engine's own string, per the location
                              ///< contract.
  int step_line;
  bool pause_requested;

  GRDBG_StopReason stop_reason;

  uint64_t generation;
  bool have_snapshot;
  GRDBG_SnapFrame * frames;
  size_t frame_count;         ///< Recorded.
  size_t frame_capacity;
  size_t frame_total;         ///< On the stack.
  GRDBG_Chunk * chunks;
};

/** @brief The allocator the debugger draws on, for the adapter that shares it. */
const GRDBG_Allocator * grdbg_debugger_allocator(const GRDBG_Debugger * debugger);

/** @brief The key, for the registry lookup and the destructor. */
GRDBG_INTERNAL_API extern const GRCORE_Key grdbg_key;

/** @brief Maps a runtime-core result into this library's. */
GRDBG_Result grdbg_from_core(GRCORE_Result result);

/** @brief Fills `out` from `in`: NULL is all defaults and a zero field is that
 *   field's default. */
void grdbg_limits_resolve(const GRDBG_Limits * in, GRDBG_Limits * out);

/** @brief Whether the caller may use the debugger: the owner of its context. */
bool grdbg_owned(const GRDBG_Debugger * debugger);

/** @brief Whether guest state may be read: the owner of an at-poll or paused
 *   context. */
bool grdbg_readable(const GRDBG_Debugger * debugger);

/** @brief Posts or clears the debugger's request kind to match `want`. Posting
 *   can fail; clearing is retried by the next call if it does. */
GRDBG_Result grdbg_arm_for(GRDBG_Debugger * debugger, bool want);

/** @brief Whether the debugger has anything to do. */
bool grdbg_wants_arming(const GRDBG_Debugger * debugger);

/** @brief Forgets the snapshot and the strings, and bumps the generation. */
void grdbg_invalidate(GRDBG_Debugger * debugger);

/** @brief Forgets the stop: its reason and the breakpoints it matched. */
void grdbg_forget_stop(GRDBG_Debugger * debugger);

/** @brief Frees every breakpoint. */
void grdbg_free_breakpoints(GRDBG_Debugger * debugger);

/** @brief Records the stack if it is not recorded, after checking that it may
 *   be read. */
GRDBG_Result grdbg_snapshot_ensure(GRDBG_Debugger * debugger);

/** @brief Allocates `size` bytes from the debugger's allocator; NULL on
 *   failure. */
void * grdbg_alloc(GRDBG_Debugger * debugger, size_t size);

/** @brief Frees what ::grdbg_alloc returned. */
void grdbg_free(GRDBG_Debugger * debugger, void * pointer);

/** @brief Where the stop's arena stood, to give back what a request took. */
typedef struct GRDBG_ArenaMark {
  const GRDBG_Chunk * chunk; ///< The head chunk then, or NULL for an empty arena.
  size_t used;               ///< Its fill then.
} GRDBG_ArenaMark;

/** @brief Notes the arena's present end. */
GRDBG_ArenaMark grdbg_arena_mark(const GRDBG_Debugger * debugger);

/** @brief Gives back everything the arena took since `mark`: strings handed
 *   out since then are no longer valid. The strings of a request are copied
 *   into the response before this is called, so a session that repeats a
 *   read does not keep every copy until the next resume. */
void grdbg_arena_release(GRDBG_Debugger * debugger, GRDBG_ArenaMark mark);

/** @brief Copies `length` bytes and a terminator into the stop's arena. */
char * grdbg_arena_string(
    GRDBG_Debugger * debugger, const char * text, size_t length);

#endif /* GHOTI_IO_GRDBG_SRC_MODEL_MODEL_INTERNAL_H */
