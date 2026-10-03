/**
 * @file
 *
 * Shared helpers for the Ghoti.io Runtime-debug unit tests.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRDBG_TEST_HELPERS_H
#define GHOTI_IO_GRDBG_TEST_HELPERS_H

#include <gtest/gtest.h>

#include <ghoti.io/runtime-debug/runtime-debug.h>

#include <ghoti.io/runtime-core/runtime-core.h>

#include "toy_engine.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

/* A base allocator that counts, tracks live blocks, and can fail the Nth
 * allocation call (malloc, calloc or realloc, counted from 1), or every call
 * from the Nth on (`sticky`). The allocation-failure sweeps use it to reach
 * every arm. */
struct TrackingAllocator {
  GRDBG_Allocator vtable;
  long calls = 0;
  long fail_at = 0;    // 0: never fail
  bool sticky = false; // with fail_at: fail every call from the Nth on
  long live = 0;       // successful allocations not yet freed

  TrackingAllocator(const TrackingAllocator &) = delete;
  TrackingAllocator & operator=(const TrackingAllocator &) = delete;
  TrackingAllocator() {
    vtable.ctx = this;
    vtable.malloc_fn = [](void * c, size_t n) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * p = std::malloc(n ? n : 1);
      if (p != nullptr) {
        t->live++;
      }
      return p;
    };
    vtable.calloc_fn = [](void * c, size_t n, size_t m) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * p = std::calloc(n ? n : 1, m ? m : 1);
      if (p != nullptr) {
        t->live++;
      }
      return p;
    };
    vtable.realloc_fn = [](void * c, void * p, size_t n) -> void * {
      auto * t = static_cast<TrackingAllocator *>(c);
      if (t->fails()) {
        return nullptr;
      }
      void * q = std::realloc(p, n ? n : 1);
      if (p == nullptr && q != nullptr) {
        t->live++;
      }
      return q;
    };
    vtable.free_fn = [](void * c, void * p) {
      if (p != nullptr) {
        static_cast<TrackingAllocator *>(c)->live--;
      }
      std::free(p);
    };
  }
  bool fails() {
    calls++;
    return fail_at != 0 && (sticky ? calls >= fail_at : calls == fail_at);
  }
  const GRDBG_Allocator * get() const { return &vtable; }
};

/* One toy program in a group and a context. The group draws on one tracking
 * allocator and the debugger on another, so a test can tell who allocated, and
 * the destructor holds the library to the contract that matters most here:
 * after the context is gone neither holds a live block. */
struct ToyWorld {
  TrackingAllocator group_allocator;
  TrackingAllocator debug_allocator;
  GRCORE_Group * group = nullptr;
  GRCORE_Context * ctx = nullptr;
  toy::Toy toy;
  GRDBG_Debugger * dbg = nullptr;
  std::unique_ptr<observer::Observer> trace;

  explicit ToyWorld(toy::Program program, uint64_t fuel = GRCORE_UNLIMITED) {
    program.finalize();
    toy.program = std::move(program);
    GRCORE_Options * o;
    EXPECT_EQ(grcore_options_create(nullptr, &o), GRCORE_OK);
    grcore_options_set_fuel(o, fuel);
    EXPECT_EQ(grcore_group_create(group_allocator.get(), nullptr, &group), GRCORE_OK);
    EXPECT_EQ(grcore_context_create(group, o, &ctx), GRCORE_OK);
    grcore_options_destroy(o);
    EXPECT_EQ(grcore_context_register(ctx, &toy::kToyKey, &toy), GRCORE_OK);
    EXPECT_EQ(grcore_engine_register(ctx, &toy::kDescriptor, &toy.engine), GRCORE_OK);
  }
  ToyWorld(const ToyWorld &) = delete;
  ToyWorld & operator=(const ToyWorld &) = delete;
  ~ToyWorld() {
    trace.reset();
    EXPECT_EQ(grcore_context_destroy(ctx), GRCORE_OK);
    EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
    EXPECT_EQ(group_allocator.live, 0);
    EXPECT_EQ(debug_allocator.live, 0);
  }

  GRDBG_Result attach(const GRDBG_Limits * limits = nullptr) {
    return grdbg_debugger_attach_with_allocator(ctx, debug_allocator.get(), limits, &dbg);
  }
  /// Every poll takes the slow path and is recorded, whatever else is true.
  void trace_polls() {
    trace = std::make_unique<observer::Observer>();
    EXPECT_EQ(trace->attach(ctx), GRCORE_OK);
  }
  GRCORE_Result run(GRCORE_Outcome * outcome) {
    return grcore_run(ctx, toy::entry, &toy, outcome);
  }
  GRCORE_Result resume(GRCORE_Outcome * outcome) {
    return grcore_resume(ctx, outcome);
  }
  /// Runs to the first pause and requires one.
  void run_to_pause() {
    GRCORE_Outcome outcome;
    ASSERT_EQ(run(&outcome), GRCORE_OK);
    ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  }
  /// Resumes and requires another pause.
  void resume_to_pause() {
    GRCORE_Outcome outcome;
    ASSERT_EQ(resume(&outcome), GRCORE_OK);
    ASSERT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  }
  /// Resumes and requires that the program finishes.
  void resume_to_end() {
    GRCORE_Outcome outcome;
    ASSERT_EQ(resume(&outcome), GRCORE_OK);
    ASSERT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  }
};

/// The innermost frame of the stop, as the model records it.
struct Here {
  std::string file;
  int line = 0;
  size_t total = 0;
};

inline Here here(GRDBG_Debugger * dbg) {
  Here h;
  GRDBG_Frame frame;
  size_t total = 0, available = 0;
  EXPECT_EQ(grdbg_debugger_frames(dbg, &total, &available), GRDBG_OK);
  EXPECT_EQ(grdbg_debugger_frame(dbg, 0, &frame), GRDBG_OK);
  h.file = frame.file != nullptr ? frame.file : "";
  h.line = frame.line;
  h.total = total;
  return h;
}

/// The stop reason the model reports for the pause in progress.
inline GRDBG_StopReason stop_reason(GRDBG_Debugger * dbg) {
  GRDBG_Stop stop;
  EXPECT_EQ(grdbg_debugger_stopped(dbg, &stop), GRDBG_OK);
  return stop.reason;
}

/// A program of the shape most tests want.
///
///   main (a.toy)            f (a.toy)
///    1: SET x 3              10: SET r 1
///    2: CALL f               11: ADD r 1
///    3: PRINT x              12: PRINT r
///    4: SET y 7
///    5: PRINT y
inline toy::Program basic_program() {
  toy::Program p;
  toy::Function & main = p.fn("main", "a.toy", {"x", "y"});
  main.set(1, "x", 3).call(2, "f").print(3, "x").set(4, "y", 7).print(5, "y");
  toy::Function & f = p.fn("f", "a.toy", {"r"});
  f.set(10, "r", 1).add(11, "r", 1).print(12, "r");
  return p;
}

/// Every test file ends with this: each is its own executable.
#define GRDBG_TEST_MAIN()                                                      \
  int main(int argc, char ** argv) {                                           \
    ::testing::InitGoogleTest(&argc, argv);                                    \
    return RUN_ALL_TESTS();                                                    \
  }

#endif
