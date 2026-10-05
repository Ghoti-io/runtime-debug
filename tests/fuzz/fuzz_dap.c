/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */

/**
 * @file
 *
 * The fuzz harness for the Debug Adapter Protocol reader and dispatcher
 * (AD-16): arbitrary bytes go in through a memory transport, against a context
 * paused three frames deep in an engine with scopes, so that every request has
 * something to read.
 *
 * The first byte is an options byte that picks the limits, so one harness
 * covers the framing and the JSON at every cap: a header block of 16 to 1024
 * bytes, a message of 32 bytes to 4 MiB, a JSON depth of 2 to 64, and
 * one-frame and one-variable pages. The adapter is served until it asks the
 * host to proceed, fails or detaches, up to a fixed number of requests; the
 * context stays paused, since nothing here resumes it, so a `continue` leaves
 * the session where it was and later requests are read too.
 *
 * The checks are the adapter's promises: it never crashes or leaks, and
 * everything it writes is a whole framed message that parses as an object with
 * a rising `seq` and, for a response, a `request_seq`, a `success` and a
 * `command`. A message that is not is a bug and aborts.
 */

#include <ghoti.io/runtime-debug/runtime-debug.h>

#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/text/json.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 3u
#define SLOTS 4u

/* An engine of three frames with four slots each, one scope of four locals. */
static GRCORE_Location fuzz_locate(
    const GRCORE_Context * context, uint64_t function, uint64_t offset) {
  (void)context;
  (void)function;
  return (GRCORE_Location){"fuzz.src", (int)offset};
}

static size_t fuzz_scope_count(const GRCORE_AbstractFrame * frame) {
  (void)frame;
  return 1;
}

static GRCORE_Result fuzz_scope(
    const GRCORE_AbstractFrame * frame, size_t index, GRCORE_ScopeInfo * out) {
  (void)frame;
  if (index != 0) {
    return GRCORE_ERR_INVALID;
  }
  *out = (GRCORE_ScopeInfo){GRCORE_SCOPE_LOCAL, "Locals", SLOTS};
  return GRCORE_OK;
}

static GRCORE_Result fuzz_variable(const GRCORE_AbstractFrame * frame,
    size_t scope, size_t index, GRCORE_Variable * out) {
  static const char * names[SLOTS] = {"a", "b", "c", "d"};
  uint64_t value;
  if (scope != 0 || index >= SLOTS ||
      grcore_frame_slot(frame, index, NULL, &value) != GRCORE_OK) {
    return GRCORE_ERR_INVALID;
  }
  *out = (GRCORE_Variable){names[index], GRCORE_SLOT_VALUE, value};
  return GRCORE_OK;
}

static const GRCORE_EngineDescriptor fuzz_engine = GRCORE_ENGINE_DESCRIPTOR_INIT("fuzz", NULL, fuzz_locate,
    NULL, {fuzz_scope_count, fuzz_scope, fuzz_variable}, {0, 0, 0}, NULL, NULL);

static GRCORE_Step fuzz_entry(GRCORE_Context * context, void * state) {
  (void)state;
  GRCORE_Stack * stack = grcore_context_stack(context);
  if (grcore_stack_frame_count(stack) == 0) {
    for (size_t i = 0; i < FRAMES; i++) {
      GRCORE_FrameRef ref;
      if (grcore_stack_push(stack, 1, SLOTS, &ref) != GRCORE_OK) {
        return GRCORE_STEP_UNWOUND;
      }
      grcore_stack_set_identity(stack, ref, (GRCORE_PollIdentity){i, 10 + i});
      uint64_t * slots = grcore_stack_slots(stack, ref);
      for (size_t s = 0; s < SLOTS; s++) {
        slots[s] = (i + 1) * 100 + s;
      }
    }
  }
  grcore_context_charge_fuel(context, 1);
  GRCORE_Verdict v = grcore_stack_poll(context, 7, 99);
  return v == GRCORE_VERDICT_PAUSE ? GRCORE_STEP_PAUSED : GRCORE_STEP_UNWOUND;
}

/* Everything written is whole messages with the shapes the protocol fixes. */
static void check_output(const uint8_t * out, size_t length) {
  size_t pos = 0;
  int64_t last_seq = 0;
  while (pos < length) {
    static const char prefix[] = "Content-Length: ";
    if (length - pos < sizeof prefix - 1 ||
        memcmp(out + pos, prefix, sizeof prefix - 1) != 0) {
      abort();
    }
    pos += sizeof prefix - 1;
    size_t n = 0;
    size_t digits = 0;
    while (pos < length && out[pos] >= '0' && out[pos] <= '9') {
      n = n * 10 + (size_t)(out[pos] - '0');
      pos++;
      digits++;
    }
    if (digits == 0 || length - pos < 4 || memcmp(out + pos, "\r\n\r\n", 4) != 0) {
      abort();
    }
    pos += 4;
    if (n > length - pos) {
      abort();
    }
    GTEXT_JSON_Value * message = gtext_json_parse((const char *)out + pos, n, NULL, NULL);
    if (message == NULL || gtext_json_typeof(message) != GTEXT_JSON_OBJECT) {
      abort();
    }
    int64_t seq = 0;
    const GTEXT_JSON_Value * v = gtext_json_object_get(message, "seq", 3);
    if (v == NULL || gtext_json_get_i64(v, &seq) != GTEXT_JSON_OK || seq != last_seq + 1) {
      abort();
    }
    last_seq = seq;
    const char * type;
    size_t type_length;
    v = gtext_json_object_get(message, "type", 4);
    if (v == NULL || gtext_json_get_string(v, &type, &type_length) != GTEXT_JSON_OK) {
      abort();
    }
    if (type_length == 8 && memcmp(type, "response", 8) == 0) {
      bool success;
      int64_t request_seq;
      const GTEXT_JSON_Value * rs = gtext_json_object_get(message, "request_seq", 11);
      const GTEXT_JSON_Value * ok = gtext_json_object_get(message, "success", 7);
      const GTEXT_JSON_Value * command = gtext_json_object_get(message, "command", 7);
      const char * c;
      size_t cl;
      if (rs == NULL || ok == NULL || command == NULL ||
          gtext_json_get_i64(rs, &request_seq) != GTEXT_JSON_OK ||
          gtext_json_get_bool(ok, &success) != GTEXT_JSON_OK ||
          gtext_json_get_string(command, &c, &cl) != GTEXT_JSON_OK) {
        abort();
      }
      if (!success && gtext_json_object_get(message, "message", 7) == NULL) {
        abort();
      }
    } else if (!(type_length == 5 && memcmp(type, "event", 5) == 0)) {
      abort();
    }
    gtext_json_free(message);
    pos += n;
  }
}

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size == 0) {
    return 0;
  }
  uint8_t options = data[0];
  data++;
  size--;

  GRDBG_Limits limits;
  grdbg_limits_default(&limits);
  static const size_t header_bytes[4] = {16, 64, 256, 1024};
  static const size_t message_bytes[4] = {32, 256, 4096, 4u * 1024u * 1024u};
  static const size_t depths[4] = {2, 4, 16, 64};
  limits.max_header_bytes = header_bytes[options & 3u];
  limits.max_message_bytes = message_bytes[(options >> 2) & 3u];
  limits.max_json_depth = depths[(options >> 4) & 3u];
  if (options & 0x40u) {
    limits.max_frames = 1;
  }
  if (options & 0x80u) {
    limits.max_variables = 1;
  }

  GRCORE_Options * context_options;
  GRCORE_Group * group;
  GRCORE_Context * context;
  if (grcore_options_create(NULL, &context_options) != GRCORE_OK) {
    return 0;
  }
  grcore_options_set_fuel(context_options, 0);
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK) {
    grcore_options_destroy(context_options);
    return 0;
  }
  if (grcore_context_create(group, context_options, &context) != GRCORE_OK) {
    grcore_options_destroy(context_options);
    grcore_group_destroy(group);
    return 0;
  }
  grcore_options_destroy(context_options);

  GRCORE_Outcome outcome;
  GRCORE_EngineId id;
  GRDBG_Debugger * debugger;
  GRDBG_Transport * transport;
  GRDBG_Dap * dap;
  if (grcore_engine_register(context, &fuzz_engine, &id) == GRCORE_OK && id == 1 &&
      grcore_run(context, fuzz_entry, NULL, &outcome) == GRCORE_OK &&
      outcome == GRCORE_OUTCOME_PAUSED &&
      grdbg_debugger_attach(context, &limits, &debugger) == GRDBG_OK &&
      grdbg_transport_create_memory(data, size, NULL, &transport) == GRDBG_OK) {
    if (grdbg_dap_create(debugger, transport, &limits, &dap) == GRDBG_OK) {
      for (int i = 0; i < 64; i++) {
        GRDBG_ServeResult result;
        if (grdbg_dap_serve(dap, &result) != GRDBG_OK || result != GRDBG_SERVE_RESUME) {
          break;
        }
      }
      (void)grdbg_dap_notify_stopped(dap);
      (void)grdbg_dap_notify_finished(dap, 0);
      const uint8_t * out;
      size_t length;
      if (grdbg_transport_memory_output(transport, &out, &length) == GRDBG_OK) {
        check_output(out, length);
      }
      grdbg_dap_destroy(dap);
    }
    grdbg_transport_destroy(transport);
  }
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return 0;
}
