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
 * A debug session from a host's side: a loopback socket, a client that plays a
 * scripted Debug Adapter Protocol session on it, and a guest that stops at a
 * breakpoint and a step.
 *
 * The "guest" is nine statements of three lines each, a loop unrolled: line 10
 * adds `i` to `sum`, line 11 increments `i`, line 12 is a no-op. It keeps its
 * whole position in a frame on the guest stack, as a real engine does, and
 * polls before every statement with `grcore_stack_poll`, so the engine
 * descriptor's `locate` can name the line a debugger stops at. The point is
 * the host's loop, not the guest:
 *
 *   1. serve the client until `configurationDone` (breakpoints are set first);
 *   2. run the context;
 *   3. at every pause, `grdbg_dap_notify_stopped`, `grdbg_dap_serve`, and then
 *      resume, or end the run if the client asked;
 *   4. when the run finishes, `grdbg_dap_notify_finished`, and serve once more
 *      so that the client's `disconnect` is answered.
 *
 * The library never listens or accepts: this host binds the socket and
 * accepts the connection, and hands the descriptor over as a transport. It
 * runs the client on a second thread, and prints the conversation, so the
 * output is the DAP session that VS Code would have had.
 *
 * Build and run with `make examples`.
 */

#ifdef _WIN32
/* The example hands a connected socket to grdbg_transport_create_fd, which is a
 * stub on Windows. Exit status 77 is "skipped": the Makefile counts it and
 * does not fail. */
#include <stdio.h>

int main(void) {
  printf("SKIP: the descriptor transport is not available on Windows\n");
  return 77;
}
#else

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/runtime-debug/runtime-debug.h>

#include <ghoti.io/runtime-core/runtime-core.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Checked in every build: an example that asserts nothing under NDEBUG is not
 * an example of anything. */
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "dap_session: check failed at line %d: %s\n", __LINE__, \
          #condition);                                                         \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define STATEMENTS 9u

/* ---- the guest ---------------------------------------------------------- */

/* A frame's slots: i, sum, the next statement, and whether its poll is taken. */
enum { SLOT_I, SLOT_SUM, SLOT_PC, SLOT_POLLED, SLOT_COUNT };

static GRCORE_Location locate(
    const GRCORE_Context * context, uint64_t function, uint64_t offset) {
  (void)context;
  (void)function;
  return (GRCORE_Location){"count.src", (int)(10 + offset % 3)};
}

static size_t scope_count(const GRCORE_AbstractFrame * frame) {
  (void)frame;
  return 1;
}

static GRCORE_Result scope(
    const GRCORE_AbstractFrame * frame, size_t index, GRCORE_ScopeInfo * out) {
  (void)frame;
  if (index != 0) {
    return GRCORE_ERR_INVALID;
  }
  *out = (GRCORE_ScopeInfo){GRCORE_SCOPE_LOCAL, "Locals", 2};
  return GRCORE_OK;
}

static GRCORE_Result variable(const GRCORE_AbstractFrame * frame, size_t scope_index,
    size_t index, GRCORE_Variable * out) {
  static const char * names[2] = {"i", "sum"};
  uint64_t value;
  if (scope_index != 0 || index >= 2 ||
      grcore_frame_slot(frame, index, NULL, &value) != GRCORE_OK) {
    return GRCORE_ERR_INVALID;
  }
  *out = (GRCORE_Variable){names[index], GRCORE_SLOT_VALUE, value};
  return GRCORE_OK;
}

static size_t inspect(const GRCORE_Context * context, GRCORE_SlotKind kind,
    uint64_t value, char * buffer, size_t size) {
  (void)context;
  (void)kind;
  return (size_t)snprintf(buffer, size, "%llu", (unsigned long long)value);
}

static const GRCORE_EngineDescriptor engine = {"count", NULL, locate, inspect,
    {scope_count, scope, variable}, {0, 0, 0}, NULL, NULL};

typedef struct {
  GRCORE_EngineId id;
  int started;
} Guest;

static GRCORE_Step guest_entry(GRCORE_Context * context, void * state) {
  Guest * guest = state;
  GRCORE_Stack * stack = grcore_context_stack(context);
  if (!guest->started) {
    guest->started = 1;
    GRCORE_FrameRef frame;
    CHECK(grcore_stack_push(stack, guest->id, SLOT_COUNT, &frame) == GRCORE_OK);
  }
  for (;;) {
    GRCORE_FrameRef top = grcore_stack_top(stack);
    uint64_t * slots = grcore_stack_slots(stack, top);
    uint64_t pc = slots[SLOT_PC];
    if (pc >= STATEMENTS) {
      return GRCORE_STEP_FINISHED;
    }
    if (slots[SLOT_POLLED] == 0) {
      GRCORE_Verdict verdict = grcore_stack_poll(context, 0, pc);
      slots = grcore_stack_slots(stack, top); /* a poll is a GC point */
      if (verdict == GRCORE_VERDICT_PAUSE) {
        slots[SLOT_POLLED] = 1; /* the poll before this statement is taken */
        return GRCORE_STEP_PAUSED;
      }
      if (verdict == GRCORE_VERDICT_UNWIND) {
        return GRCORE_STEP_UNWOUND;
      }
    } else {
      slots[SLOT_POLLED] = 0;
    }
    if (pc % 3 == 0) {
      slots[SLOT_SUM] += slots[SLOT_I];
    } else if (pc % 3 == 1) {
      slots[SLOT_I] += 1;
    }
    slots[SLOT_PC] = pc + 1;
  }
}

/* ---- the client --------------------------------------------------------- */

typedef struct {
  struct sockaddr_in address;
  char * transcript; /* what was said, both ways, in order */
  size_t length;
  size_t capacity;
} Client;

static void note(Client * c, const char * direction, const char * text, size_t n) {
  size_t need = c->length + n + 8;
  if (need > c->capacity) {
    c->capacity = need * 2;
    c->transcript = realloc(c->transcript, c->capacity);
    CHECK(c->transcript != NULL);
  }
  c->length += (size_t)sprintf(c->transcript + c->length, "%s ", direction);
  memcpy(c->transcript + c->length, text, n);
  c->length += n;
  c->transcript[c->length++] = '\n';
}

static const char * script[] = {
    "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"count\",\"linesStartAt1\":true}}",
    "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{}}",
    "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"count.src\"},\"breakpoints\":[{\"line\":11}]}}",
    "{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}",
    /* stopped: breakpoint, line 11, i = 0 */
    "{\"seq\":5,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}",
    "{\"seq\":6,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":1}}",
    "{\"seq\":7,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":1}}",
    "{\"seq\":8,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":1}}",
    /* stopped: step, line 12 */
    "{\"seq\":9,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}",
    /* stopped: breakpoint, line 11, i = 1 */
    "{\"seq\":10,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"count.src\"},\"breakpoints\":[]}}",
    "{\"seq\":11,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}",
    /* the guest finishes: exited, terminated */
    "{\"seq\":12,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{\"terminateDebuggee\":false}}",
};

static void send_all(int fd, const char * bytes, size_t n) {
  while (n > 0) {
    ssize_t sent = send(fd, bytes, n, MSG_NOSIGNAL);
    CHECK(sent > 0);
    bytes += sent;
    n -= (size_t)sent;
  }
}

/* Reads one framed message into `body`; returns its length, or 0 at the end of
 * the stream. */
static size_t read_message(int fd, char ** buffer, size_t * have, char * body, size_t cap) {
  for (;;) {
    char * end = NULL;
    for (size_t i = 0; i + 4 <= *have; i++) {
      if (memcmp(*buffer + i, "\r\n\r\n", 4) == 0) {
        end = *buffer + i + 4;
        break;
      }
    }
    if (end != NULL) {
      size_t n = (size_t)strtoul(*buffer + strlen("Content-Length: "), NULL, 10);
      size_t header = (size_t)(end - *buffer);
      if (*have >= header + n) {
        CHECK(n < cap);
        memcpy(body, end, n);
        body[n] = '\0';
        memmove(*buffer, *buffer + header + n, *have - header - n);
        *have -= header + n;
        return n;
      }
    }
    char chunk[1024];
    ssize_t got = read(fd, chunk, sizeof chunk);
    if (got <= 0) {
      return 0;
    }
    *buffer = realloc(*buffer, *have + (size_t)got);
    CHECK(*buffer != NULL);
    memcpy(*buffer + *have, chunk, (size_t)got);
    *have += (size_t)got;
  }
}

static void * client_main(void * argument) {
  Client * c = argument;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  CHECK(connect(fd, (struct sockaddr *)&c->address, sizeof c->address) == 0);
  char * pending = NULL;
  size_t have = 0;
  char body[8192];
  for (size_t i = 0; i < sizeof script / sizeof script[0]; i++) {
    char header[64];
    int h = snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", strlen(script[i]));
    send_all(fd, header, (size_t)h);
    send_all(fd, script[i], strlen(script[i]));
    note(c, ">>", script[i], strlen(script[i]));
    /* Read until this request's response; events on the way are noted too. */
    char marker[32];
    snprintf(marker, sizeof marker, "\"request_seq\":%zu,", i + 1);
    for (;;) {
      size_t n = read_message(fd, &pending, &have, body, sizeof body);
      CHECK(n > 0);
      note(c, "<<", body, n);
      if (strstr(body, marker) != NULL) {
        break;
      }
    }
  }
  /* Whatever the host still says (the exit), until it closes the stream. */
  shutdown(fd, SHUT_WR);
  for (;;) {
    size_t n = read_message(fd, &pending, &have, body, sizeof body);
    if (n == 0) {
      break;
    }
    note(c, "<<", body, n);
  }
  close(fd);
  free(pending);
  return NULL;
}

/* ---- the host ----------------------------------------------------------- */

int main(void) {
  /* The host binds and accepts. The library never does. */
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(listener >= 0);
  Client client;
  memset(&client, 0, sizeof client);
  client.address.sin_family = AF_INET;
  client.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  client.address.sin_port = 0;
  CHECK(bind(listener, (struct sockaddr *)&client.address, sizeof client.address) == 0);
  CHECK(listen(listener, 1) == 0);
  socklen_t length = sizeof client.address;
  CHECK(getsockname(listener, (struct sockaddr *)&client.address, &length) == 0);
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, client_main, &client) == 0);
  int connection = accept(listener, NULL, NULL);
  CHECK(connection >= 0);

  /* The context, the guest, the debugger, and the session on the connection. */
  GRCORE_Group * group;
  GRCORE_Context * context;
  Guest guest = {0, 0};
  CHECK(grcore_group_create(NULL, NULL, &group) == GRCORE_OK);
  CHECK(grcore_context_create(group, NULL, &context) == GRCORE_OK);
  CHECK(grcore_engine_register(context, &engine, &guest.id) == GRCORE_OK);
  GRDBG_Debugger * debugger;
  GRDBG_Transport * transport;
  GRDBG_Dap * dap;
  CHECK(grdbg_debugger_attach(context, NULL, &debugger) == GRDBG_OK);
  CHECK(grdbg_transport_create_fd(connection, connection, NULL, &transport) == GRDBG_OK);
  CHECK(grdbg_dap_create(debugger, transport, NULL, &dap) == GRDBG_OK);

  /* 1. Serve until the client has set its breakpoints and says go. */
  GRDBG_ServeResult serve;
  CHECK(grdbg_dap_serve(dap, &serve) == GRDBG_OK && serve == GRDBG_SERVE_RESUME);

  /* 2, 3. Run, and at every pause: notify, serve, resume. */
  GRCORE_Outcome outcome;
  CHECK(grcore_run(context, guest_entry, &guest, &outcome) == GRCORE_OK);
  int stops = 0;
  while (outcome == GRCORE_OUTCOME_PAUSED) {
    stops++;
    CHECK(grdbg_dap_notify_stopped(dap) == GRDBG_OK);
    CHECK(grdbg_dap_serve(dap, &serve) == GRDBG_OK);
    if (serve == GRDBG_SERVE_TERMINATE) {
      CHECK(grcore_context_terminate(context) == GRCORE_OK);
    }
    CHECK(grcore_resume(context, &outcome) == GRCORE_OK || serve == GRDBG_SERVE_TERMINATE);
  }

  /* 4. The run finished: say so, and answer the client's disconnect. */
  CHECK(grdbg_dap_notify_finished(dap, 0) == GRDBG_OK);
  CHECK(grdbg_dap_serve(dap, &serve) == GRDBG_OK && serve == GRDBG_SERVE_DETACH);

  GRCORE_FrameRef top = grcore_stack_top(grcore_context_stack(context));
  uint64_t sum = 0;
  CHECK(grcore_stack_slot_get(grcore_context_stack(context), top, SLOT_SUM, &sum) == GRCORE_OK);

  grdbg_dap_destroy(dap);
  grdbg_transport_destroy(transport);
  close(connection); /* the client reads to the end of the stream */
  CHECK(pthread_join(thread, NULL) == 0);
  close(listener);
  grcore_context_destroy(context);
  grcore_group_destroy(group);

  fputs(client.transcript, stdout);
  free(client.transcript);
  printf("\nthe guest stopped %d times and finished with sum %llu\n", stops,
      (unsigned long long)sum);
  /* i runs 0, 1, 2, one pass of three lines each, and line 10 adds it: 0 + 1 +
   * 2 = 3. Three stops: the breakpoint at line 11 with i = 0, the step to line
   * 12, and the breakpoint again with i = 1; the client then clears its
   * breakpoints, so the third pass runs free. */
  CHECK(stops == 3);
  CHECK(sum == 3);
  return 0;
}

#endif /* _WIN32 */
