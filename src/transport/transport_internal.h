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
 * The transports this library creates share a first part, so that one destroy
 * can free either kind.
 */

#ifndef GHOTI_IO_GRDBG_SRC_TRANSPORT_TRANSPORT_INTERNAL_H
#define GHOTI_IO_GRDBG_SRC_TRANSPORT_TRANSPORT_INTERNAL_H

#include <ghoti.io/runtime-debug/macros.h>

#include <ghoti.io/runtime-debug/transport.h>

/** @brief The common first part: what a destroy needs to know. */
typedef struct GRDBG_TransportBase {
  GRDBG_Transport pub;               ///< Must be first.
  const GRDBG_Allocator * allocator; ///< Frees the object.
  void (*release)(struct GRDBG_TransportBase * base); ///< Frees the extras.
} GRDBG_TransportBase;

#endif /* GHOTI_IO_GRDBG_SRC_TRANSPORT_TRANSPORT_INTERNAL_H */
