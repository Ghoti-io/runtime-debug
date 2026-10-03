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
 * @file runtime-debug.h
 * @stability stable
 *
 * Umbrella header for Ghoti.io Runtime-debug.
 *
 * The debugger of the language runtime stack: one debug model of breakpoints,
 * stepping and the state of a stop, attached to a `runtime-core` context, and
 * a thin Debug Adapter Protocol adapter over a transport the host binds
 * (AD-13, AD-15). No engine depends on it, and it names no engine: it learns
 * what a frame means from the descriptor the engine registered. It may include
 * all of `runtime-core` (it reads A's abstract frame), `text` and `cutil`, and
 * `make check-edges` checks that it includes nothing else.
 */

#ifndef GHOTI_IO_GRDBG_RUNTIME_DEBUG_H
#define GHOTI_IO_GRDBG_RUNTIME_DEBUG_H

#include <ghoti.io/runtime-debug/macros.h>

#include <ghoti.io/runtime-debug/allocator.h>
#include <ghoti.io/runtime-debug/core.h>
#include <ghoti.io/runtime-debug/libver.h>

#include <ghoti.io/runtime-debug/dap.h>
#include <ghoti.io/runtime-debug/limits.h>
#include <ghoti.io/runtime-debug/model.h>
#include <ghoti.io/runtime-debug/transport.h>

#endif /* GHOTI_IO_GRDBG_RUNTIME_DEBUG_H */
