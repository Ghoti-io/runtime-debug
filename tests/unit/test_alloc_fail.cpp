/**
 * @file
 *
 * Allocation failure is a code path: the Nth allocation of the debugger's
 * allocator (and, for the parts that reach it, the group's) fails for N from 1
 * until nothing fails, and each operation either succeeds or reports
 * ::GRDBG_ERR_OOM with the state it had. Every world's destructor then holds
 * both allocators to zero live blocks.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "dap_helpers.h"

namespace {

/// Runs `body(n)` for n = 1, 2, ... until one run has no allocation to fail
/// (the Nth call is never reached). Returns how many failures were exercised.
template <typename Body>
int sweep(Body && body) {
  int failures = 0;
  for (long n = 1; n < 2000; ++n) {
    bool reached = body(n);
    if (!reached) {
      return failures;
    }
    ++failures;
  }
  ADD_FAILURE() << "the sweep never ran out of allocations";
  return failures;
}

toy::Program program() {
  toy::Program p;
  p.fn("main", "a.toy", {"x", "y"}).set(1, "x", 3).call(2, "f").print(3, "x");
  p.fn("f", "a.toy", {"r"}).set(10, "r", 1).add(11, "r", 1).print(12, "r");
  return p;
}

}  // namespace

TEST(AllocFail, AttachIsOomOrWholeAndAFailedAttachCanBeRetried) {
  int failures = sweep([&](long n) {
    ToyWorld w(program());
    w.debug_allocator.fail_at = n;
    GRDBG_Result r = w.attach();
    bool reached = w.debug_allocator.calls >= n;
    w.debug_allocator.fail_at = 0;
    if (r == GRDBG_OK) {
      EXPECT_EQ(grdbg_debugger_get(w.ctx), w.dbg);
    }
    else {
      EXPECT_EQ(r, GRDBG_ERR_OOM);
      EXPECT_EQ(grdbg_debugger_get(w.ctx), nullptr);
      EXPECT_EQ(w.attach(), GRDBG_OK);  // and then it works
    }
    int line = 2;
    uint64_t id;
    EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
    w.run_to_pause();
    return reached;
  });
  EXPECT_GE(failures, 1);
}

TEST(AllocFail, AttachWhenTheGroupsAllocatorFailsIsOomToo) {
  int failures = sweep([&](long n) {
    ToyWorld w(program());
    w.group_allocator.fail_at = n + w.group_allocator.calls;
    GRDBG_Result r = w.attach();
    bool reached = w.group_allocator.calls >= w.group_allocator.fail_at;
    w.group_allocator.fail_at = 0;
    if (r != GRDBG_OK) {
      EXPECT_EQ(r, GRDBG_ERR_OOM);
      EXPECT_EQ(grdbg_debugger_get(w.ctx), nullptr);
      EXPECT_EQ(w.attach(), GRDBG_OK);
    }
    return reached;
  });
  EXPECT_GE(failures, 1);
}

TEST(AllocFail, SetBreakpointsIsOomOrWholeAndChangesNothingOnOom) {
  int failures = sweep([&](long n) {
    ToyWorld w(program());
    EXPECT_EQ(w.attach(), GRDBG_OK);
    int first[] = {3, 12};
    uint64_t ids[3];
    EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "b.toy", first, 2, ids), GRDBG_OK);
    w.debug_allocator.calls = 0;
    w.debug_allocator.fail_at = n;
    int lines[] = {2, 11, 12};
    GRDBG_Result r = grdbg_debugger_set_breakpoints(w.dbg, "a.toy", lines, 3, ids);
    bool reached = w.debug_allocator.calls >= n;
    w.debug_allocator.fail_at = 0;
    if (r == GRDBG_OK) {
      EXPECT_EQ(grdbg_debugger_breakpoint_count(w.dbg), 5u);
    }
    else {
      EXPECT_EQ(r, GRDBG_ERR_OOM);
      EXPECT_EQ(grdbg_debugger_breakpoint_count(w.dbg), 2u);
      // Still the b.toy ones, so the program is not stopped at a.toy lines.
      GRCORE_Outcome outcome;
      EXPECT_EQ(w.run(&outcome), GRCORE_OK);
      EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
    }
    return reached;
  });
  EXPECT_GE(failures, 2);
}

TEST(AllocFail, ReadingAStopIsOomOrWholeAndCanBeRepeated) {
  int failures = sweep([&](long n) {
    ToyWorld w(program());
    EXPECT_EQ(w.attach(), GRDBG_OK);
    int line = 11;
    uint64_t id;
    EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
    w.run_to_pause();
    w.debug_allocator.calls = 0;
    w.debug_allocator.fail_at = n;
    GRDBG_Result results[4];
    size_t total = 0, available = 0;
    GRDBG_Scope scope;
    GRDBG_Variable v;
    results[0] = grdbg_debugger_frames(w.dbg, &total, &available);
    results[1] = grdbg_debugger_scope(w.dbg, 1, 0, &scope);
    results[2] = grdbg_debugger_variable(w.dbg, 0, 0, 0, &v);
    bool found = false;
    results[3] = grdbg_debugger_find_variable(w.dbg, 1, "x", &found, &v);
    bool reached = w.debug_allocator.calls >= n;
    w.debug_allocator.fail_at = 0;
    for (GRDBG_Result r : results) {
      EXPECT_TRUE(r == GRDBG_OK || r == GRDBG_ERR_OOM) << grdbg_result_string(r);
    }
    // After a failure, the same reads succeed and say the same things.
    EXPECT_EQ(grdbg_debugger_frames(w.dbg, &total, &available), GRDBG_OK);
    EXPECT_EQ(total, 2u);
    EXPECT_EQ(grdbg_debugger_scope(w.dbg, 1, 0, &scope), GRDBG_OK);
    EXPECT_STREQ(scope.name, "Locals");
    EXPECT_EQ(grdbg_debugger_variable(w.dbg, 0, 0, 0, &v), GRDBG_OK);
    EXPECT_STREQ(v.text, "1");
    return reached;
  });
  EXPECT_GE(failures, 2);
}

TEST(AllocFail, AStackTraceResponseIsAnAnswerOrOomAndTheSessionGoesOn) {
  dap::Script script;
  script.request("stackTrace").request("scopes", R"({"frameId":1})").request("variables", R"({"variablesReference":1})")
      .request("configurationDone");
  std::string expected;
  {
    ToyWorld w(program());
    ASSERT_EQ(w.attach(), GRDBG_OK);
    dap::Wire wire;
    wire.input = script.bytes();
    GRDBG_Transport t = wire.transport();
    GRDBG_Dap * s;
    ASSERT_EQ(grdbg_dap_create(w.dbg, &t, nullptr, &s), GRDBG_OK);
    int line = 11;
    uint64_t id;
    ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
    w.run_to_pause();
    GRDBG_ServeResult r;
    ASSERT_EQ(grdbg_dap_serve(s, &r), GRDBG_OK);
    expected = wire.output;
    grdbg_dap_destroy(s);
  }
  int failures = sweep([&](long n) {
    ToyWorld w(program());
    EXPECT_EQ(w.attach(), GRDBG_OK);
    dap::Wire wire;
    wire.input = script.bytes();
    GRDBG_Transport t = wire.transport();
    GRDBG_Dap * s;
    EXPECT_EQ(grdbg_dap_create(w.dbg, &t, nullptr, &s), GRDBG_OK);
    int line = 11;
    uint64_t id;
    EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
    w.run_to_pause();
    w.debug_allocator.calls = 0;
    w.debug_allocator.fail_at = n;
    GRDBG_ServeResult r = GRDBG_SERVE_DETACH;
    GRDBG_Result result = grdbg_dap_serve(s, &r);
    bool reached = w.debug_allocator.calls >= n;
    w.debug_allocator.fail_at = 0;
    std::string leftover;
    std::vector<std::string> bodies = dap::split(wire.output, &leftover);
    EXPECT_EQ(leftover, "");  // whatever was written is whole messages
    if (result == GRDBG_OK) {
      // Either nothing failed, or the failure was met by an error response.
      if (wire.output != expected) {
        bool some_error = false;
        for (const std::string & b : bodies) {
          dap::Msg m(b);
          some_error = some_error || (m.is_response() && !m.success());
        }
        EXPECT_TRUE(some_error);
      }
    }
    else {
      EXPECT_EQ(result, GRDBG_ERR_OOM);
      // The session is not over: it serves on.
      GRDBG_Result again = grdbg_dap_serve(s, &r);
      EXPECT_TRUE(again == GRDBG_OK || again == GRDBG_ERR_OOM || again == GRDBG_ERR_FORMAT) << again;
    }
    grdbg_dap_destroy(s);
    return reached;
  });
  EXPECT_GE(failures, 10);
}

TEST(AllocFail, TheStopEventAndTheFinishedEventsAreOomOrWhole) {
  int failures = sweep([&](long n) {
    ToyWorld w(program());
    EXPECT_EQ(w.attach(), GRDBG_OK);
    dap::Wire wire;
    GRDBG_Transport t = wire.transport();
    GRDBG_Dap * s;
    EXPECT_EQ(grdbg_dap_create(w.dbg, &t, nullptr, &s), GRDBG_OK);
    int line = 11;
    uint64_t id;
    EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
    w.run_to_pause();
    w.debug_allocator.calls = 0;
    w.debug_allocator.fail_at = n;
    GRDBG_Result a = grdbg_dap_notify_stopped(s);
    GRDBG_Result b = grdbg_dap_notify_finished(s, 0);
    bool reached = w.debug_allocator.calls >= n;
    w.debug_allocator.fail_at = 0;
    EXPECT_TRUE(a == GRDBG_OK || a == GRDBG_ERR_OOM);
    EXPECT_TRUE(b == GRDBG_OK || b == GRDBG_ERR_OOM);
    std::string leftover;
    dap::split(wire.output, &leftover);
    EXPECT_EQ(leftover, "");
    grdbg_dap_destroy(s);
    return reached;
  });
  EXPECT_GE(failures, 3);
}

GRDBG_TEST_MAIN()
