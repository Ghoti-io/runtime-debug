/**
 * @file
 *
 * An engine-free toy engine for the debugger's tests, and the world it runs in.
 *
 * `runtime-debug` never names an engine, so its tests need one that is not a
 * language: a descriptor with `locate`, `inspect` and a scope interface, and
 * functions made of numbered lines that push frames through A the way a real
 * interpreter does. A statement is one of:
 *
 *   SET var imm, ADD var imm, PRINT var   locals, and the program's output
 *   GSET imm, GADD imm                     the one global ("Globals" scope)
 *   CALL fn                                pushes a frame; the caller's call-site
 *                                          identity is recorded first
 *   JNZ var target, JG target, JMP target  control flow within a function
 *   LOOP var target                        var -= 1, and jump if it is not zero:
 *                                          a one-statement loop when it jumps
 *                                          to itself
 *   NATIVE                                 a native: calls grcore_runtime_poll
 *   NESTED                                 polls inside a nested activation
 *
 * It polls with `grcore_stack_poll` before every statement except the last two,
 * which poll in their own way, and it keeps its whole position in the guest
 * stack: a frame's slots are the function, the pc, a "polled" flag and then
 * the locals. After a pause the entry function carries on from the frame, and
 * the flag says the poll before the statement was already taken, so a resumed
 * program is not stopped twice at one poll (runtime-core's pause_resume
 * example does the same with a counter).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRDBG_TESTS_TOY_ENGINE_H
#define GHOTI_IO_GRDBG_TESTS_TOY_ENGINE_H

#include "observer.h"

#include <ghoti.io/runtime-core/runtime-core.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

namespace toy {

enum class Op {
  NOP, SET, ADD, PRINT, GSET, GADD, CALL, JNZ, JG, JMP, LOOP, NATIVE, NESTED
};

struct Stmt {
  Op op = Op::NOP;
  int line = 0;
  int var = 0;
  int64_t imm = 0;
  int target = 0;
  int callee = -1;
  std::string callee_name;
};

struct Function {
  std::string name;
  std::string file;
  std::vector<std::string> vars;
  std::vector<Stmt> code;

  int var(const std::string & v) const {
    for (size_t i = 0; i < vars.size(); ++i) {
      if (vars[i] == v) {
        return static_cast<int>(i);
      }
    }
    ADD_FAILURE() << "no variable " << v << " in " << name;
    return 0;
  }
  Function & push(Op op, int line) {
    Stmt s;
    s.op = op;
    s.line = line;
    code.push_back(s);
    return *this;
  }
  Function & nop(int line) { return push(Op::NOP, line); }
  Function & set(int line, const std::string & v, int64_t imm) {
    push(Op::SET, line);
    code.back().var = var(v);
    code.back().imm = imm;
    return *this;
  }
  Function & add(int line, const std::string & v, int64_t imm) {
    push(Op::ADD, line);
    code.back().var = var(v);
    code.back().imm = imm;
    return *this;
  }
  Function & print(int line, const std::string & v) {
    push(Op::PRINT, line);
    code.back().var = var(v);
    return *this;
  }
  Function & gset(int line, int64_t imm) {
    push(Op::GSET, line);
    code.back().imm = imm;
    return *this;
  }
  Function & gadd(int line, int64_t imm) {
    push(Op::GADD, line);
    code.back().imm = imm;
    return *this;
  }
  Function & call(int line, const std::string & callee) {
    push(Op::CALL, line);
    code.back().callee_name = callee;
    return *this;
  }
  Function & jnz(int line, const std::string & v, int target) {
    push(Op::JNZ, line);
    code.back().var = var(v);
    code.back().target = target;
    return *this;
  }
  Function & jg(int line, int target) {
    push(Op::JG, line);
    code.back().target = target;
    return *this;
  }
  Function & jmp(int line, int target) {
    push(Op::JMP, line);
    code.back().target = target;
    return *this;
  }
  Function & loop(int line, const std::string & v, int target) {
    push(Op::LOOP, line);
    code.back().var = var(v);
    code.back().target = target;
    return *this;
  }
  Function & native(int line) { return push(Op::NATIVE, line); }
  Function & nested(int line) { return push(Op::NESTED, line); }
};

struct Program {
  std::deque<Function> functions;
  int main = 0;

  Function & fn(const std::string & name, const std::string & file,
      std::vector<std::string> vars = {}) {
    functions.emplace_back();
    Function & f = functions.back();
    f.name = name;
    f.file = file;
    f.vars = std::move(vars);
    return f;
  }
  /// Resolves the callee names. Call once, after the last statement is added.
  Program & finalize() {
    for (Function & f : functions) {
      for (Stmt & s : f.code) {
        if (s.op != Op::CALL) {
          continue;
        }
        s.callee = -1;
        for (size_t i = 0; i < functions.size(); ++i) {
          if (functions[i].name == s.callee_name) {
            s.callee = static_cast<int>(i);
          }
        }
        EXPECT_GE(s.callee, 0) << "no function " << s.callee_name;
      }
    }
    return *this;
  }
};

/// The engine's state: the program, its output, and what a run did.
struct Toy {
  Program program;
  std::string output;
  int64_t global = 0;
  uint64_t polls = 0;
  bool started = false;
  GRCORE_EngineId engine = 0;
};

constexpr size_t kHeader = 3;  // function, pc, polled

inline const GRCORE_Key kToyKey = GRCORE_KEY_INIT("toy engine", GRCORE_CARDINALITY_ONE,
    GRCORE_PHASE_NONE, nullptr, nullptr, nullptr, nullptr, nullptr);

inline Toy * of(const GRCORE_Context * context) {
  return static_cast<Toy *>(grcore_context_slot(context, &kToyKey));
}

inline uint64_t slot_of(const GRCORE_AbstractFrame * frame, size_t index) {
  uint64_t v = 0;
  grcore_stack_slot_get(grcore_context_stack(frame->context), frame->frame, index, &v);
  return v;
}

inline GRCORE_SlotKind slot_kind(const GRCORE_AbstractFrame *, size_t index) {
  return index >= kHeader ? GRCORE_SLOT_VALUE : GRCORE_SLOT_RAW;
}

inline GRCORE_Location locate(const GRCORE_Context * context, uint64_t function, uint64_t offset) {
  Toy * t = of(context);
  if (t == nullptr || function >= t->program.functions.size()) {
    return GRCORE_Location{nullptr, 0};
  }
  const Function & f = t->program.functions[function];
  if (offset >= f.code.size()) {
    return GRCORE_Location{f.file.c_str(), 0};
  }
  return GRCORE_Location{f.file.c_str(), f.code[offset].line};
}

constexpr uint64_t kLongValue = 0xDEADBEEF;  // inspects as 3000 euro signs (9000 bytes)

inline size_t inspect(const GRCORE_Context *, GRCORE_SlotKind kind, uint64_t value, char * buffer, size_t size) {
  if (kind == GRCORE_SLOT_VALUE && value == kLongValue) {
    std::string text;
    for (int i = 0; i < 3000; ++i) {
      text += "\xe2\x82\xac";
    }
    if (size > 0) {
      std::snprintf(buffer, size, "%s", text.c_str());
    }
    return text.size();
  }
  int n = kind == GRCORE_SLOT_VALUE
      ? std::snprintf(buffer, size, "%lld", static_cast<long long>(static_cast<int64_t>(value)))
      : std::snprintf(buffer, size, "0x%llx", static_cast<unsigned long long>(value));
  return static_cast<size_t>(n);
}

inline const Function * function_of(const GRCORE_AbstractFrame * frame) {
  Toy * t = of(frame->context);
  uint64_t f = slot_of(frame, 0);
  return t != nullptr && f < t->program.functions.size() ? &t->program.functions[f] : nullptr;
}

inline size_t scope_count(const GRCORE_AbstractFrame *) { return 2; }

inline GRCORE_Result scope(const GRCORE_AbstractFrame * frame, size_t index, GRCORE_ScopeInfo * out) {
  const Function * f = function_of(frame);
  if (f == nullptr || index >= 2) {
    return GRCORE_ERR_INVALID;
  }
  if (index == 0) {
    *out = GRCORE_ScopeInfo{GRCORE_SCOPE_LOCAL, "Locals", f->vars.size()};
  }
  else {
    *out = GRCORE_ScopeInfo{GRCORE_SCOPE_GLOBAL, "Globals", 1};
  }
  return GRCORE_OK;
}

inline GRCORE_Result variable(const GRCORE_AbstractFrame * frame, size_t scope, size_t index, GRCORE_Variable * out) {
  const Function * f = function_of(frame);
  Toy * t = of(frame->context);
  if (f == nullptr || t == nullptr) {
    return GRCORE_ERR_INVALID;
  }
  if (scope == 0 && index < f->vars.size()) {
    *out = GRCORE_Variable{f->vars[index].c_str(), GRCORE_SLOT_VALUE, slot_of(frame, kHeader + index)};
    return GRCORE_OK;
  }
  if (scope == 1 && index == 0) {
    *out = GRCORE_Variable{"g", GRCORE_SLOT_VALUE, static_cast<uint64_t>(t->global)};
    return GRCORE_OK;
  }
  return GRCORE_ERR_INVALID;
}

inline const GRCORE_EngineDescriptor kDescriptor = GRCORE_ENGINE_DESCRIPTOR_INIT("toy", slot_kind, locate, inspect,
    {scope_count, scope, variable}, {0, 0, 0}, nullptr, nullptr, nullptr, nullptr);

inline bool push_frame(Toy * t, GRCORE_Context * context, int function) {
  GRCORE_Stack * stack = grcore_context_stack(context);
  const Function & f = t->program.functions[static_cast<size_t>(function)];
  GRCORE_FrameRef ref;
  if (grcore_stack_push(stack, t->engine, kHeader + f.vars.size(), &ref) != GRCORE_OK) {
    return false;
  }
  uint64_t * slots = grcore_stack_slots(stack, ref);
  slots[0] = static_cast<uint64_t>(function);
  return true;
}

/// The engine's entry function: `run` and `resume` both call it, and the
/// stack says where to carry on.
inline GRCORE_Step entry(GRCORE_Context * context, void * state) {
  Toy * t = static_cast<Toy *>(state);
  GRCORE_Stack * stack = grcore_context_stack(context);
  if (!t->started) {
    t->started = true;
    if (!push_frame(t, context, t->program.main)) {
      return GRCORE_STEP_UNWOUND;
    }
  }
  for (;;) {
    GRCORE_FrameRef top = grcore_stack_top(stack);
    if (top.offset == 0) {
      return GRCORE_STEP_FINISHED;
    }
    uint64_t * slots = grcore_stack_slots(stack, top);
    uint64_t fn = slots[0];
    uint64_t pc = slots[1];
    const Function & f = t->program.functions[fn];
    if (pc >= f.code.size()) {
      grcore_stack_pop(stack);
      continue;
    }
    const Stmt & s = f.code[pc];

    if (s.op == Op::NATIVE) {
      grcore_stack_set_identity(stack, top, GRCORE_PollIdentity{fn, pc});
      ++t->polls;
      if (grcore_runtime_poll(context, 1, locate(context, fn, pc)) != GRCORE_OK) {
        return GRCORE_STEP_UNWOUND;
      }
      slots = grcore_stack_slots(stack, top);
    }
    else if (s.op == Op::NESTED) {
      grcore_context_nested_enter(context);
      ++t->polls;
      GRCORE_Verdict v = grcore_stack_poll(context, fn, pc);
      grcore_context_nested_leave(context);
      if (v != GRCORE_VERDICT_CONTINUE) {
        return GRCORE_STEP_UNWOUND;  // a pause here is refused, so this is an unwind
      }
      slots = grcore_stack_slots(stack, top);
    }
    else if (slots[2] == 0) {
      grcore_context_charge_fuel(context, 1);
      ++t->polls;
      GRCORE_Verdict v = grcore_stack_poll(context, fn, pc);
      slots = grcore_stack_slots(stack, top);
      if (v == GRCORE_VERDICT_PAUSE) {
        slots[2] = 1;  // the poll before this statement is taken
        return GRCORE_STEP_PAUSED;
      }
      if (v == GRCORE_VERDICT_UNWIND) {
        return GRCORE_STEP_UNWOUND;
      }
    }
    else {
      slots[2] = 0;
    }

    int64_t next = static_cast<int64_t>(pc) + 1;
    switch (s.op) {
      case Op::NOP:
      case Op::NATIVE:
      case Op::NESTED:
        break;
      case Op::SET:
        slots[kHeader + static_cast<size_t>(s.var)] = static_cast<uint64_t>(s.imm);
        break;
      case Op::ADD:
        slots[kHeader + static_cast<size_t>(s.var)] += static_cast<uint64_t>(s.imm);
        break;
      case Op::PRINT:
        t->output += f.vars[static_cast<size_t>(s.var)] + "=" +
            std::to_string(static_cast<int64_t>(slots[kHeader + static_cast<size_t>(s.var)])) + "\n";
        break;
      case Op::GSET:
        t->global = s.imm;
        break;
      case Op::GADD:
        t->global += s.imm;
        break;
      case Op::JNZ:
        if (slots[kHeader + static_cast<size_t>(s.var)] != 0) {
          next = s.target;
        }
        break;
      case Op::JG:
        if (t->global != 0) {
          next = s.target;
        }
        break;
      case Op::JMP:
        next = s.target;
        break;
      case Op::LOOP:
        slots[kHeader + static_cast<size_t>(s.var)] -= 1;
        if (slots[kHeader + static_cast<size_t>(s.var)] != 0) {
          next = s.target;
        }
        break;
      case Op::CALL:
        grcore_stack_set_identity(stack, top, GRCORE_PollIdentity{fn, pc});
        slots[1] = static_cast<uint64_t>(next);
        if (!push_frame(t, context, s.callee)) {
          return GRCORE_STEP_UNWOUND;
        }
        continue;
    }
    slots[1] = static_cast<uint64_t>(next);
  }
}

}  // namespace toy

#endif
