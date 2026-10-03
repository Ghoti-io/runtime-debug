/**
 * @file
 *
 * Helpers for the protocol tests: building a client's bytes, reading a
 * server's, and the host loop of the header (the same one examples/ uses).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRDBG_TESTS_DAP_HELPERS_H
#define GHOTI_IO_GRDBG_TESTS_DAP_HELPERS_H

#include "test_helpers.h"

#include <ghoti.io/text/json.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <thread>
#include <string>
#include <vector>

namespace dap {

/// `Content-Length: N\r\n\r\n` and the body.
inline std::string frame(const std::string & body) {
  return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

/// The client's side of a session: requests with increasing seqs.
class Script {
 public:
  /// Appends a request. `arguments` is the JSON text of the object, or empty.
  Script & request(const std::string & command, const std::string & arguments = "") {
    std::string body = "{\"seq\":" + std::to_string(++seq_) + ",\"type\":\"request\",\"command\":\"" +
        command + "\"";
    if (!arguments.empty()) {
      body += ",\"arguments\":" + arguments;
    }
    body += "}";
    bodies_.push_back(body);
    bytes_ += frame(body);
    return *this;
  }
  /// Appends bytes as they are.
  Script & raw(const std::string & bytes) {
    bytes_ += bytes;
    return *this;
  }
  const std::string & bytes() const { return bytes_; }
  int64_t last_seq() const { return seq_; }
  /// The request bodies, in order (what a client that waits for each response
  /// sends one at a time).
  const std::vector<std::string> & bodies() const { return bodies_; }

 private:
  int64_t seq_ = 0;
  std::string bytes_;
  std::vector<std::string> bodies_;
};

/// A parsed message. Owns its value.
class Msg {
 public:
  Msg() = default;
  explicit Msg(const std::string & text) : text_(text) {
    value_ = gtext_json_parse(text.data(), text.size(), nullptr, nullptr);
  }
  Msg(const Msg &) = delete;
  Msg & operator=(const Msg &) = delete;
  Msg(Msg && o) noexcept : text_(std::move(o.text_)), value_(o.value_) { o.value_ = nullptr; }
  Msg & operator=(Msg && o) noexcept {
    if (this != &o) {
      gtext_json_free(value_);
      text_ = std::move(o.text_);
      value_ = o.value_;
      o.value_ = nullptr;
    }
    return *this;
  }
  ~Msg() { gtext_json_free(value_); }

  bool ok() const { return value_ != nullptr; }
  const std::string & text() const { return text_; }
  const GTEXT_JSON_Value * root() const { return value_; }

  /// A value by a dotted path of keys and indexes: "body.stackFrames.0.line".
  const GTEXT_JSON_Value * at(const std::string & path) const {
    const GTEXT_JSON_Value * v = value_;
    size_t i = 0;
    while (v != nullptr && i <= path.size()) {
      size_t dot = path.find('.', i);
      std::string key = path.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
      if (gtext_json_typeof(v) == GTEXT_JSON_ARRAY) {
        v = gtext_json_array_get(v, static_cast<size_t>(std::stoul(key)));
      }
      else {
        v = gtext_json_object_get(v, key.data(), key.size());
      }
      if (dot == std::string::npos) {
        break;
      }
      i = dot + 1;
    }
    return v;
  }
  bool has(const std::string & path) const { return at(path) != nullptr; }
  std::string str(const std::string & path) const {
    const char * s = nullptr;
    size_t n = 0;
    const GTEXT_JSON_Value * v = at(path);
    if (v == nullptr || gtext_json_get_string(v, &s, &n) != GTEXT_JSON_OK) {
      return "<none>";
    }
    return std::string(s, n);
  }
  int64_t num(const std::string & path, int64_t fallback = -1) const {
    int64_t x = 0;
    const GTEXT_JSON_Value * v = at(path);
    return v != nullptr && gtext_json_get_i64(v, &x) == GTEXT_JSON_OK ? x : fallback;
  }
  bool boolean(const std::string & path) const {
    bool b = false;
    const GTEXT_JSON_Value * v = at(path);
    return v != nullptr && gtext_json_get_bool(v, &b) == GTEXT_JSON_OK && b;
  }
  size_t size(const std::string & path) const { return gtext_json_array_size(at(path)); }

  // The shapes the protocol fixes.
  bool is_response() const { return str("type") == "response"; }
  bool is_event() const { return str("type") == "event"; }
  bool success() const { return boolean("success"); }
  std::string command() const { return str("command"); }
  std::string event() const { return str("event"); }
  std::string message() const { return str("message"); }

 private:
  std::string text_;
  GTEXT_JSON_Value * value_ = nullptr;
};

/// Splits a server's bytes into message bodies. `leftover` is non-empty if the
/// bytes ended in the middle of a message or held a bad frame.
inline std::vector<std::string> split(const std::string & bytes, std::string * leftover = nullptr) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < bytes.size()) {
    size_t end = bytes.find("\r\n\r\n", pos);
    if (end == std::string::npos || bytes.compare(pos, 16, "Content-Length: ") != 0) {
      break;
    }
    size_t n = std::stoul(bytes.substr(pos + 16, end - pos - 16));
    if (end + 4 + n > bytes.size()) {
      break;
    }
    out.push_back(bytes.substr(end + 4, n));
    pos = end + 4 + n;
  }
  if (leftover != nullptr) {
    *leftover = bytes.substr(pos);
  }
  return out;
}

/// Everything a server wrote, as parsed messages.
inline std::vector<Msg> messages(const std::string & bytes) {
  std::vector<Msg> out;
  for (const std::string & body : split(bytes)) {
    out.emplace_back(body);
  }
  return out;
}

/// The first response to `command`, or null.
inline const Msg * response_to(const std::vector<Msg> & all, const std::string & command) {
  for (const Msg & m : all) {
    if (m.is_response() && m.command() == command) {
      return &m;
    }
  }
  return nullptr;
}

inline std::vector<const Msg *> events(const std::vector<Msg> & all, const std::string & name) {
  std::vector<const Msg *> out;
  for (const Msg & m : all) {
    if (m.is_event() && m.event() == name) {
      out.push_back(&m);
    }
  }
  return out;
}

/// What the host loop did.
struct HostResult {
  std::vector<GRDBG_ServeResult> serves;
  GRDBG_Result serve_error = GRDBG_OK;
  GRCORE_Result run_result = GRCORE_OK;
  int pauses = 0;
  bool finished = false;
};

/// The host loop of dap.h: serve until configured, run, and at every pause
/// notify, serve and resume. `on_foreign_pause` is called when something other
/// than the debugger paused the context (a budget); it may raise the budget.
inline HostResult drive(ToyWorld & w, GRDBG_Dap * dap, const std::function<void(ToyWorld &)> & on_foreign_pause = nullptr) {
  HostResult h;
  GRDBG_ServeResult serve;
  h.serve_error = grdbg_dap_serve(dap, &serve);
  if (h.serve_error != GRDBG_OK) {
    return h;
  }
  h.serves.push_back(serve);
  if (serve == GRDBG_SERVE_TERMINATE) {
    grdbg_dap_notify_finished(dap, 1);
    return h;
  }
  GRCORE_Outcome outcome = GRCORE_OUTCOME_FINISHED;
  h.run_result = w.run(&outcome);
  while (h.run_result == GRCORE_OK && outcome == GRCORE_OUTCOME_PAUSED) {
    ++h.pauses;
    EXPECT_EQ(grdbg_dap_notify_stopped(dap), GRDBG_OK);
    bool ours = false;
    for (size_t i = 0; i < grcore_context_pause_key_count(w.ctx); ++i) {
      ours = ours || grcore_context_pause_key(w.ctx, i) == grdbg_debugger_key();
    }
    if (!ours && on_foreign_pause) {
      on_foreign_pause(w);
    }
    h.serve_error = grdbg_dap_serve(dap, &serve);
    if (h.serve_error != GRDBG_OK) {
      // The session is over and the debugger disarmed: run free.
      h.run_result = w.resume(&outcome);
      continue;
    }
    h.serves.push_back(serve);
    if (serve == GRDBG_SERVE_TERMINATE) {
      EXPECT_EQ(grcore_context_terminate(w.ctx), GRCORE_OK);
    }
    h.run_result = w.resume(&outcome);
  }
  h.finished = h.run_result == GRCORE_OK && outcome == GRCORE_OUTCOME_FINISHED;
  EXPECT_EQ(grdbg_dap_notify_finished(dap, h.run_result == GRCORE_OK ? 0 : 1), GRDBG_OK);
  // The client's `disconnect` comes after `terminated`, and wants an answer.
  for (int i = 0; i < 16 && h.serve_error == GRDBG_OK; ++i) {
    h.serve_error = grdbg_dap_serve(dap, &serve);
    if (h.serve_error != GRDBG_OK || serve != GRDBG_SERVE_RESUME) {
      break;
    }
  }
  return h;
}

/// A transport that hands bytes out one at a time, collects what is written,
/// and fails on request. The torn case, and the broken ones.
struct Wire {
  std::string input;
  size_t position = 0;
  size_t chunk = 1;          // bytes per read
  size_t total_read = 0;     // bytes handed to the library
  size_t reads = 0;
  std::string output;
  size_t writes = 0;
  bool fail_read = false;
  bool fail_write = false;
  bool closed = false;

  GRDBG_Transport transport() {
    GRDBG_Transport t;
    t.user = this;
    t.read = [](void * u, void * buffer, size_t capacity, size_t * out) -> GRDBG_Result {
      auto * w = static_cast<Wire *>(u);
      ++w->reads;
      if (w->fail_read) {
        return GRDBG_ERR_IO;
      }
      size_t n = std::min({capacity, w->chunk, w->input.size() - w->position});
      std::memcpy(buffer, w->input.data() + w->position, n);
      w->position += n;
      w->total_read += n;
      *out = n;
      return GRDBG_OK;
    };
    t.write = [](void * u, const void * buffer, size_t n) -> GRDBG_Result {
      auto * w = static_cast<Wire *>(u);
      ++w->writes;
      if (w->fail_write) {
        return GRDBG_ERR_IO;
      }
      w->output.append(static_cast<const char *>(buffer), n);
      return GRDBG_OK;
    };
    t.close = [](void * u) -> GRDBG_Result {
      static_cast<Wire *>(u)->closed = true;
      return GRDBG_OK;
    };
    return t;
  }
};

}  // namespace dap

#endif
