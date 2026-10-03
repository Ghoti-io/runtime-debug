/**
 * @file
 *
 * A paused, debugged context moves between threads (CAP-3, AD-6, AD-20) and
 * the debugger goes with it. Run under ThreadSanitizer (`make test-tsan`): the
 * handoffs through release and acquire are what orders the debugger's plain
 * data between threads.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "dap_helpers.h"

#include <thread>

namespace {

toy::Program program() {
  toy::Program p;
  p.fn("main", "a.toy", {"x"}).set(1, "x", 3).call(2, "f").print(3, "x");
  p.fn("f", "a.toy", {"r"}).set(10, "r", 1).add(11, "r", 1).print(12, "r");
  return p;
}

}  // namespace

TEST(Threads, ADebuggedContextPausedOnOneThreadIsReadSteppedAndFinishedOnOthers) {
  ToyWorld w(program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int lines[] = {2, 11};
  uint64_t ids[2];
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", lines, 2, ids), GRDBG_OK);
  w.run_to_pause();
  EXPECT_EQ(here(w.dbg).line, 2);
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);

  // Thread B takes the stop: reads it, continues and pauses at the next.
  std::thread([&] {
    ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
    EXPECT_EQ(here(w.dbg).line, 2);
    GRDBG_Variable v;
    EXPECT_EQ(grdbg_debugger_variable(w.dbg, 0, 0, 0, &v), GRDBG_OK);
    EXPECT_STREQ(v.text, "3");
    EXPECT_EQ(grdbg_debugger_continue(w.dbg), GRDBG_OK);
    w.resume_to_pause();
    EXPECT_EQ(here(w.dbg).line, 11);
    EXPECT_EQ(here(w.dbg).total, 2u);
    EXPECT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_OUT), GRDBG_OK);
    EXPECT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  }).join();

  // Thread C finishes the step and the program.
  std::thread([&] {
    ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
    w.resume_to_pause();
    EXPECT_EQ(here(w.dbg).line, 3);
    EXPECT_EQ(stop_reason(w.dbg), GRDBG_STOP_STEP);
    EXPECT_EQ(grdbg_debugger_continue(w.dbg), GRDBG_OK);
    w.resume_to_end();
    EXPECT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  }).join();

  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  EXPECT_EQ(w.toy.output, "r=2\nx=3\n");
}

TEST(Threads, ASessionServedOnAnotherThreadAfterAHandoffAnswersFromThere) {
  ToyWorld w(program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  dap::Script script;
  script.request("stackTrace").request("scopes", R"({"frameId":1})").request("variables", R"({"variablesReference":1})")
      .request("continue");
  GRDBG_Transport * memory;
  ASSERT_EQ(grdbg_transport_create_memory(script.bytes().data(), script.bytes().size(), nullptr, &memory), GRDBG_OK);
  GRDBG_Dap * session;
  ASSERT_EQ(grdbg_dap_create(w.dbg, memory, nullptr, &session), GRDBG_OK);
  int line = 11;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
  w.run_to_pause();
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  std::thread([&] {
    ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
    EXPECT_EQ(grdbg_dap_notify_stopped(session), GRDBG_OK);
    GRDBG_ServeResult r;
    EXPECT_EQ(grdbg_dap_serve(session, &r), GRDBG_OK);
    EXPECT_EQ(r, GRDBG_SERVE_RESUME);
    w.resume_to_end();
    EXPECT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  }).join();
  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  const uint8_t * out;
  size_t length;
  ASSERT_EQ(grdbg_transport_memory_output(memory, &out, &length), GRDBG_OK);
  std::vector<dap::Msg> all = dap::messages(std::string(reinterpret_cast<const char *>(out), length));
  ASSERT_GE(all.size(), 5u);
  EXPECT_EQ(all[1].num("body.stackFrames.0.line"), 11);
  EXPECT_EQ(all[3].str("body.variables.0.value"), "1");
  grdbg_dap_destroy(session);
  grdbg_transport_destroy(memory);
}

GRDBG_TEST_MAIN()
