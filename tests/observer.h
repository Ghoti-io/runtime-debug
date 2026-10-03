/**
 * @file
 *
 * The frame-differential observer (CAP-7, AD-16, AD-18): a header-only
 * instrument that records, at every poll, what the abstract frame walk shows,
 * and compares two such traces poll by poll.
 *
 * It registers an OBSERVE handler with runtime-core, plus a request kind that
 * is posted once and stays pending, so that every poll takes the slow path and
 * the handler runs at each of them (the fast path runs nothing). Per poll it
 * records the verdict and, for each frame, the depth, the poll identity, the
 * location (file and line), the slot count, each slot's kind and inspected
 * text, and each scope's kind and name and each variable's name and inspected
 * text. It reads the frame walk and nothing else: never the engine's own
 * structures, so it is the same instrument for any engine behind the frame
 * protocol (story 15 reuses it for interpreter against JIT, story 12 for a
 * debugger attached against absent) and sees what a debugger or a recorder
 * would see.
 *
 * What it does not record, and why: a slot's raw bits when the slot holds an
 * engine value (its inspected text is recorded instead, because the bits are
 * heap addresses that differ from run to run and under a moving stack); the
 * address of a frame. The raw header words of a frame (function, pc, sp,
 * flags) are recorded as the text the inspector gives them, and they are
 * stable.
 *
 * The comparison reports the first divergence with its poll index and the
 * frame, and says what differs. A trace that is a prefix of the other
 * diverges at the first poll one lacks.
 *
 * Nothing here is specific to any engine. This is a copy of lang-tang's
 * tests/observer.h, kept here because debug and lang-tang stay separate
 * libraries (AD-2): story 12 uses it for the debugger attached against
 * absent, the use its design note names.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRDBG_TESTS_OBSERVER_H
#define GHOTI_IO_GRDBG_TESTS_OBSERVER_H

#include <ghoti.io/runtime-core/runtime-core.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace observer {

struct SlotRecord {
  GRCORE_SlotKind kind = GRCORE_SLOT_RAW;
  std::string text;
};

struct VariableRecord {
  std::string name;
  GRCORE_SlotKind kind = GRCORE_SLOT_RAW;
  std::string text;
};

struct ScopeRecord {
  GRCORE_ScopeKind kind = GRCORE_SCOPE_LOCAL;
  std::string name;
  std::vector<VariableRecord> variables;
};

struct FrameRecord {
  size_t depth = 0;
  std::string engine;
  uint64_t function = 0;  ///< The poll identity's function word.
  uint64_t offset = 0;    ///< The poll identity's offset.
  std::string file;
  int line = 0;
  size_t slot_count = 0;
  std::vector<SlotRecord> slots;
  std::vector<ScopeRecord> scopes;
};

struct PollRecord {
  bool captured = true;  ///< False if the frame walk failed: the frames are then not a record of anything.
  GRCORE_Verdict verdict = GRCORE_VERDICT_CONTINUE;
  std::vector<FrameRecord> frames;
};

struct Trace {
  std::vector<PollRecord> polls;
  /// The most polls recorded; later ones are counted and not recorded, in
  /// every run alike.
  size_t limit = 20000;
  size_t total = 0;
};

namespace detail {

inline std::string slot_text(const GRCORE_AbstractFrame & frame, size_t index) {
  char small[256];
  size_t length = 0;
  if (grcore_frame_inspect(&frame, index, small, sizeof(small), &length) != GRCORE_OK) {
    return "<unreadable>";
  }
  if (length < sizeof(small)) {
    return std::string(small, length);
  }
  std::string big(length + 1, '\0');
  size_t again = 0;
  if (grcore_frame_inspect(&frame, index, big.data(), big.size(), &again) != GRCORE_OK) {
    return "<unreadable>";
  }
  big.resize(again < big.size() ? again : big.size() - 1);
  return big;
}

inline std::string variable_text(const GRCORE_Context * context, const GRCORE_AbstractFrame & frame, const GRCORE_Variable & variable) {
  char small[256];
  size_t length = 0;
  if (grcore_engine_inspect(context, frame.engine, variable.kind, variable.value, small, sizeof(small), &length) != GRCORE_OK) {
    return "<unreadable>";
  }
  if (length < sizeof(small)) {
    return std::string(small, length);
  }
  std::string big(length + 1, '\0');
  size_t again = 0;
  if (grcore_engine_inspect(context, frame.engine, variable.kind, variable.value, big.data(), big.size(), &again) != GRCORE_OK) {
    return "<unreadable>";
  }
  big.resize(again < big.size() ? again : big.size() - 1);
  return big;
}

}  // namespace detail

/// Reads the whole stack through the frame walk. Valid where the walk is: at a
/// poll, in a handler, or while a run is paused and the caller holds it.
inline bool capture(const GRCORE_Context * context, std::vector<FrameRecord> * out) {
  GRCORE_FrameWalk walk;
  if (grcore_frame_walk_begin(context, &walk) != GRCORE_OK) {
    return false;
  }
  GRCORE_AbstractFrame frame;
  while (grcore_frame_walk_next(&walk, &frame)) {
    FrameRecord rec;
    rec.depth = frame.depth;
    rec.engine = frame.descriptor && frame.descriptor->name ? frame.descriptor->name : "";
    rec.function = frame.identity.function;
    rec.offset = frame.identity.offset;
    rec.file = frame.location.file ? frame.location.file : "";
    rec.line = frame.location.line;
    rec.slot_count = frame.slot_count;
    for (size_t i = 0; i < frame.slot_count; ++i) {
      SlotRecord slot;
      uint64_t value = 0;
      if (grcore_frame_slot(&frame, i, &slot.kind, &value) != GRCORE_OK) {
        slot.text = "<unreadable>";
      }
      else {
        slot.text = detail::slot_text(frame, i);
      }
      rec.slots.push_back(slot);
    }
    size_t scopes = grcore_frame_scope_count(&frame);
    for (size_t s = 0; s < scopes; ++s) {
      GRCORE_ScopeInfo info;
      if (grcore_frame_scope(&frame, s, &info) != GRCORE_OK) {
        continue;
      }
      ScopeRecord scope;
      scope.kind = info.kind;
      scope.name = info.name ? info.name : "";
      for (size_t v = 0; v < info.variable_count; ++v) {
        GRCORE_Variable variable;
        if (grcore_frame_variable(&frame, s, v, &variable) != GRCORE_OK) {
          continue;
        }
        VariableRecord var;
        var.name = variable.name ? variable.name : "";
        var.kind = variable.kind;
        var.text = detail::variable_text(context, frame, variable);
        scope.variables.push_back(var);
      }
      rec.scopes.push_back(scope);
    }
    out->push_back(rec);
  }
  return true;
}

/// The handler's state. Attach it to a context before the run starts.
class Observer {
 public:
  Trace trace;

  Observer() = default;
  Observer(const Observer &) = delete;
  Observer & operator=(const Observer &) = delete;
  ~Observer() { grcore_port_release(port_); }

  /// Registers the handler and posts the request that keeps every poll on the
  /// slow path. The context must be parked and owned by the caller.
  GRCORE_Result attach(GRCORE_Context * context) {
    GRCORE_Result r = grcore_context_request_kind(context, &key(), &kind_);
    if (r == GRCORE_OK) {
      r = grcore_context_port(context, &port_);
    }
    if (r == GRCORE_OK) {
      r = grcore_context_register(context, &key(), this);
    }
    if (r == GRCORE_OK) {
      r = grcore_port_post(port_, kind_);
    }
    return r;
  }

 private:
  GRCORE_Port * port_ = nullptr;
  GRCORE_RequestKind kind_ = 0;

  static void handler(GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
    Observer * self = static_cast<Observer *>(value);
    ++self->trace.total;
    if (self->trace.polls.size() >= self->trace.limit) {
      return;
    }
    PollRecord poll;
    poll.verdict = grcore_pollcall_verdict(call);
    poll.captured = capture(context, &poll.frames);
    self->trace.polls.push_back(std::move(poll));
  }

  static const GRCORE_Key & key() {
    static const GRCORE_Key k = {"frame observer", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_OBSERVE, nullptr, &Observer::handler};
    return k;
  }
};

// ---------------------------------------------------------------------------
// The comparison
// ---------------------------------------------------------------------------

struct Divergence {
  size_t poll = 0;         ///< The index of the first poll that differs.
  bool has_frame = false;
  size_t frame = 0;        ///< Which frame (0 is the innermost).
  std::string what;        ///< What differs.
  std::string left, right; ///< The two values, as text.

  std::string str() const {
    std::string s = "poll " + std::to_string(poll);
    if (has_frame) {
      s += ", frame " + std::to_string(frame);
    }
    s += ": " + what + ": `" + left + "` against `" + right + "`";
    return s;
  }
};

namespace detail {

inline const char * verdict_name(GRCORE_Verdict v) {
  switch (v) {
    case GRCORE_VERDICT_CONTINUE: return "continue";
    case GRCORE_VERDICT_PAUSE: return "pause";
    case GRCORE_VERDICT_UNWIND: return "unwind";
  }
  return "?";
}

inline bool differ(Divergence * d, size_t poll, bool has_frame, size_t frame, const std::string & what, const std::string & a, const std::string & b) {
  d->poll = poll;
  d->has_frame = has_frame;
  d->frame = frame;
  d->what = what;
  d->left = a;
  d->right = b;
  return true;
}

inline std::string where(const FrameRecord & f) {
  return f.file + ":" + std::to_string(f.line);
}

/// Whether two frames differ, and how.
inline bool compare_frames(const FrameRecord & a, const FrameRecord & b, size_t poll, size_t index, Divergence * d) {
  auto at = [&](const std::string & what, const std::string & x, const std::string & y) {
    return differ(d, poll, true, index, what, x, y);
  };
  if (a.depth != b.depth) {
    return at("depth", std::to_string(a.depth), std::to_string(b.depth));
  }
  if (a.engine != b.engine) {
    return at("engine", a.engine, b.engine);
  }
  if (a.function != b.function || a.offset != b.offset) {
    return at("poll identity", std::to_string(a.function) + "/" + std::to_string(a.offset), std::to_string(b.function) + "/" + std::to_string(b.offset));
  }
  if (a.file != b.file || a.line != b.line) {
    return at("location", where(a), where(b));
  }
  if (a.slot_count != b.slot_count || a.slots.size() != b.slots.size()) {
    return at("slot count", std::to_string(a.slot_count), std::to_string(b.slot_count));
  }
  for (size_t i = 0; i < a.slots.size(); ++i) {
    if (a.slots[i].kind != b.slots[i].kind) {
      return at("slot " + std::to_string(i) + " kind", a.slots[i].kind == GRCORE_SLOT_RAW ? "raw" : "value", b.slots[i].kind == GRCORE_SLOT_RAW ? "raw" : "value");
    }
    if (a.slots[i].text != b.slots[i].text) {
      return at("slot " + std::to_string(i) + " text", a.slots[i].text, b.slots[i].text);
    }
  }
  if (a.scopes.size() != b.scopes.size()) {
    return at("scope count", std::to_string(a.scopes.size()), std::to_string(b.scopes.size()));
  }
  for (size_t s = 0; s < a.scopes.size(); ++s) {
    const ScopeRecord & x = a.scopes[s];
    const ScopeRecord & y = b.scopes[s];
    if (x.kind != y.kind || x.name != y.name) {
      return at("scope " + std::to_string(s), x.name, y.name);
    }
    if (x.variables.size() != y.variables.size()) {
      return at("scope " + x.name + " variable count", std::to_string(x.variables.size()), std::to_string(y.variables.size()));
    }
    for (size_t v = 0; v < x.variables.size(); ++v) {
      if (x.variables[v].name != y.variables[v].name) {
        return at("scope " + x.name + " variable " + std::to_string(v) + " name", x.variables[v].name, y.variables[v].name);
      }
      if (x.variables[v].text != y.variables[v].text) {
        return at("scope " + x.name + " variable `" + x.variables[v].name + "`", x.variables[v].text, y.variables[v].text);
      }
    }
  }
  return false;
}

}  // namespace detail

/// The first divergence between two traces, poll by poll; true if there is one.
inline bool first_divergence(const Trace & a, const Trace & b, Divergence * out) {
  size_t common = a.polls.size() < b.polls.size() ? a.polls.size() : b.polls.size();
  for (size_t p = 0; p < common; ++p) {
    const PollRecord & x = a.polls[p];
    const PollRecord & y = b.polls[p];
    if (!x.captured || !y.captured) {
      return detail::differ(out, p, false, 0, "frame walk failed", x.captured ? "captured" : "not captured", y.captured ? "captured" : "not captured");
    }
    if (x.verdict != y.verdict) {
      return detail::differ(out, p, false, 0, "verdict", detail::verdict_name(x.verdict), detail::verdict_name(y.verdict));
    }
    if (x.frames.size() != y.frames.size()) {
      // The first frame one stack has and the other lacks is the one named.
      return detail::differ(out, p, true, x.frames.size() < y.frames.size() ? x.frames.size() : y.frames.size(), "depth (frame count)",
          std::to_string(x.frames.size()), std::to_string(y.frames.size()));
    }
    for (size_t f = 0; f < x.frames.size(); ++f) {
      if (detail::compare_frames(x.frames[f], y.frames[f], p, f, out)) {
        return true;
      }
    }
  }
  if (a.polls.size() != b.polls.size() || a.total != b.total) {
    return detail::differ(out, common, false, 0, "number of polls", std::to_string(a.total), std::to_string(b.total));
  }
  return false;
}

// ---------------------------------------------------------------------------
// Planted mismatches, for the tests that show the instrument fails
// ---------------------------------------------------------------------------

/// Alters one slot's text in one poll of one trace. Returns false if there is
/// no such slot.
inline bool plant_slot_mismatch(Trace * trace, size_t poll, size_t frame, size_t slot) {
  if (poll >= trace->polls.size() || frame >= trace->polls[poll].frames.size() || slot >= trace->polls[poll].frames[frame].slots.size()) {
    return false;
  }
  trace->polls[poll].frames[frame].slots[slot].text += "!";
  return true;
}

/// Removes one poll from a trace (a poll the other run had and this one missed).
inline bool plant_missing_poll(Trace * trace, size_t poll) {
  if (poll >= trace->polls.size()) {
    return false;
  }
  trace->polls.erase(trace->polls.begin() + (long)poll);
  --trace->total;
  return true;
}

/// Drops the outermost frame of one poll, so the depth differs.
inline bool plant_different_depth(Trace * trace, size_t poll) {
  if (poll >= trace->polls.size() || trace->polls[poll].frames.empty()) {
    return false;
  }
  trace->polls[poll].frames.pop_back();
  return true;
}

}  // namespace observer

#endif
