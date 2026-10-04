/**
 * @file
 *
 * Stepping, resuming, and the polls that cannot pause.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/model/model_internal.h"

namespace {

/// Where the program is, as "line:depth".
std::string place(GRDBG_Debugger * d) {
  Here h = here(d);
  return std::to_string(h.line) + ":" + std::to_string(h.total);
}

/// Sets a breakpoint at `line` of a.toy and returns its id.
uint64_t break_at(GRDBG_Debugger * d, int line) {
  uint64_t id = 0;
  EXPECT_EQ(grdbg_debugger_set_breakpoints(d, "a.toy", &line, 1, &id), GRDBG_OK);
  return id;
}

/// Starts the program stopped at `line` of main.
void stop_at(ToyWorld & w, int line) {
  ASSERT_EQ(w.attach(), GRDBG_OK);
  break_at(w.dbg, line);
  w.run_to_pause();
  ASSERT_EQ(place(w.dbg), std::to_string(line) + ":1");
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", nullptr, 0, nullptr), GRDBG_OK);
}

}  // namespace

// ---------------------------------------------------------------------------
// The three steps
// ---------------------------------------------------------------------------

TEST(Step, InGoesIntoTheCalleesFirstLineAtDepthTwo) {
  ToyWorld w(basic_program());
  stop_at(w, 2);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_IN), GRDBG_OK);
  w.resume_to_pause();
  EXPECT_EQ(place(w.dbg), "10:2");
  EXPECT_EQ(stop_reason(w.dbg), GRDBG_STOP_STEP);
}

TEST(Step, OverStopsOnTheFollowingLineOfTheCallerWithoutStoppingInTheCallee) {
  ToyWorld w(basic_program());
  stop_at(w, 2);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_OVER), GRDBG_OK);
  w.resume_to_pause();
  EXPECT_EQ(place(w.dbg), "3:1");
  EXPECT_EQ(stop_reason(w.dbg), GRDBG_STOP_STEP);
  EXPECT_EQ(w.toy.output, "r=2\n");  // the callee ran, unstopped
}

TEST(Step, OutFromTheCalleeStopsInTheCallerAfterTheCall) {
  ToyWorld w(basic_program());
  stop_at(w, 2);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_IN), GRDBG_OK);
  w.resume_to_pause();
  ASSERT_EQ(place(w.dbg), "10:2");
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_OUT), GRDBG_OK);
  w.resume_to_pause();
  EXPECT_EQ(place(w.dbg), "3:1");
  EXPECT_EQ(w.toy.output, "r=2\n");
}

TEST(Step, InOnALineWithNoCallGoesToTheNextLine) {
  ToyWorld w(basic_program());
  stop_at(w, 3);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_IN), GRDBG_OK);
  w.resume_to_pause();
  EXPECT_EQ(place(w.dbg), "4:1");
}

TEST(Step, OverOnALineWithNoCallGoesToTheNextLine) {
  ToyWorld w(basic_program());
  stop_at(w, 3);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_OVER), GRDBG_OK);
  w.resume_to_pause();
  EXPECT_EQ(place(w.dbg), "4:1");
}

TEST(Step, InsideTheCalleeOverStaysAtItsDepthLineByLine) {
  ToyWorld w(basic_program());
  stop_at(w, 2);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_IN), GRDBG_OK);
  w.resume_to_pause();
  std::vector<std::string> seen{place(w.dbg)};
  for (int i = 0; i < 2; ++i) {
    ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_OVER), GRDBG_OK);
    w.resume_to_pause();
    seen.push_back(place(w.dbg));
  }
  EXPECT_EQ(seen, (std::vector<std::string>{"10:2", "11:2", "12:2"}));
  // Over the last line of the callee returns to the caller: depth is smaller.
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_OVER), GRDBG_OK);
  w.resume_to_pause();
  EXPECT_EQ(place(w.dbg), "3:1");
}

TEST(Step, OutWithNoOuterFrameRunsToTheEnd) {
  ToyWorld w(basic_program());
  stop_at(w, 3);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_OUT), GRDBG_OK);
  w.resume_to_end();
  EXPECT_EQ(w.toy.output, "r=2\nx=3\ny=7\n");
}

TEST(Step, OverARecursiveCallIsNotStoppedByTheInnerFramesOfTheSameLines) {
  toy::Program p;
  p.fn("main", "a.toy").gset(1, 4).call(2, "rec").nop(3);
  p.fn("rec", "a.toy").gadd(10, -1).jg(11, 3).jmp(12, 4).call(13, "rec").nop(14);
  ToyWorld w(std::move(p));
  stop_at(w, 2);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_OVER), GRDBG_OK);
  w.resume_to_pause();
  EXPECT_EQ(place(w.dbg), "3:1");
}

TEST(Step, InIntoARecursiveCallStopsAtTheSameLineOneFrameDeeper) {
  toy::Program p;
  p.fn("main", "a.toy").gset(1, 4).call(2, "rec").nop(3);
  p.fn("rec", "a.toy").gadd(10, -1).jg(11, 3).jmp(12, 4).call(13, "rec").nop(14);
  ToyWorld w(std::move(p));
  stop_at(w, 2);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_IN), GRDBG_OK);
  w.resume_to_pause();
  EXPECT_EQ(place(w.dbg), "10:2");
  for (int expected_depth = 2; expected_depth < 4; ++expected_depth) {
    // 10, 11, 13 and then the call: step in thrice more gets to the next 10.
    for (int k = 0; k < 3; ++k) {
      ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_IN), GRDBG_OK);
      w.resume_to_pause();
    }
    EXPECT_EQ(place(w.dbg), "10:" + std::to_string(expected_depth + 1));
  }
}

TEST(Step, ABreakpointOnTheNextLineWinsAndIsReportedAsOne) {
  ToyWorld w(basic_program());
  stop_at(w, 2);
  uint64_t id = break_at(w.dbg, 3);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_OVER), GRDBG_OK);
  w.resume_to_pause();
  GRDBG_Stop stop;
  ASSERT_EQ(grdbg_debugger_stopped(w.dbg, &stop), GRDBG_OK);
  EXPECT_EQ(stop.reason, GRDBG_STOP_BREAKPOINT);
  ASSERT_EQ(stop.hit_count, 1u);
  EXPECT_EQ(stop.hit_ids[0], id);
  // The step was cancelled by it: continuing runs to the end.
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", nullptr, 0, nullptr), GRDBG_OK);
  ASSERT_EQ(grdbg_debugger_continue(w.dbg), GRDBG_OK);
  w.resume_to_end();
}

TEST(Step, ABreakpointInsideTheSteppedOverCallStopsThere) {
  ToyWorld w(basic_program());
  stop_at(w, 2);
  break_at(w.dbg, 11);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_OVER), GRDBG_OK);
  w.resume_to_pause();
  EXPECT_EQ(place(w.dbg), "11:2");
  EXPECT_EQ(stop_reason(w.dbg), GRDBG_STOP_BREAKPOINT);
}

TEST(Step, ContinueCancelsAStep) {
  ToyWorld w(basic_program());
  stop_at(w, 2);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_IN), GRDBG_OK);
  EXPECT_TRUE(grdbg_debugger_armed(w.dbg));
  ASSERT_EQ(grdbg_debugger_continue(w.dbg), GRDBG_OK);
  EXPECT_FALSE(grdbg_debugger_armed(w.dbg));
  w.resume_to_end();
  EXPECT_EQ(w.toy.output, "r=2\nx=3\ny=7\n");
}

TEST(Step, ABudgetPauseDuringAStepCancelsIt) {
  ToyWorld w(basic_program());
  stop_at(w, 2);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_IN), GRDBG_OK);
  // The step would stop on the callee's first line, but fuel runs out first:
  // the fuel key outranks the YIELD phase, which does not run at all.
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, grcore_context_fuel_used(w.ctx)), GRCORE_OK);
  w.resume_to_pause();
  EXPECT_EQ(grcore_context_pause_key(w.ctx, 0), grcore_core_key(GRCORE_REQUEST_FUEL));
  EXPECT_EQ(place(w.dbg), "10:2");  // paused in the callee, by fuel
  GRDBG_Stop stop;
  ASSERT_EQ(grdbg_debugger_stopped(w.dbg, &stop), GRDBG_OK);
  EXPECT_EQ(stop.reason, GRDBG_STOP_NONE);
  EXPECT_FALSE(grdbg_debugger_armed(w.dbg));  // the step is over
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  w.resume_to_end();  // no later step stop
}

TEST(Step, AStepNeedsAStopToStepFrom) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  EXPECT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_IN), GRDBG_ERR_INVALID);
  break_at(w.dbg, 2);
  w.run_to_pause();
  EXPECT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_NONE), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_step(w.dbg, static_cast<GRDBG_StepKind>(9)), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_step(nullptr, GRDBG_STEP_IN), GRDBG_ERR_INVALID);
  EXPECT_TRUE(grdbg_debugger_armed(w.dbg));  // the breakpoint's, and nothing else
  EXPECT_EQ(w.dbg->step, GRDBG_STEP_NONE);
}

// ---------------------------------------------------------------------------
// Resuming
// ---------------------------------------------------------------------------

TEST(Resume, AProgramIsNotStoppedTwiceAtOnePoll) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  break_at(w.dbg, 2);
  w.run_to_pause();
  int stops = 1;
  uint64_t polls_at_stop = w.toy.polls;
  ASSERT_EQ(grdbg_debugger_continue(w.dbg), GRDBG_OK);
  GRCORE_Outcome outcome;
  ASSERT_EQ(w.resume(&outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(stops, 1);
  // The poll that paused was taken once; the rest followed it.
  EXPECT_GT(w.toy.polls, polls_at_stop);
  EXPECT_EQ(w.toy.output, "r=2\nx=3\ny=7\n");
}

TEST(Resume, AOneStatementLoopStopsOnEveryIteration) {
  toy::Program p;
  p.fn("main", "a.toy", {"n"}).set(1, "n", 3).loop(2, "n", 1).print(3, "n");
  ToyWorld w(std::move(p));
  ASSERT_EQ(w.attach(), GRDBG_OK);
  break_at(w.dbg, 2);
  GRCORE_Outcome outcome;
  ASSERT_EQ(w.run(&outcome), GRCORE_OK);
  int stops = 0;
  std::vector<std::string> n_at_stop;
  while (outcome == GRCORE_OUTCOME_PAUSED) {
    ++stops;
    GRDBG_Variable v;
    EXPECT_EQ(grdbg_debugger_variable(w.dbg, 0, 0, 0, &v), GRDBG_OK);
    n_at_stop.push_back(v.text);
    ASSERT_EQ(grdbg_debugger_continue(w.dbg), GRDBG_OK);
    ASSERT_EQ(w.resume(&outcome), GRCORE_OK);
  }
  EXPECT_EQ(stops, 3);
  EXPECT_EQ(n_at_stop, (std::vector<std::string>{"3", "2", "1"}));
  EXPECT_EQ(w.toy.output, "n=0\n");
}

TEST(Resume, ContinueForgetsTheStopAndKeepsTheBreakpoints) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  break_at(w.dbg, 2);
  w.run_to_pause();
  GRDBG_Stop stop;
  ASSERT_EQ(grdbg_debugger_stopped(w.dbg, &stop), GRDBG_OK);
  EXPECT_EQ(stop.reason, GRDBG_STOP_BREAKPOINT);
  ASSERT_EQ(grdbg_debugger_continue(w.dbg), GRDBG_OK);
  EXPECT_EQ(grdbg_debugger_breakpoint_count(w.dbg), 1u);
  EXPECT_TRUE(grdbg_debugger_armed(w.dbg));
  EXPECT_EQ(grdbg_debugger_continue(nullptr), GRDBG_ERR_INVALID);
}

// ---------------------------------------------------------------------------
// Polls that cannot pause
// ---------------------------------------------------------------------------

namespace {

toy::Program with_native_and_nested() {
  toy::Program p;
  toy::Function & m = p.fn("main", "a.toy", {"x"});
  m.set(1, "x", 1).native(2).nested(3).print(4, "x");
  return p;
}

}  // namespace

TEST(CannotPause, ABreakpointAtANativesRuntimePollNeverStopsAndNothingUnwinds) {
  ToyWorld w(with_native_and_nested());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  break_at(w.dbg, 2);
  GRCORE_Outcome outcome;
  ASSERT_EQ(w.run(&outcome), GRCORE_OK);  // not ERR_LIMIT: the program is unchanged
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(w.toy.output, "x=1\n");
}

TEST(CannotPause, ABreakpointInANestedActivationNeverStopsAndNothingUnwinds) {
  ToyWorld w(with_native_and_nested());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  break_at(w.dbg, 3);
  GRCORE_Outcome outcome;
  ASSERT_EQ(w.run(&outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(w.toy.output, "x=1\n");
}

TEST(CannotPause, AStepAcrossThemSkipsThemAndStopsAtThePollsThatCan) {
  ToyWorld w(with_native_and_nested());
  stop_at(w, 1);
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_IN), GRDBG_OK);
  w.resume_to_pause();
  EXPECT_EQ(place(w.dbg), "4:1");  // lines 2 and 3 polled and could not pause
}

namespace {

/// What the debugger must not be: a YIELD handler that votes to pause at every
/// poll without asking whether the poll may pause.
struct Reckless {
  GRCORE_Port * port = nullptr;
  GRCORE_RequestKind kind = 0;
};

void reckless_handler(GRCORE_Context *, void *, GRCORE_PollCall * call) {
  grcore_pollcall_vote(call, GRCORE_VERDICT_PAUSE);
}

const GRCORE_Key kRecklessKey = {"reckless", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_YIELD, nullptr, reckless_handler, nullptr, nullptr, nullptr};

}  // namespace

TEST(CannotPause, AHandlerThatVotesWithoutAskingEndsTheProgramItMeantToLookAt) {
  // The control for the tests above: core turns a pause voted at a native's
  // poll into a limit unwind, so the program does not finish. This is the
  // mistake the debugger's handler avoids by asking grcore_pollcall_pause_allowed.
  ToyWorld w(with_native_and_nested());
  Reckless r;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kRecklessKey, &r.kind), GRCORE_OK);
  ASSERT_EQ(grcore_context_port(w.ctx, &r.port), GRCORE_OK);
  ASSERT_EQ(grcore_context_register(w.ctx, &kRecklessKey, &r), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(r.port, r.kind), GRCORE_OK);
  GRCORE_Outcome outcome;
  // Every poll pauses; resume until the native's poll, where the pause is refused.
  GRCORE_Result result = w.run(&outcome);
  while (result == GRCORE_OK && outcome == GRCORE_OUTCOME_PAUSED) {
    result = w.resume(&outcome);
  }
  EXPECT_EQ(result, GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_context_pause_key(w.ctx, 0), &kRecklessKey);
  grcore_port_release(r.port);
}

TEST(CannotPause, TheSameProgramWithoutTheseBreakpointsStopsAtOrdinaryLines) {
  ToyWorld w(with_native_and_nested());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  break_at(w.dbg, 4);
  w.run_to_pause();
  EXPECT_EQ(place(w.dbg), "4:1");
}

GRDBG_TEST_MAIN()
