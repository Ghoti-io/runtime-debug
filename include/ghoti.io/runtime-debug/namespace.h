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
 * @file namespace.h
 * @stability stable
 *
 * Maps every public name of this library into its version namespace.
 *
 * Kept in one file rather than beside each declaration: a type rename has to
 * be in effect before any struct tag that uses the name, and an internal
 * header may define such a tag without including the public header that
 * declares the typedef.
 *
 * `make check-symbols` fails if an exported symbol is missing from this list.
 *
 * See CONVENTIONS.md section 4.
 */

#ifndef GHOTI_IO_GRDBG_NAMESPACE_H
#define GHOTI_IO_GRDBG_NAMESPACE_H

#include <ghoti.io/runtime-debug/libver.h>

/// @cond HIDDEN_SYMBOLS

/* Public types, and the private ones an internal header may name through a
 * struct tag. GCU_* names are cutil's and GRCORE_* names are runtime-core's;
 * each has already renamed them. */
#define GRDBG_Allocator GHOTIIO_RUNTIME_DEBUG(GRDBG_Allocator)
#define GRDBG_Breakpoint GHOTIIO_RUNTIME_DEBUG(GRDBG_Breakpoint)
#define GRDBG_Buf GHOTIIO_RUNTIME_DEBUG(GRDBG_Buf)
#define GRDBG_Chunk GHOTIIO_RUNTIME_DEBUG(GRDBG_Chunk)
#define GRDBG_Dap GHOTIIO_RUNTIME_DEBUG(GRDBG_Dap)
#define GRDBG_Debugger GHOTIIO_RUNTIME_DEBUG(GRDBG_Debugger)
#define GRDBG_Frame GHOTIIO_RUNTIME_DEBUG(GRDBG_Frame)
#define GRDBG_Limits GHOTIIO_RUNTIME_DEBUG(GRDBG_Limits)
#define GRDBG_Out GHOTIIO_RUNTIME_DEBUG(GRDBG_Out)
#define GRDBG_Ref GHOTIIO_RUNTIME_DEBUG(GRDBG_Ref)
#define GRDBG_Request GHOTIIO_RUNTIME_DEBUG(GRDBG_Request)
#define GRDBG_Result GHOTIIO_RUNTIME_DEBUG(GRDBG_Result)
#define GRDBG_Scope GHOTIIO_RUNTIME_DEBUG(GRDBG_Scope)
#define GRDBG_ScopeKind GHOTIIO_RUNTIME_DEBUG(GRDBG_ScopeKind)
#define GRDBG_ServeResult GHOTIIO_RUNTIME_DEBUG(GRDBG_ServeResult)
#define GRDBG_SnapFrame GHOTIIO_RUNTIME_DEBUG(GRDBG_SnapFrame)
#define GRDBG_StepKind GHOTIIO_RUNTIME_DEBUG(GRDBG_StepKind)
#define GRDBG_Stop GHOTIIO_RUNTIME_DEBUG(GRDBG_Stop)
#define GRDBG_StopReason GHOTIIO_RUNTIME_DEBUG(GRDBG_StopReason)
#define GRDBG_Transport GHOTIIO_RUNTIME_DEBUG(GRDBG_Transport)
#define GRDBG_TransportBase GHOTIIO_RUNTIME_DEBUG(GRDBG_TransportBase)
#define GRDBG_ValueKind GHOTIIO_RUNTIME_DEBUG(GRDBG_ValueKind)
#define GRDBG_Variable GHOTIIO_RUNTIME_DEBUG(GRDBG_Variable)

/* Functions and the key. */
#define grdbg_alloc GHOTIIO_RUNTIME_DEBUG(grdbg_alloc)
#define grdbg_allocator_default GHOTIIO_RUNTIME_DEBUG(grdbg_allocator_default)
#define grdbg_arena_string GHOTIIO_RUNTIME_DEBUG(grdbg_arena_string)
#define grdbg_arm_for GHOTIIO_RUNTIME_DEBUG(grdbg_arm_for)
#define grdbg_dap_create GHOTIIO_RUNTIME_DEBUG(grdbg_dap_create)
#define grdbg_dap_destroy GHOTIIO_RUNTIME_DEBUG(grdbg_dap_destroy)
#define grdbg_dap_free_json GHOTIIO_RUNTIME_DEBUG(grdbg_dap_free_json)
#define grdbg_dap_handle GHOTIIO_RUNTIME_DEBUG(grdbg_dap_handle)
#define grdbg_dap_notify_finished GHOTIIO_RUNTIME_DEBUG(grdbg_dap_notify_finished)
#define grdbg_dap_notify_stopped GHOTIIO_RUNTIME_DEBUG(grdbg_dap_notify_stopped)
#define grdbg_dap_parse GHOTIIO_RUNTIME_DEBUG(grdbg_dap_parse)
#define grdbg_dap_read_message GHOTIIO_RUNTIME_DEBUG(grdbg_dap_read_message)
#define grdbg_dap_send GHOTIIO_RUNTIME_DEBUG(grdbg_dap_send)
#define grdbg_dap_serve GHOTIIO_RUNTIME_DEBUG(grdbg_dap_serve)
#define grdbg_debugger_allocator GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_allocator)
#define grdbg_debugger_armed GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_armed)
#define grdbg_debugger_attach GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_attach)
#define grdbg_debugger_attach_with_allocator GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_attach_with_allocator)
#define grdbg_debugger_breakpoint_count GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_breakpoint_count)
#define grdbg_debugger_context GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_context)
#define grdbg_debugger_continue GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_continue)
#define grdbg_debugger_disarm GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_disarm)
#define grdbg_debugger_find_variable GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_find_variable)
#define grdbg_debugger_frame GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_frame)
#define grdbg_debugger_frames GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_frames)
#define grdbg_debugger_generation GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_generation)
#define grdbg_debugger_get GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_get)
#define grdbg_debugger_key GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_key)
#define grdbg_debugger_request_pause GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_request_pause)
#define grdbg_debugger_scope GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_scope)
#define grdbg_debugger_scope_count GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_scope_count)
#define grdbg_debugger_set_breakpoints GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_set_breakpoints)
#define grdbg_debugger_step GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_step)
#define grdbg_debugger_stopped GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_stopped)
#define grdbg_debugger_variable GHOTIIO_RUNTIME_DEBUG(grdbg_debugger_variable)
#define grdbg_event_begin GHOTIIO_RUNTIME_DEBUG(grdbg_event_begin)
#define grdbg_event_end GHOTIIO_RUNTIME_DEBUG(grdbg_event_end)
#define grdbg_forget_stop GHOTIIO_RUNTIME_DEBUG(grdbg_forget_stop)
#define grdbg_free GHOTIIO_RUNTIME_DEBUG(grdbg_free)
#define grdbg_free_breakpoints GHOTIIO_RUNTIME_DEBUG(grdbg_free_breakpoints)
#define grdbg_from_core GHOTIIO_RUNTIME_DEBUG(grdbg_from_core)
#define grdbg_invalidate GHOTIIO_RUNTIME_DEBUG(grdbg_invalidate)
#define grdbg_key GHOTIIO_RUNTIME_DEBUG(grdbg_key)
#define grdbg_limits_default GHOTIIO_RUNTIME_DEBUG(grdbg_limits_default)
#define grdbg_limits_resolve GHOTIIO_RUNTIME_DEBUG(grdbg_limits_resolve)
#define grdbg_out_arr_begin GHOTIIO_RUNTIME_DEBUG(grdbg_out_arr_begin)
#define grdbg_out_arr_end GHOTIIO_RUNTIME_DEBUG(grdbg_out_arr_end)
#define grdbg_out_begin GHOTIIO_RUNTIME_DEBUG(grdbg_out_begin)
#define grdbg_out_bool GHOTIIO_RUNTIME_DEBUG(grdbg_out_bool)
#define grdbg_out_discard GHOTIIO_RUNTIME_DEBUG(grdbg_out_discard)
#define grdbg_out_int GHOTIIO_RUNTIME_DEBUG(grdbg_out_int)
#define grdbg_out_key GHOTIIO_RUNTIME_DEBUG(grdbg_out_key)
#define grdbg_out_obj_begin GHOTIIO_RUNTIME_DEBUG(grdbg_out_obj_begin)
#define grdbg_out_obj_end GHOTIIO_RUNTIME_DEBUG(grdbg_out_obj_end)
#define grdbg_out_string GHOTIIO_RUNTIME_DEBUG(grdbg_out_string)
#define grdbg_out_string_n GHOTIIO_RUNTIME_DEBUG(grdbg_out_string_n)
#define grdbg_owned GHOTIIO_RUNTIME_DEBUG(grdbg_owned)
#define grdbg_readable GHOTIIO_RUNTIME_DEBUG(grdbg_readable)
#define grdbg_response_begin GHOTIIO_RUNTIME_DEBUG(grdbg_response_begin)
#define grdbg_response_end GHOTIIO_RUNTIME_DEBUG(grdbg_response_end)
#define grdbg_response_error GHOTIIO_RUNTIME_DEBUG(grdbg_response_error)
#define grdbg_response_ok GHOTIIO_RUNTIME_DEBUG(grdbg_response_ok)
#define grdbg_result_string GHOTIIO_RUNTIME_DEBUG(grdbg_result_string)
#define grdbg_snapshot_ensure GHOTIIO_RUNTIME_DEBUG(grdbg_snapshot_ensure)
#define grdbg_transport_create_fd GHOTIIO_RUNTIME_DEBUG(grdbg_transport_create_fd)
#define grdbg_transport_create_memory GHOTIIO_RUNTIME_DEBUG(grdbg_transport_create_memory)
#define grdbg_transport_destroy GHOTIIO_RUNTIME_DEBUG(grdbg_transport_destroy)
#define grdbg_transport_memory_output GHOTIIO_RUNTIME_DEBUG(grdbg_transport_memory_output)
#define grdbg_transport_valid GHOTIIO_RUNTIME_DEBUG(grdbg_transport_valid)
#define grdbg_version_number GHOTIIO_RUNTIME_DEBUG(grdbg_version_number)
#define grdbg_version_string GHOTIIO_RUNTIME_DEBUG(grdbg_version_string)
#define grdbg_wants_arming GHOTIIO_RUNTIME_DEBUG(grdbg_wants_arming)

/// @endcond

#endif /* GHOTI_IO_GRDBG_NAMESPACE_H */
