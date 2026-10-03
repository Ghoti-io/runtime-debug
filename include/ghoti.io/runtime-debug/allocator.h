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
 * @file allocator.h
 * @stability stable
 *
 * Allocator abstraction for Ghoti.io Runtime-debug.
 *
 * This is cutil's `GCU_Allocator` under a local name. One definition
 * across the suite means an allocator written for any library works with all
 * of them. A `NULL` allocator argument means the default.
 *
 * The debugger allocates only through this one: its breakpoints, its snapshot
 * of a stop and the buffers of the protocol adapter. It never allocates
 * through the context's counting allocator or page provider, because a
 * debugger that did would change the memory a program is charged for by being
 * attached (AD-15, AD-20).
 */

#ifndef GHOTI_IO_GRDBG_ALLOCATOR_H
#define GHOTI_IO_GRDBG_ALLOCATOR_H

#include <ghoti.io/runtime-debug/macros.h>

#include <ghoti.io/cutil/allocator.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocator interface used by the library.
 *
 * All function pointers must be non-NULL. `calloc_fn` must treat overflow of
 * `nitems * size` as failure. A zero-size request returns a usable non-NULL
 * pointer, so NULL always means failure.
 */
typedef GCU_Allocator GRDBG_Allocator;

/**
 * @brief The process-global default allocator.
 *
 * @return cutil's default allocator. Never NULL.
 */
GRDBG_API const GRDBG_Allocator * grdbg_allocator_default(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRDBG_ALLOCATOR_H */
