/**
 * @file
 *
 * The debug model: attaching, breakpoints, stops, the snapshot of a stop and
 * what is refused when.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include "../../src/model/model_internal.h"

#include <cstring>
#include <thread>

namespace {

std::vector<std::string> scope_names(GRDBG_Debugger * d, size_t frame) {
  std::vector<std::string> names;
  size_t count = 0;
  EXPECT_EQ(grdbg_debugger_scope_count(d, frame, &count), GRDBG_OK);
  for (size_t i = 0; i < count; ++i) {
    GRDBG_Scope scope;
    EXPECT_EQ(grdbg_debugger_scope(d, frame, i, &scope), GRDBG_OK);
    names.push_back(scope.name);
  }
  return names;
}

}  // namespace

// ---------------------------------------------------------------------------
// Attaching
// ---------------------------------------------------------------------------

TEST(Attach, ADebuggerIsFoundByItsContextAndKnowsIt) {
  ToyWorld w(basic_program());
  EXPECT_EQ(grdbg_debugger_get(w.ctx), nullptr);
  ASSERT_EQ(w.attach(), GRDBG_OK);
  EXPECT_EQ(grdbg_debugger_get(w.ctx), w.dbg);
  EXPECT_EQ(grdbg_debugger_context(w.dbg), w.ctx);
  EXPECT_EQ(grdbg_debugger_context(nullptr), nullptr);
  EXPECT_EQ(grdbg_debugger_get(nullptr), nullptr);
}

TEST(Attach, TheKeyIsCardinalityOneInTheYieldPhase) {
  const GRCORE_Key * key = grdbg_debugger_key();
  ASSERT_NE(key, nullptr);
  EXPECT_EQ(key->cardinality, GRCORE_CARDINALITY_ONE);
  EXPECT_EQ(key->phase, GRCORE_PHASE_YIELD);
  EXPECT_NE(key->destroy, nullptr);
  EXPECT_NE(key->poll, nullptr);
}

TEST(Attach, ASecondDebuggerIsRefusedAndTheFirstStillWorks) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  GRDBG_Debugger * first = w.dbg;
  GRDBG_Debugger * second = nullptr;
  EXPECT_EQ(grdbg_debugger_attach(w.ctx, nullptr, &second), GRDBG_ERR_INVALID);
  EXPECT_EQ(second, nullptr);
  EXPECT_EQ(grdbg_debugger_get(w.ctx), first);
  uint64_t id;
  int line = 2;
  EXPECT_EQ(grdbg_debugger_set_breakpoints(first, "a.toy", &line, 1, &id), GRDBG_OK);
  w.run_to_pause();
  EXPECT_EQ(stop_reason(first), GRDBG_STOP_BREAKPOINT);
}

TEST(Attach, TheQueriesOfANonOwnerAnswerNullFalseOrZero) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 2;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
  GRDBG_Debugger * found = reinterpret_cast<GRDBG_Debugger *>(0x1);
  bool armed = true;
  size_t count = 9;
  uint64_t generation = 9;
  std::thread([&] {
    found = grdbg_debugger_get(w.ctx);
    armed = grdbg_debugger_armed(w.dbg);
    count = grdbg_debugger_breakpoint_count(w.dbg);
    generation = grdbg_debugger_generation(w.dbg);
  }).join();
  EXPECT_EQ(found, nullptr);
  EXPECT_FALSE(armed);
  EXPECT_EQ(count, 0u);
  EXPECT_EQ(generation, 0u);
  // And the owner still sees it all.
  EXPECT_EQ(grdbg_debugger_get(w.ctx), w.dbg);
  EXPECT_TRUE(grdbg_debugger_armed(w.dbg));
}

TEST(Attach, ArgumentsAreCheckedAndNothingIsWrittenOnFailure) {
  ToyWorld w(basic_program());
  GRDBG_Debugger * d = reinterpret_cast<GRDBG_Debugger *>(0x1);
  EXPECT_EQ(grdbg_debugger_attach(nullptr, nullptr, &d), GRDBG_ERR_INVALID);
  EXPECT_EQ(d, reinterpret_cast<GRDBG_Debugger *>(0x1));
  EXPECT_EQ(grdbg_debugger_attach(w.ctx, nullptr, nullptr), GRDBG_ERR_INVALID);
}

TEST(Attach, ANonOwnerCannotAttach) {
  ToyWorld w(basic_program());
  GRDBG_Result r = GRDBG_OK;
  std::thread([&] {
    GRDBG_Debugger * d = nullptr;
    r = grdbg_debugger_attach(w.ctx, nullptr, &d);
  }).join();
  EXPECT_EQ(r, GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_get(w.ctx), nullptr);
}

TEST(Attach, ARunningContextCannotBeAttached) {
  ToyWorld w(basic_program());
  toy::Program p;
  p.fn("main", "a.toy").nop(1);
  GRDBG_Result r = GRDBG_OK;
  GRDBG_Debugger * d = nullptr;
  struct State { GRDBG_Result * r; GRDBG_Debugger ** d; } state{&r, &d};
  GRCORE_Outcome outcome;
  auto fn = [](GRCORE_Context * c, void * s) {
    auto * st = static_cast<State *>(s);
    *st->r = grdbg_debugger_attach(c, nullptr, st->d);
    return GRCORE_STEP_FINISHED;
  };
  ASSERT_EQ(grcore_run(w.ctx, fn, &state, &outcome), GRCORE_OK);
  EXPECT_EQ(r, GRDBG_ERR_INVALID);
}

TEST(Attach, TheDebuggerDiesWithItsContextAndFreesEverythingItAllocated) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int lines[] = {2, 3, 4};
  uint64_t ids[3];
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", lines, 3, ids), GRDBG_OK);
  w.run_to_pause();
  size_t total, available;
  ASSERT_EQ(grdbg_debugger_frames(w.dbg, &total, &available), GRDBG_OK);
  EXPECT_GT(w.debug_allocator.live, 0);
  // The world's destructor requires zero live blocks once the context is gone.
}

// ---------------------------------------------------------------------------
// Breakpoints
// ---------------------------------------------------------------------------

TEST(Breakpoints, SettingReturnsIdsFromOneThatAreNeverReused) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int first[] = {2, 3};
  uint64_t ids[2];
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", first, 2, ids), GRDBG_OK);
  EXPECT_EQ(ids[0], 1u);
  EXPECT_EQ(ids[1], 2u);
  // A replacement assigns fresh ids; the old ones are gone for good.
  int second[] = {4};
  uint64_t again[1];
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", second, 1, again), GRDBG_OK);
  EXPECT_EQ(again[0], 3u);
  EXPECT_EQ(grdbg_debugger_breakpoint_count(w.dbg), 1u);
  // Clearing and setting again keeps counting.
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", nullptr, 0, nullptr), GRDBG_OK);
  EXPECT_EQ(grdbg_debugger_breakpoint_count(w.dbg), 0u);
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", second, 1, again), GRDBG_OK);
  EXPECT_EQ(again[0], 4u);
}

TEST(Breakpoints, ASetReplacesOnlyItsOwnSource) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int a[] = {1, 2}, b[] = {5};
  uint64_t ids[2];
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", a, 2, ids), GRDBG_OK);
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "b.toy", b, 1, ids), GRDBG_OK);
  EXPECT_EQ(grdbg_debugger_breakpoint_count(w.dbg), 3u);
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", nullptr, 0, nullptr), GRDBG_OK);
  EXPECT_EQ(grdbg_debugger_breakpoint_count(w.dbg), 1u);
}

TEST(Breakpoints, DuplicateLinesInOneCallEachGetAnIdAndAreAllReportedWhenHit) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int lines[] = {2, 2};
  uint64_t ids[2];
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", lines, 2, ids), GRDBG_OK);
  EXPECT_NE(ids[0], ids[1]);
  w.run_to_pause();
  GRDBG_Stop stop;
  ASSERT_EQ(grdbg_debugger_stopped(w.dbg, &stop), GRDBG_OK);
  EXPECT_EQ(stop.reason, GRDBG_STOP_BREAKPOINT);
  ASSERT_EQ(stop.hit_count, 2u);
  EXPECT_EQ(stop.hit_ids[0], ids[0]);
  EXPECT_EQ(stop.hit_ids[1], ids[1]);
}

TEST(Breakpoints, MoreThanTheLimitIsRefusedAndNothingChanges) {
  ToyWorld w(basic_program());
  GRDBG_Limits limits;
  grdbg_limits_default(&limits);
  limits.max_breakpoints = 3;
  ASSERT_EQ(w.attach(&limits), GRDBG_OK);
  int two[] = {1, 2}, four[] = {1, 2, 3, 4}, one[] = {7};
  uint64_t ids[4];
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", two, 2, ids), GRDBG_OK);
  uint64_t untouched[4] = {99, 99, 99, 99};
  EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", four, 4, untouched), GRDBG_ERR_LIMIT);
  EXPECT_EQ(untouched[0], 99u);  // out parameters are written only on success
  EXPECT_EQ(grdbg_debugger_breakpoint_count(w.dbg), 2u);
  // Together with another source's, the sum is what is capped.
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "b.toy", one, 1, ids), GRDBG_OK);
  EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "b.toy", two, 2, ids), GRDBG_ERR_LIMIT);
  EXPECT_EQ(grdbg_debugger_breakpoint_count(w.dbg), 3u);
  // The id counter did not move for the refused calls.
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "b.toy", one, 1, ids), GRDBG_OK);
  EXPECT_EQ(ids[0], 4u);
}

TEST(Breakpoints, ArgumentsAreChecked) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int good[] = {1}, zero[] = {0}, negative[] = {-4};
  uint64_t id;
  EXPECT_EQ(grdbg_debugger_set_breakpoints(nullptr, "a.toy", good, 1, &id), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, nullptr, good, 1, &id), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", nullptr, 1, &id), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", good, 1, nullptr), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", zero, 1, &id), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", negative, 1, &id), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_breakpoint_count(w.dbg), 0u);
  EXPECT_EQ(grdbg_debugger_breakpoint_count(nullptr), 0u);
}

TEST(Breakpoints, ANonOwnerIsRefusedAndNothingChanges) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 2;
  GRDBG_Result r = GRDBG_OK;
  uint64_t id = 0;
  std::thread([&] {
    r = grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id);
  }).join();
  EXPECT_EQ(r, GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_breakpoint_count(w.dbg), 0u);
  EXPECT_FALSE(grdbg_debugger_armed(w.dbg));
}

TEST(Breakpoints, ABreakpointStopsBeforeItsLineRunsAndTheStackShowsIt) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 3;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
  w.run_to_pause();
  Here h = here(w.dbg);
  EXPECT_EQ(h.file, "a.toy");
  EXPECT_EQ(h.line, 3);
  EXPECT_EQ(h.total, 1u);
  EXPECT_EQ(w.toy.output, "r=2\n");  // the call has run; PRINT x has not
  GRDBG_Stop stop;
  ASSERT_EQ(grdbg_debugger_stopped(w.dbg, &stop), GRDBG_OK);
  EXPECT_EQ(stop.reason, GRDBG_STOP_BREAKPOINT);
  ASSERT_EQ(stop.hit_count, 1u);
  EXPECT_EQ(stop.hit_ids[0], id);
  EXPECT_EQ(stop.other_key, nullptr);
  // The pause names the debugger's key.
  ASSERT_EQ(grcore_context_pause_key_count(w.ctx), 1u);
  EXPECT_EQ(grcore_context_pause_key(w.ctx, 0), grdbg_debugger_key());
}

TEST(Breakpoints, ASourceIsComparedByteForByte) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 2;
  uint64_t id;
  for (const char * source : {"A.toy", "a.toy ", "./a.toy", "a.to", "a.toyy"}) {
    ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, source, &line, 1, &id), GRDBG_OK);
  }
  GRCORE_Outcome outcome;
  ASSERT_EQ(w.run(&outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);  // none matched
}

TEST(Breakpoints, ALineThatIsNeverReachedNeverStops) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 99;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
  GRCORE_Outcome outcome;
  ASSERT_EQ(w.run(&outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(w.toy.output, "r=2\nx=3\ny=7\n");
}

// ---------------------------------------------------------------------------
// Arming
// ---------------------------------------------------------------------------

TEST(Arming, AnAttachedDebuggerWithNothingToDoLeavesTheFastPathAlone) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  EXPECT_FALSE(grdbg_debugger_armed(w.dbg));
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, w.dbg->kind));
  // The kind exists, and belongs to the debugger's key.
  EXPECT_EQ(grcore_context_request_kind_key(w.ctx, w.dbg->kind), grdbg_debugger_key());
  // A handler of another service never runs: no poll left the fast path.
  int slow_polls = 0;
  static const GRCORE_Key probe_key = GRCORE_KEY_INIT("probe", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_OBSERVE, nullptr,
      [](GRCORE_Context *, void * v, GRCORE_PollCall *) { ++*static_cast<int *>(v); }, nullptr, nullptr, nullptr);
  ASSERT_EQ(grcore_context_register(w.ctx, &probe_key, &slow_polls), GRCORE_OK);
  GRCORE_Outcome outcome;
  ASSERT_EQ(w.run(&outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_GT(w.toy.polls, 5u);
  EXPECT_EQ(slow_polls, 0);
}

TEST(Arming, ItArmsForABreakpointAndDisarmsWhenTheLastIsCleared) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 2;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
  EXPECT_TRUE(grdbg_debugger_armed(w.dbg));
  EXPECT_TRUE(grcore_context_request_pending(w.ctx, w.dbg->kind));
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", nullptr, 0, nullptr), GRDBG_OK);
  EXPECT_FALSE(grdbg_debugger_armed(w.dbg));
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, w.dbg->kind));
}

TEST(Arming, DisarmClearsBreakpointsAndStepsAndTheProgramThenRunsFree) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int lines[] = {2, 4};
  uint64_t ids[2];
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", lines, 2, ids), GRDBG_OK);
  w.run_to_pause();
  ASSERT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_IN), GRDBG_OK);
  ASSERT_EQ(grdbg_debugger_disarm(w.dbg), GRDBG_OK);
  EXPECT_EQ(grdbg_debugger_breakpoint_count(w.dbg), 0u);
  EXPECT_FALSE(grdbg_debugger_armed(w.dbg));
  w.resume_to_end();
  EXPECT_EQ(w.toy.output, "r=2\nx=3\ny=7\n");
  // Still attached; a breakpoint set again arms it again.
  EXPECT_EQ(grdbg_debugger_get(w.ctx), w.dbg);
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", lines, 1, ids), GRDBG_OK);
  EXPECT_TRUE(grdbg_debugger_armed(w.dbg));
}

TEST(Arming, ARequestedPauseStopsAtTheNextPollThatMayPauseAndIsSpent) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  ASSERT_EQ(grdbg_debugger_request_pause(w.dbg), GRDBG_OK);
  EXPECT_TRUE(grdbg_debugger_armed(w.dbg));
  w.run_to_pause();
  EXPECT_EQ(here(w.dbg).line, 1);
  EXPECT_EQ(stop_reason(w.dbg), GRDBG_STOP_PAUSE);
  ASSERT_EQ(grdbg_debugger_continue(w.dbg), GRDBG_OK);
  EXPECT_FALSE(grdbg_debugger_armed(w.dbg));  // it was a one-shot
  w.resume_to_end();
}

// ---------------------------------------------------------------------------
// What may be read, and when
// ---------------------------------------------------------------------------

TEST(Reads, AreRefusedUnlessTheContextIsPausedOrAtPollAndHeldByTheCaller) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  size_t total = 77, available = 77, n = 77;
  GRDBG_Frame frame;
  GRDBG_Scope scope;
  GRDBG_Variable variable;
  GRDBG_Stop stop;
  bool found;
  // Before the first run the context is parked.
  EXPECT_EQ(grdbg_debugger_frames(w.dbg, &total, &available), GRDBG_ERR_INVALID);
  EXPECT_EQ(total, 77u);  // untouched
  EXPECT_EQ(grdbg_debugger_frame(w.dbg, 0, &frame), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_scope_count(w.dbg, 0, &n), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_scope(w.dbg, 0, 0, &scope), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_variable(w.dbg, 0, 0, 0, &variable), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_find_variable(w.dbg, 0, "x", &found, &variable), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_stopped(w.dbg, &stop), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_step(w.dbg, GRDBG_STEP_IN), GRDBG_ERR_INVALID);
  EXPECT_EQ(n, 77u);

  int line = 2;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
  w.run_to_pause();
  EXPECT_EQ(grdbg_debugger_frames(w.dbg, &total, &available), GRDBG_OK);

  // Another thread holds nothing, so it reads nothing.
  GRDBG_Result r = GRDBG_OK;
  std::thread([&] { r = grdbg_debugger_frames(w.dbg, &total, &available); }).join();
  EXPECT_EQ(r, GRDBG_ERR_INVALID);

  w.resume_to_end();
}

TEST(Reads, AreRefusedWhileTheGuestIsRunning) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  struct State { GRDBG_Debugger * d; GRDBG_Result frames; GRDBG_Result step; GRDBG_Result stopped; } state{w.dbg, GRDBG_OK, GRDBG_OK, GRDBG_OK};
  auto fn = [](GRCORE_Context *, void * s) {
    auto * st = static_cast<State *>(s);
    size_t t, a;
    GRDBG_Stop stop;
    st->frames = grdbg_debugger_frames(st->d, &t, &a);
    st->step = grdbg_debugger_step(st->d, GRDBG_STEP_IN);
    st->stopped = grdbg_debugger_stopped(st->d, &stop);
    return GRCORE_STEP_FINISHED;
  };
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn, &state, &outcome), GRCORE_OK);
  EXPECT_EQ(state.frames, GRDBG_ERR_INVALID);
  EXPECT_EQ(state.step, GRDBG_ERR_INVALID);
  EXPECT_EQ(state.stopped, GRDBG_ERR_INVALID);
  EXPECT_FALSE(grdbg_debugger_armed(w.dbg));  // the refused step armed nothing
}

TEST(Reads, ANonOwnerThatTheContextWasReleasedToReadsAfterAcquiringIt) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 2;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
  w.run_to_pause();
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  size_t total = 0, available = 0;
  GRDBG_Result before = GRDBG_OK, after = GRDBG_ERR_INVALID;
  std::thread([&] {
    before = grdbg_debugger_frames(w.dbg, &total, &available);  // not held yet
    EXPECT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
    after = grdbg_debugger_frames(w.dbg, &total, &available);
    EXPECT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  }).join();
  EXPECT_EQ(before, GRDBG_ERR_INVALID);
  EXPECT_EQ(after, GRDBG_OK);
  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
}

// ---------------------------------------------------------------------------
// The snapshot of a stop
// ---------------------------------------------------------------------------

namespace {

/// main calls f, which calls g: stopped in g, three frames deep.
toy::Program three_deep() {
  toy::Program p;
  p.fn("main", "a.toy", {"x"}).set(1, "x", 5).call(2, "f").print(3, "x");
  p.fn("f", "a.toy", {"a", "b"}).set(10, "a", 7).call(11, "g").print(12, "a");
  p.fn("g", "a.toy", {"n"}).set(20, "n", 9).add(21, "n", 1).print(22, "n");
  return p;
}

}  // namespace

TEST(Snapshot, FramesAreInnermostFirstWithTheirNamesFilesLinesAndEngine) {
  ToyWorld w(three_deep());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 21;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
  w.run_to_pause();
  size_t total = 0, available = 0;
  ASSERT_EQ(grdbg_debugger_frames(w.dbg, &total, &available), GRDBG_OK);
  EXPECT_EQ(total, 3u);
  EXPECT_EQ(available, 3u);
  const int expected_line[] = {21, 11, 2};
  for (size_t i = 0; i < 3; ++i) {
    GRDBG_Frame frame;
    ASSERT_EQ(grdbg_debugger_frame(w.dbg, i, &frame), GRDBG_OK);
    EXPECT_EQ(frame.depth, i);
    EXPECT_EQ(frame.name, "frame " + std::to_string(i + 1));
    EXPECT_STREQ(frame.file, "a.toy");
    EXPECT_EQ(frame.line, expected_line[i]);
    EXPECT_STREQ(frame.engine, "toy");
  }
  GRDBG_Frame frame;
  EXPECT_EQ(grdbg_debugger_frame(w.dbg, 3, &frame), GRDBG_ERR_INVALID);
}

TEST(Snapshot, ScopesAndVariablesAreReadThroughTheDescriptorPerFrame) {
  ToyWorld w(three_deep());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 21;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
  w.run_to_pause();
  EXPECT_EQ(scope_names(w.dbg, 0), (std::vector<std::string>{"Locals", "Globals"}));

  GRDBG_Scope scope;
  ASSERT_EQ(grdbg_debugger_scope(w.dbg, 0, 0, &scope), GRDBG_OK);
  EXPECT_EQ(scope.kind, GRDBG_SCOPE_LOCAL);
  EXPECT_EQ(scope.variable_count, 1u);
  ASSERT_EQ(grdbg_debugger_scope(w.dbg, 0, 1, &scope), GRDBG_OK);
  EXPECT_EQ(scope.kind, GRDBG_SCOPE_GLOBAL);

  GRDBG_Variable v;
  ASSERT_EQ(grdbg_debugger_variable(w.dbg, 0, 0, 0, &v), GRDBG_OK);
  EXPECT_STREQ(v.name, "n");
  EXPECT_STREQ(v.text, "9");
  EXPECT_EQ(v.kind, GRDBG_VALUE_ENGINE);
  // Frame 2 (index 1) shows its own variables.
  ASSERT_EQ(grdbg_debugger_scope(w.dbg, 1, 0, &scope), GRDBG_OK);
  EXPECT_EQ(scope.variable_count, 2u);
  ASSERT_EQ(grdbg_debugger_variable(w.dbg, 1, 0, 0, &v), GRDBG_OK);
  EXPECT_STREQ(v.name, "a");
  EXPECT_STREQ(v.text, "7");
  ASSERT_EQ(grdbg_debugger_variable(w.dbg, 1, 0, 1, &v), GRDBG_OK);
  EXPECT_STREQ(v.name, "b");
  EXPECT_STREQ(v.text, "0");
  ASSERT_EQ(grdbg_debugger_variable(w.dbg, 2, 0, 0, &v), GRDBG_OK);
  EXPECT_STREQ(v.name, "x");
  EXPECT_STREQ(v.text, "5");
  // Out of range anywhere is a refusal, not a read.
  EXPECT_EQ(grdbg_debugger_variable(w.dbg, 0, 0, 1, &v), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_variable(w.dbg, 0, 2, 0, &v), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_variable(w.dbg, 3, 0, 0, &v), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_debugger_scope(w.dbg, 0, 2, &scope), GRDBG_ERR_INVALID);
  size_t n = 5;
  EXPECT_EQ(grdbg_debugger_scope_count(w.dbg, 9, &n), GRDBG_ERR_INVALID);
  EXPECT_EQ(n, 5u);
}

TEST(Snapshot, AValueLongerThanTheDisplayCapIsCutOutsideAUtf8SequenceAndMarked) {
  toy::Program p;
  p.fn("main", "a.toy", {"big", "small"}).set(1, "big", static_cast<int64_t>(toy::kLongValue)).set(2, "small", 7).nop(3);
  ToyWorld w(std::move(p));
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 3;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
  w.run_to_pause();
  GRDBG_Variable v;
  ASSERT_EQ(grdbg_debugger_variable(w.dbg, 0, 0, 0, &v), GRDBG_OK);
  std::string text = v.text;
  // 9,000 bytes of three-byte characters, cut at 8,192: 8,190 is the last whole
  // character, and an ellipsis (three bytes) says it was cut.
  EXPECT_EQ(text.size(), 8190u + 3u);
  EXPECT_EQ(text.substr(8190), "\xe2\x80\xa6");
  EXPECT_EQ(text.substr(0, 3), "\xe2\x82\xac");
  ASSERT_EQ(grdbg_debugger_variable(w.dbg, 0, 0, 1, &v), GRDBG_OK);
  EXPECT_STREQ(v.text, "7");  // and a short one is untouched
}

TEST(Snapshot, AVariableCanBeFoundByNameInnermostScopeFirst) {
  ToyWorld w(three_deep());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 21;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
  w.run_to_pause();
  bool found = false;
  GRDBG_Variable v;
  ASSERT_EQ(grdbg_debugger_find_variable(w.dbg, 0, "n", &found, &v), GRDBG_OK);
  EXPECT_TRUE(found);
  EXPECT_STREQ(v.text, "9");
  ASSERT_EQ(grdbg_debugger_find_variable(w.dbg, 0, "g", &found, &v), GRDBG_OK);
  EXPECT_TRUE(found);  // in the Globals scope
  ASSERT_EQ(grdbg_debugger_find_variable(w.dbg, 0, "a", &found, &v), GRDBG_OK);
  EXPECT_FALSE(found);  // f's variable, not g's
  ASSERT_EQ(grdbg_debugger_find_variable(w.dbg, 1, "a", &found, nullptr), GRDBG_OK);
  EXPECT_TRUE(found);
  EXPECT_EQ(grdbg_debugger_find_variable(w.dbg, 0, nullptr, &found, &v), GRDBG_ERR_INVALID);
}

TEST(Snapshot, ATenThousandFrameStackRecordsTheCapAndCountsTheRest) {
  toy::Program p;
  p.fn("main", "a.toy").gset(1, 10000).call(2, "deep");
  p.fn("deep", "d.toy").gadd(5, -1).jg(6, 3).jmp(7, 4).call(8, "deep").nop(9);
  ToyWorld w(std::move(p));
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 9;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "d.toy", &line, 1, &id), GRDBG_OK);
  w.run_to_pause();
  size_t total = 0, available = 0;
  ASSERT_EQ(grdbg_debugger_frames(w.dbg, &total, &available), GRDBG_OK);
  EXPECT_EQ(total, 10001u);  // main and 10,000 calls of deep
  EXPECT_EQ(available, 1000u);
  GRDBG_Frame frame;
  EXPECT_EQ(grdbg_debugger_frame(w.dbg, 999, &frame), GRDBG_OK);
  EXPECT_EQ(grdbg_debugger_frame(w.dbg, 1000, &frame), GRDBG_ERR_INVALID);
}

TEST(Snapshot, IsDroppedByAContinueAndTheGenerationSaysSo) {
  ToyWorld w(three_deep());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int lines[] = {21, 22};
  uint64_t ids[2];
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", lines, 2, ids), GRDBG_OK);
  w.run_to_pause();
  uint64_t g1 = grdbg_debugger_generation(w.dbg);
  GRDBG_Frame frame;
  ASSERT_EQ(grdbg_debugger_frame(w.dbg, 0, &frame), GRDBG_OK);
  EXPECT_EQ(grdbg_debugger_generation(w.dbg), g1);  // reads change nothing
  ASSERT_EQ(grdbg_debugger_continue(w.dbg), GRDBG_OK);
  EXPECT_NE(grdbg_debugger_generation(w.dbg), g1);
  w.resume_to_pause();
  EXPECT_EQ(here(w.dbg).line, 22);  // a fresh stop, a fresh snapshot
  EXPECT_EQ(grdbg_debugger_generation(nullptr), 0u);
}

TEST(Snapshot, AStaleSnapshotIsRefusedNotMisread) {
  ToyWorld w(three_deep());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  int line = 21;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
  w.run_to_pause();
  GRDBG_Frame frame;
  ASSERT_EQ(grdbg_debugger_frame(w.dbg, 2, &frame), GRDBG_OK);  // records the stack
  // The host resumes without telling the debugger, and the program pauses
  // again on its fuel budget, one statement on. The debugger's handler did not
  // run for that pause (YIELD runs only when nothing else paused), so nothing
  // told it that its snapshot is old.
  ASSERT_EQ(grcore_context_set_fuel(w.ctx, grcore_context_fuel_used(w.ctx)), GRCORE_OK);
  w.resume_to_pause();
  EXPECT_EQ(grcore_context_pause_key(w.ctx, 0), grcore_core_key(GRCORE_REQUEST_FUEL));
  // The innermost frame moved: the old record is refused, not misread.
  EXPECT_EQ(grdbg_debugger_frame(w.dbg, 0, &frame), GRDBG_ERR_INVALID);
  // Beginning the stop, as the host must at every pause, takes a new snapshot.
  GRDBG_Stop stop;
  ASSERT_EQ(grdbg_debugger_stopped(w.dbg, &stop), GRDBG_OK);
  EXPECT_EQ(stop.reason, GRDBG_STOP_NONE);
  ASSERT_NE(stop.other_key, nullptr);
  EXPECT_STREQ(stop.other_key, "fuel");
  ASSERT_EQ(grdbg_debugger_frame(w.dbg, 0, &frame), GRDBG_OK);
  EXPECT_EQ(frame.line, 22);
}

GRDBG_TEST_MAIN()
