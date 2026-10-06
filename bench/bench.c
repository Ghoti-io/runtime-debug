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
 * The benchmark harness (AD-26).
 *
 * Every library ships one from its first commit, so that "performant" is a
 * claim with a way to check it. Besides the calibration case it holds the cases
 * of the debugger:
 *
 *  - a poll with no debugger, and with a debugger attached and unarmed: the
 *    spine's claim (AD-4, AD-15) is that these are the same one load and one
 *    branch, and the two figures sit side by side so that it is checked;
 *  - a poll with a debugger armed by one breakpoint that is never reached, and
 *    by a hundred: the slow path through the YIELD handler, which walks the
 *    innermost frame and compares lines;
 *  - a stop and a resume of the model: the pause, reading the stop and the
 *    innermost frame, continuing and resuming;
 *  - the protocol: parsing one framed request and answering a trivial one, and
 *    answering a stackTrace of 100 frames (encoding is most of it).
 *
 * The calibration case is a fixed amount of integer work that touches no
 * library code, run the same way every real case will be, so a figure from a
 * real case can be read against the machine it was taken on. A budget recorded
 * without the calibration beside it cannot be compared across hosts or
 * compilers.
 *
 * Usage:
 *   bench           run every case: several repeats, report min and median
 *   bench --smoke   run every case once with a tiny workload (what `make
 *                   test` does); proves the harness builds, links and runs
 *
 * No numeric budget is asserted here. AD-26 records budgets once a first
 * measurement of a real case exists.
 */

/* clock_gettime(CLOCK_MONOTONIC) is POSIX, and -std=c17 hides it. A benchmark
 * wants a clock that cannot step backwards, so it asks for it by name. */
#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/runtime-debug/runtime-debug.h>

#include <ghoti.io/runtime-core/runtime-core.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  const char * name;
  /** Performs @p iterations units of work; returns a value derived from all
   *  of it so the compiler cannot discard the loop. */
  uint64_t (*run)(uint64_t iterations);
  uint64_t iterations;       /* per repeat, full run */
  uint64_t smoke_iterations; /* per repeat, --smoke */
} Case;

/* A case that cannot set itself up must not report a short, fast run as a
 * measurement. */
static _Noreturn void setup_failed(const char * what) {
  fprintf(stderr, "bench: setup failed: %s\n", what);
  abort();
}

static double now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* xorshift64: one dependent chain of shifts and xors per step. The chain is
 * serial on purpose, so the figure is latency-bound and does not move with
 * how wide the host's execution units are. */
static uint64_t calibration_run(uint64_t iterations) {
  uint64_t x = 0x9E3779B97F4A7C15ull;
  for (uint64_t i = 0; i < iterations; i++) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
  }
  return x;
}

/* ---- a minimal engine: frames named by a function and an offset ---------- */

static GRCORE_Location bench_locate(
    const GRCORE_Context * context, uint64_t function, uint64_t offset) {
  (void)context;
  (void)function;
  return (GRCORE_Location){"bench.c", (int)offset + 1};
}

static const GRCORE_EngineDescriptor bench_engine = GRCORE_ENGINE_DESCRIPTOR_INIT("bench", NULL,
    bench_locate, NULL, {NULL, NULL, NULL}, {0, 0, 0}, NULL, NULL, NULL, NULL);

typedef struct {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRCORE_EngineId engine;
  GRDBG_Debugger * debugger; /* NULL for the case with none */
} World;

/* A world with `frames` frames on the stack, each at a different place. */
static void world_open(World * w, size_t frames, bool attach) {
  memset(w, 0, sizeof *w);
  if (grcore_group_create(NULL, NULL, &w->group) != GRCORE_OK) {
    setup_failed("group create");
  }
  if (grcore_context_create(w->group, NULL, &w->context) != GRCORE_OK) {
    setup_failed("context create");
  }
  if (grcore_engine_register(w->context, &bench_engine, &w->engine) != GRCORE_OK) {
    setup_failed("engine register");
  }
  GRCORE_Stack * stack = grcore_context_stack(w->context);
  for (size_t i = 0; i < frames; i++) {
    GRCORE_FrameRef ref;
    if (grcore_stack_push(stack, w->engine, 2, &ref) != GRCORE_OK) {
      setup_failed("stack push");
    }
    if (grcore_stack_set_identity(stack, ref, (GRCORE_PollIdentity){i, i + 1}) != GRCORE_OK) {
      setup_failed("set identity");
    }
  }
  if (attach && grdbg_debugger_attach(w->context, NULL, &w->debugger) != GRDBG_OK) {
    setup_failed("attach");
  }
}

static void world_close(World * w) {
  grcore_context_destroy(w->context);
  grcore_group_destroy(w->group);
}

/* The guest: `remaining` polls, then finished; a pause saves nothing because
 * `remaining` is the position. */
static GRCORE_Step poller_entry(GRCORE_Context * context, void * state) {
  uint64_t * remaining = state;
  while (*remaining > 0) {
    (*remaining)--;
    GRCORE_Verdict v = grcore_stack_poll(context, 0, 0);
    if (v == GRCORE_VERDICT_PAUSE) {
      return GRCORE_STEP_PAUSED;
    }
    if (v == GRCORE_VERDICT_UNWIND) {
      return GRCORE_STEP_UNWOUND;
    }
  }
  return GRCORE_STEP_FINISHED;
}

static void set_never_reached(World * w, size_t count) {
  int lines[128];
  uint64_t ids[128];
  if (count > 128) {
    setup_failed("too many breakpoints");
  }
  for (size_t i = 0; i < count; i++) {
    lines[i] = (int)(1000 + i);
  }
  if (grdbg_debugger_set_breakpoints(w->debugger, "other.c", lines, count, ids) != GRDBG_OK) {
    setup_failed("set breakpoints");
  }
}

/* Polls `iterations` times inside one run. */
static uint64_t polls(uint64_t iterations, bool attach, size_t breakpoints) {
  World w;
  world_open(&w, 4, attach);
  if (breakpoints > 0) {
    set_never_reached(&w, breakpoints);
  }
  uint64_t remaining = iterations;
  GRCORE_Outcome outcome;
  if (grcore_run(w.context, poller_entry, &remaining, &outcome) != GRCORE_OK ||
      outcome != GRCORE_OUTCOME_FINISHED) {
    setup_failed("run");
  }
  uint64_t sink = grcore_context_pause_key_count(w.context) + remaining;
  world_close(&w);
  return sink;
}

static uint64_t poll_detached_run(uint64_t iterations) {
  return polls(iterations, false, 0);
}

static uint64_t poll_unarmed_run(uint64_t iterations) {
  return polls(iterations, true, 0);
}

static uint64_t poll_armed_1_run(uint64_t iterations) {
  return polls(iterations, true, 1);
}

static uint64_t poll_armed_100_run(uint64_t iterations) {
  return polls(iterations, true, 100);
}

/* The model's round trip: a breakpoint stops the guest, the host reads the
 * stop and the innermost frame, continues and resumes. An iteration is one
 * stop and one resume. */
static uint64_t stop_resume_run(uint64_t iterations) {
  World w;
  world_open(&w, 4, true);
  int line = 1; /* the place of frame 0's poll: (0, 0) is line 1 */
  uint64_t id;
  /* The poller polls at (0, 0); the engine names that "bench.c", line 1. */
  if (grdbg_debugger_set_breakpoints(w.debugger, "bench.c", &line, 1, &id) != GRDBG_OK) {
    setup_failed("set breakpoint");
  }
  uint64_t remaining = iterations + 1;
  GRCORE_Outcome outcome;
  if (grcore_run(w.context, poller_entry, &remaining, &outcome) != GRCORE_OK ||
      outcome != GRCORE_OUTCOME_PAUSED) {
    setup_failed("first stop");
  }
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    GRDBG_Stop stop;
    GRDBG_Frame frame;
    if (grdbg_debugger_stopped(w.debugger, &stop) != GRDBG_OK ||
        grdbg_debugger_frame(w.debugger, 0, &frame) != GRDBG_OK ||
        grdbg_debugger_continue(w.debugger) != GRDBG_OK ||
        grcore_resume(w.context, &outcome) != GRCORE_OK) {
      setup_failed("stop and resume");
    }
    sink += (uint64_t)frame.line + stop.hit_count;
  }
  world_close(&w);
  return sink;
}

/* ---- the protocol ------------------------------------------------------- */

static char * repeat_request(const char * body, uint64_t count, size_t * out_length) {
  char header[64];
  int h = snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", strlen(body));
  size_t each = (size_t)h + strlen(body);
  char * input = malloc(each * count + 1);
  if (input == NULL) {
    setup_failed("memory for the requests");
  }
  for (uint64_t i = 0; i < count; i++) {
    memcpy(input + each * i, header, (size_t)h);
    memcpy(input + each * i + (size_t)h, body, strlen(body));
  }
  *out_length = each * count;
  return input;
}

/* Serves `count` copies of one request on a world stopped with `frames`
 * frames, and returns how many bytes it answered. */
static uint64_t serve_requests(const char * body, uint64_t count, size_t frames) {
  World w;
  world_open(&w, frames, true);
  /* A stop to read: a pause request, taken by the first poll. */
  if (grdbg_debugger_request_pause(w.debugger) != GRDBG_OK) {
    setup_failed("request pause");
  }
  uint64_t remaining = 1;
  GRCORE_Outcome outcome;
  if (grcore_run(w.context, poller_entry, &remaining, &outcome) != GRCORE_OK ||
      outcome != GRCORE_OUTCOME_PAUSED) {
    setup_failed("stop");
  }
  size_t length;
  char * input = repeat_request(body, count, &length);
  GRDBG_Transport * transport;
  GRDBG_Dap * dap;
  GRDBG_ServeResult result;
  if (grdbg_transport_create_memory(input, length, NULL, &transport) != GRDBG_OK ||
      grdbg_dap_create(w.debugger, transport, NULL, &dap) != GRDBG_OK ||
      grdbg_dap_serve(dap, &result) != GRDBG_OK || result != GRDBG_SERVE_DETACH) {
    setup_failed("serve");
  }
  const uint8_t * out;
  size_t out_length;
  if (grdbg_transport_memory_output(transport, &out, &out_length) != GRDBG_OK || out_length == 0) {
    setup_failed("answers");
  }
  grdbg_dap_destroy(dap);
  grdbg_transport_destroy(transport);
  free(input);
  world_close(&w);
  return out_length;
}

static uint64_t parse_request_run(uint64_t iterations) {
  return serve_requests(
      "{\"seq\":1,\"type\":\"request\",\"command\":\"threads\",\"arguments\":{}}", iterations, 4);
}

static uint64_t stack_trace_100_run(uint64_t iterations) {
  return serve_requests(
      "{\"seq\":1,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}", iterations, 100);
}

static const Case cases[] = {
    {"calibration", calibration_run, 200u * 1000u * 1000u, 1000u * 1000u},
    {"poll-detached", poll_detached_run, 200u * 1000u * 1000u, 100000u},
    {"poll-unarmed", poll_unarmed_run, 200u * 1000u * 1000u, 100000u},
    {"poll-armed-1", poll_armed_1_run, 10u * 1000u * 1000u, 10000u},
    {"poll-armed-100", poll_armed_100_run, 10u * 1000u * 1000u, 10000u},
    {"stop-resume", stop_resume_run, 1u * 1000u * 1000u, 1000u},
    {"parse-request", parse_request_run, 200u * 1000u, 100u},
    {"stacktrace-100", stack_trace_100_run, 5u * 1000u, 5u},
};

#define REPEATS 7

static int compare_double(const void * a, const void * b) {
  double x = *(const double *)a;
  double y = *(const double *)b;
  return (x > y) - (x < y);
}

int main(int argc, char ** argv) {
  int smoke = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--smoke") == 0) {
      smoke = 1;
    } else {
      fprintf(stderr, "bench: unknown argument '%s'\n", argv[i]);
      return 2;
    }
  }

  /* Naming the library's version proves the harness linked the library it
   * claims to measure, and ties every figure to the build that produced it. */
  printf("runtime-debug %s, %s run\n", grdbg_version_string(),
      smoke ? "smoke" : "full");

  int repeats = smoke ? 1 : REPEATS;
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    uint64_t n = smoke ? cases[c].smoke_iterations : cases[c].iterations;
    double ns[REPEATS];
    uint64_t sink = 0;
    for (int r = 0; r < repeats; r++) {
      double start = now_ns();
      sink ^= cases[c].run(n);
      ns[r] = now_ns() - start;
    }
    qsort(ns, (size_t)repeats, sizeof(ns[0]), compare_double);
    double best = ns[0] / (double)n;
    double median = ns[repeats / 2] / (double)n;
    if (!(best > 0.0)) {
      fprintf(stderr, "bench: %s measured no time; the clock is unusable\n",
          cases[c].name);
      return 1;
    }
    printf("%-15s best %10.3f ns/op  median %10.3f ns/op  (%llu ops x %d, "
           "check %016llx)\n",
        cases[c].name, best, median, (unsigned long long)n, repeats,
        (unsigned long long)sink);
  }
  return 0;
}
