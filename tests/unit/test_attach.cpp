/**
 * @file
 *
 * Attaching a debugger changes nothing about the program (AD-15, AD-20).
 *
 * One program is run unattended, with a debugger attached and unarmed, with
 * breakpoints that are never reached, and with a debugger that stops at every
 * line and is continued (reading the whole stack at each stop). All four must
 * agree on output, outcome, fuel, the memory the context was charged, the
 * number of polls the engine made and the poll-by-poll abstract-frame trace;
 * a difference is reported with the first differing poll. The comparison is
 * then shown to fail, by two planted "debuggers" that do what a debugger must
 * not: charge a unit of fuel, and write a slot.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <sstream>

namespace {

/// A program with calls, a loop, a native and a nested activation.
toy::Program program() {
  toy::Program p;
  p.fn("main", "a.toy", {"x", "n"})
      .set(1, "x", 1)
      .set(2, "n", 3)
      .call(3, "f")
      .add(4, "x", 1)
      .loop(5, "n", 2)
      .print(6, "x");
  p.fn("f", "a.toy", {"r"})
      .set(10, "r", 1)
      .add(11, "r", 2)
      .print(12, "r")
      .native(13)
      .nested(14)
      .call(15, "g");
  p.fn("g", "b.toy", {"q"}).set(20, "q", 5).print(21, "q");
  return p;
}

enum class Mode {
  Plain,
  Unarmed,
  NeverReached,
  ContinueAtEveryStop,
  PlantedFuel,
  PlantedSlot
};

struct Outcome {
  std::string output;
  GRCORE_Result result = GRCORE_OK;
  uint64_t fuel_used = 0;
  uint64_t memory_in_use = 0;
  uint64_t memory_peak = 0;
  uint64_t polls = 0;
  observer::Trace trace;
  int stops = 0;
  bool armed_ever = false;
};

/// The things a debugger must not be: each runs at every poll, as a debugger
/// armed with a breakpoint does.
struct Planted {
  GRCORE_Port * port = nullptr;
  GRCORE_RequestKind kind = 0;
  bool charge = false;
  bool write_slot = false;
};

void planted_handler(GRCORE_Context * context, void * value, GRCORE_PollCall *) {
  auto * p = static_cast<Planted *>(value);
  if (p->charge) {
    grcore_context_charge_fuel(context, 1);
  }
  if (p->write_slot) {
    GRCORE_Stack * stack = grcore_context_stack(context);
    GRCORE_FrameRef top = grcore_stack_top(stack);
    if (top.offset != 0) {
      grcore_stack_slot_set(stack, top, toy::kHeader, 4242);
    }
  }
}

const GRCORE_Key kPlantedKey = {"planted debugger", GRCORE_CARDINALITY_ONE,
    GRCORE_PHASE_YIELD, nullptr, planted_handler, nullptr, nullptr, nullptr};

/// Reads everything a client could ask for at a stop.
void read_everything(GRDBG_Debugger * d) {
  size_t total = 0, available = 0;
  ASSERT_EQ(grdbg_debugger_frames(d, &total, &available), GRDBG_OK);
  for (size_t f = 0; f < available; ++f) {
    GRDBG_Frame frame;
    ASSERT_EQ(grdbg_debugger_frame(d, f, &frame), GRDBG_OK);
    size_t scopes = 0;
    ASSERT_EQ(grdbg_debugger_scope_count(d, f, &scopes), GRDBG_OK);
    for (size_t s = 0; s < scopes; ++s) {
      GRDBG_Scope scope;
      ASSERT_EQ(grdbg_debugger_scope(d, f, s, &scope), GRDBG_OK);
      for (size_t v = 0; v < scope.variable_count; ++v) {
        GRDBG_Variable variable;
        ASSERT_EQ(grdbg_debugger_variable(d, f, s, v, &variable), GRDBG_OK);
      }
    }
  }
}

Outcome run_mode(Mode mode) {
  Outcome out;
  ToyWorld w(program());
  w.trace_polls();  // every poll, in every mode, takes the slow path and is recorded
  Planted planted;
  if (mode != Mode::Plain && mode != Mode::PlantedFuel && mode != Mode::PlantedSlot) {
    EXPECT_EQ(w.attach(), GRDBG_OK);
  }
  if (mode == Mode::NeverReached) {
    int lines[] = {7, 99};
    uint64_t ids[2];
    EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", lines, 2, ids), GRDBG_OK);
    EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "zzz.toy", lines, 2, ids), GRDBG_OK);
  }
  if (mode == Mode::ContinueAtEveryStop) {
    int a[] = {1, 2, 3, 4, 5, 6, 10, 11, 12, 13, 14, 15};
    int b[] = {20, 21};
    uint64_t ids[12];
    EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", a, 12, ids), GRDBG_OK);
    EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "b.toy", b, 2, ids), GRDBG_OK);
  }
  if (mode == Mode::PlantedFuel || mode == Mode::PlantedSlot) {
    planted.charge = mode == Mode::PlantedFuel;
    planted.write_slot = mode == Mode::PlantedSlot;
    EXPECT_EQ(grcore_context_request_kind(w.ctx, &kPlantedKey, &planted.kind), GRCORE_OK);
    EXPECT_EQ(grcore_context_port(w.ctx, &planted.port), GRCORE_OK);
    EXPECT_EQ(grcore_context_register(w.ctx, &kPlantedKey, &planted), GRCORE_OK);
    EXPECT_EQ(grcore_port_post(planted.port, planted.kind), GRCORE_OK);
  }

  GRCORE_Outcome outcome;
  out.result = w.run(&outcome);
  while (out.result == GRCORE_OK && outcome == GRCORE_OUTCOME_PAUSED) {
    out.stops++;
    out.armed_ever = out.armed_ever || (w.dbg != nullptr && grdbg_debugger_armed(w.dbg));
    GRDBG_Stop stop;
    EXPECT_EQ(grdbg_debugger_stopped(w.dbg, &stop), GRDBG_OK);
    EXPECT_EQ(stop.reason, GRDBG_STOP_BREAKPOINT);
    read_everything(w.dbg);
    EXPECT_EQ(grdbg_debugger_continue(w.dbg), GRDBG_OK);
    out.result = w.resume(&outcome);
  }
  if (w.dbg != nullptr) {
    out.armed_ever = out.armed_ever || grdbg_debugger_armed(w.dbg);
  }
  out.output = w.toy.output;
  out.fuel_used = grcore_context_fuel_used(w.ctx);
  out.memory_in_use = grcore_context_memory_in_use(w.ctx);
  out.memory_peak = grcore_context_memory_peak(w.ctx);
  out.polls = w.toy.polls;
  out.trace = std::move(w.trace->trace);
  grcore_port_release(planted.port);
  return out;
}

/// The first difference between two runs, or an empty string.
std::string diff(const Outcome & a, const Outcome & b) {
  std::ostringstream s;
  if (a.output != b.output) {
    s << "output: `" << a.output << "` against `" << b.output << "`";
  }
  else if (a.result != b.result) {
    s << "outcome: " << a.result << " against " << b.result;
  }
  else if (a.fuel_used != b.fuel_used) {
    s << "fuel used: " << a.fuel_used << " against " << b.fuel_used;
  }
  else if (a.memory_in_use != b.memory_in_use) {
    s << "memory in use: " << a.memory_in_use << " against " << b.memory_in_use;
  }
  else if (a.memory_peak != b.memory_peak) {
    s << "memory peak: " << a.memory_peak << " against " << b.memory_peak;
  }
  else if (a.polls != b.polls) {
    s << "polls: " << a.polls << " against " << b.polls;
  }
  else {
    observer::Divergence d;
    if (observer::first_divergence(a.trace, b.trace, &d)) {
      s << "trace: " << d.str();
    }
  }
  return s.str();
}

}  // namespace

TEST(AttachChangesNothing, TheUnattendedRunIsTheBaselineAndRecordsEveryPoll) {
  Outcome plain = run_mode(Mode::Plain);
  EXPECT_EQ(plain.result, GRCORE_OK);
  EXPECT_EQ(plain.output, "r=3\nq=5\nr=3\nq=5\nr=3\nq=5\nx=4\n");
  EXPECT_GT(plain.polls, 20u);
  // Every statement but the two that poll specially is polled once, plus those.
  EXPECT_EQ(plain.trace.total, plain.polls);
  EXPECT_GT(plain.memory_peak, 0u);
}

TEST(AttachChangesNothing, AttachedAndUnarmedEqualsUnattended) {
  Outcome plain = run_mode(Mode::Plain);
  Outcome unarmed = run_mode(Mode::Unarmed);
  EXPECT_EQ(diff(plain, unarmed), "");
  EXPECT_FALSE(unarmed.armed_ever);
  EXPECT_EQ(unarmed.stops, 0);
}

TEST(AttachChangesNothing, AttachedWithBreakpointsThatAreNeverReachedEqualsUnattended) {
  Outcome plain = run_mode(Mode::Plain);
  Outcome armed = run_mode(Mode::NeverReached);
  EXPECT_EQ(diff(plain, armed), "");
  EXPECT_TRUE(armed.armed_ever);
  EXPECT_EQ(armed.stops, 0);
}

TEST(AttachChangesNothing, AttachedAndContinuedAtEveryStopEqualsUnattended) {
  Outcome plain = run_mode(Mode::Plain);
  Outcome continued = run_mode(Mode::ContinueAtEveryStop);
  // The extra stops are not in the comparison: the engine polls at the same
  // places, and a stop is a return from `run` between two polls.
  EXPECT_EQ(diff(plain, continued), "");
  // Natives and nested activations cannot pause, so of the polls the engine
  // made, all but those two lines stopped: a poll per ordinary statement.
  EXPECT_GT(continued.stops, 20);
  EXPECT_LT(static_cast<uint64_t>(continued.stops), continued.polls);
  EXPECT_TRUE(continued.armed_ever);
}

TEST(AttachChangesNothing, ADebuggerThatChargesAUnitOfFuelIsCaught) {
  Outcome plain = run_mode(Mode::Plain);
  Outcome planted = run_mode(Mode::PlantedFuel);
  std::string d = diff(plain, planted);
  EXPECT_NE(d, "");
  EXPECT_NE(d.find("fuel used"), std::string::npos) << d;
}

TEST(AttachChangesNothing, ADebuggerThatWritesASlotIsCaught) {
  Outcome plain = run_mode(Mode::Plain);
  Outcome planted = run_mode(Mode::PlantedSlot);
  std::string d = diff(plain, planted);
  EXPECT_NE(d, "");
  EXPECT_NE(d.find("output"), std::string::npos) << d;
}

TEST(AttachChangesNothing, TheComparisonNamesTheFirstDifferingPoll) {
  Outcome plain = run_mode(Mode::Plain);
  Outcome other = run_mode(Mode::Plain);
  ASSERT_EQ(diff(plain, other), "");
  ASSERT_TRUE(observer::plant_slot_mismatch(&other.trace, 5, 0, toy::kHeader));
  std::string d = diff(plain, other);
  EXPECT_NE(d.find("poll 5"), std::string::npos) << d;
  Outcome shorter = run_mode(Mode::Plain);
  ASSERT_TRUE(observer::plant_missing_poll(&shorter.trace, 7));
  EXPECT_NE(diff(plain, shorter), "");
}

TEST(AttachChangesNothing, TheDebuggerAllocatesNothingFromTheContext) {
  // The context's allocator is the group's tracking allocator, charged through
  // the context's meter. Attaching, arming, stopping and reading leave its
  // count of blocks where an unattended run leaves it.
  auto blocks = [](bool attach) {
    ToyWorld w(program());
    if (attach) {
      EXPECT_EQ(w.attach(), GRDBG_OK);
      int lines[] = {3, 11};
      uint64_t ids[2];
      EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", lines, 2, ids), GRDBG_OK);
    }
    GRCORE_Outcome outcome;
    EXPECT_EQ(w.run(&outcome), GRCORE_OK);
    while (outcome == GRCORE_OUTCOME_PAUSED) {
      read_everything(w.dbg);
      EXPECT_EQ(grdbg_debugger_continue(w.dbg), GRDBG_OK);
      EXPECT_EQ(w.resume(&outcome), GRCORE_OK);
    }
    return std::make_pair(grcore_context_memory_peak(w.ctx), grcore_context_memory_blocks(w.ctx));
  };
  EXPECT_EQ(blocks(false), blocks(true));
}

GRDBG_TEST_MAIN()
