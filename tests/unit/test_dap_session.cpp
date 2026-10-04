/**
 * @file
 *
 * A scripted Debug Adapter Protocol session against the toy engine, compared
 * with a stored transcript, over a memory transport, over a socket pair, and
 * over loopback TCP that the test binds and accepts.
 *
 * The socket transports are POSIX descriptors and do not exist on Windows (the
 * descriptor transport is a stub there); the two tests are reported SKIPPED and
 * the memory transport carries the transcript.
 *
 * The three transports carry one transcript: the adapter is a function of the
 * requests, not of how the bytes arrive. The stored transcript is a
 * requirement written down, not an observation: it was read through against
 * the Debug Adapter Protocol 1.71 and the task's description when it was
 * made (GRDBG_UPDATE_GOLDEN=1 rewrites it, and the diff is then to be read).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "dap_helpers.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace {

/// main calls f. (Lines are the toy engine's, not the file's.)
toy::Program session_program() {
  toy::Program p;
  p.fn("main", "a.toy", {"x", "y"}).set(1, "x", 3).call(2, "f").print(3, "x").set(4, "y", 7).print(5, "y");
  p.fn("f", "a.toy", {"r"}).set(10, "r", 1).add(11, "r", 1).print(12, "r");
  return p;
}

/// The whole conversation: set a breakpoint, hit it, read the stack, locals and
/// scopes, step in, over and out, and continue.
dap::Script session_script() {
  dap::Script s;
  s.request("initialize", R"({"clientID":"test","adapterID":"toy","linesStartAt1":true,"columnsStartAt1":true})")
      .request("launch", "{}")
      .request("setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[{"line":2}]})")
      .request("setExceptionBreakpoints", R"({"filters":[]})")
      .request("configurationDone")
      .request("threads")
      .request("stackTrace", R"({"threadId":1})")
      .request("scopes", R"({"frameId":1})")
      .request("variables", R"({"variablesReference":1})")
      .request("variables", R"({"variablesReference":2})")
      .request("evaluate", R"({"expression":"x","frameId":1,"context":"hover"})")
      .request("evaluate", R"({"expression":"x + 1","frameId":1,"context":"repl"})")
      .request("stepIn", R"({"threadId":1})")
      .request("stackTrace", R"({"threadId":1})")
      .request("scopes", R"({"frameId":2})")
      .request("variables", R"({"variablesReference":1})")
      .request("scopes", R"({"frameId":3})")
      .request("variables", R"({"variablesReference":3})")
      .request("next", R"({"threadId":1})")
      .request("stackTrace", R"({"threadId":1,"levels":1})")
      .request("stepOut", R"({"threadId":1})")
      .request("stackTrace", R"({"threadId":1})")
      .request("next", R"({"threadId":1})")
      .request("variables", R"({"variablesReference":99})")
      .request("continue", R"({"threadId":1})")
      .request("disconnect", R"({"terminateDebuggee":false})");
  return s;
}

std::string golden_path() {
  return std::string(GRDBG_TEST_DATA) + "/session.transcript";
}

std::string read_file(const std::string & path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/// One server message per line.
std::string transcript_of(const std::vector<std::string> & bodies) {
  std::string t;
  for (const std::string & b : bodies) {
    t += b + "\n";
  }
  return t;
}

/// Compares with the stored transcript, or rewrites it on request.
void expect_golden(const std::string & transcript) {
  if (std::getenv("GRDBG_UPDATE_GOLDEN") != nullptr) {
    std::ofstream out(golden_path(), std::ios::binary);
    out << transcript;
  }
  std::string golden = read_file(golden_path());
  ASSERT_FALSE(golden.empty()) << "no transcript at " << golden_path();
  EXPECT_EQ(transcript, golden);
}

/// What the program printed, run unattended.
std::string unattended_output() {
  ToyWorld w(session_program());
  GRCORE_Outcome outcome;
  EXPECT_EQ(w.run(&outcome), GRCORE_OK);
  return w.toy.output;
}

/// Serves a session on `transport` against a fresh toy world and returns what
/// the host did and the program's output.
struct Served {
  dap::HostResult host;
  std::string output;
};

Served serve_on(const GRDBG_Transport & transport) {
  Served served;
  ToyWorld w(session_program());
  EXPECT_EQ(w.attach(), GRDBG_OK);
  GRDBG_Dap * session = nullptr;
  EXPECT_EQ(grdbg_dap_create(w.dbg, &transport, nullptr, &session), GRDBG_OK);
  served.host = dap::drive(w, session);
  served.output = w.toy.output;
  grdbg_dap_destroy(session);
  return served;
}

#ifndef _WIN32
/// The client of a connected stream socket: sends each request, waits for its
/// response, and collects every server message in the order it arrives.
std::vector<std::string> lockstep_client(int fd, const dap::Script & script) {
  std::vector<std::string> received;
  std::string pending;
  auto read_more = [&]() {
    char buffer[4096];
    ssize_t n = read(fd, buffer, sizeof buffer);
    if (n > 0) {
      pending.append(buffer, static_cast<size_t>(n));
    }
    return n > 0;
  };
  auto take = [&]() {
    std::string leftover;
    std::vector<std::string> bodies = dap::split(pending, &leftover);
    pending = leftover;
    return bodies;
  };
  int64_t seq = 0;
  for (const std::string & body : script.bodies()) {
    ++seq;
    std::string bytes = dap::frame(body);
    size_t sent = 0;
    while (sent < bytes.size()) {
      ssize_t n = send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        return received;
      }
      sent += static_cast<size_t>(n);
    }
    bool answered = false;
    while (!answered) {
      for (const std::string & m : take()) {
        received.push_back(m);
        dap::Msg msg(m);
        if (msg.is_response() && msg.num("request_seq") == seq) {
          answered = true;
        }
      }
      if (!answered && !read_more()) {
        return received;
      }
    }
  }
  // Whatever the server still says, until it closes the connection.
  shutdown(fd, SHUT_WR);
  while (read_more()) {
  }
  for (const std::string & m : take()) {
    received.push_back(m);
  }
  return received;
}
#endif  // _WIN32

}  // namespace

TEST(DapSession, OverAMemoryTransportItMatchesTheStoredTranscript) {
  dap::Script script = session_script();
  GRDBG_Transport* memory;
  ASSERT_EQ(grdbg_transport_create_memory(script.bytes().data(), script.bytes().size(), nullptr, &memory), GRDBG_OK);
  Served served = serve_on(*memory);
  const uint8_t * out;
  size_t length;
  ASSERT_EQ(grdbg_transport_memory_output(memory, &out, &length), GRDBG_OK);
  std::string leftover;
  std::vector<std::string> bodies = dap::split(std::string(reinterpret_cast<const char *>(out), length), &leftover);
  EXPECT_EQ(leftover, "");
  expect_golden(transcript_of(bodies));
  grdbg_transport_destroy(memory);

  EXPECT_EQ(served.host.serve_error, GRDBG_OK);
  EXPECT_TRUE(served.host.finished);
  // The debugged run printed what the unattended one does.
  EXPECT_EQ(served.output, unattended_output());
}

TEST(DapSession, TheTranscriptSaysWhatTheTaskAsksOf) {
  // Not the golden file but the facts it is meant to hold, so that a
  // regenerated transcript with the wrong content cannot pass for the right one.
  dap::Script script = session_script();
  GRDBG_Transport * memory;
  ASSERT_EQ(grdbg_transport_create_memory(script.bytes().data(), script.bytes().size(), nullptr, &memory), GRDBG_OK);
  serve_on(*memory);
  const uint8_t * out;
  size_t length;
  ASSERT_EQ(grdbg_transport_memory_output(memory, &out, &length), GRDBG_OK);
  std::vector<dap::Msg> all = dap::messages(std::string(reinterpret_cast<const char *>(out), length));
  grdbg_transport_destroy(memory);

  // seq runs 1, 2, 3... and every response names its request and its command.
  int64_t seq = 0;
  for (const dap::Msg & m : all) {
    EXPECT_EQ(m.num("seq"), ++seq) << m.text();
    if (m.is_response()) {
      EXPECT_TRUE(m.has("request_seq")) << m.text();
      EXPECT_TRUE(m.has("success")) << m.text();
      EXPECT_NE(m.command(), "<none>") << m.text();
    }
  }
  // Capabilities, then `initialized`.
  const dap::Msg * init = dap::response_to(all, "initialize");
  ASSERT_NE(init, nullptr);
  EXPECT_TRUE(init->boolean("body.supportsConfigurationDoneRequest"));
  EXPECT_EQ(dap::events(all, "initialized").size(), 1u);

  // The stops, in order: the breakpoint, then three steps, then a step.
  std::vector<const dap::Msg *> stops = dap::events(all, "stopped");
  ASSERT_EQ(stops.size(), 5u);
  EXPECT_EQ(stops[0]->str("body.reason"), "breakpoint");
  EXPECT_EQ(stops[0]->num("body.hitBreakpointIds.0"), 1);
  for (size_t i = 1; i < 5; ++i) {
    EXPECT_EQ(stops[i]->str("body.reason"), "step");
  }
  EXPECT_EQ(dap::events(all, "exited").size(), 1u);
  EXPECT_EQ(dap::events(all, "terminated").size(), 1u);
  EXPECT_EQ(dap::events(all, "exited")[0]->num("body.exitCode"), 0);
  EXPECT_FALSE(dap::events(all, "continued").size());  // never sent

  // The stack at each stop.
  std::vector<const dap::Msg *> traces;
  for (const dap::Msg & m : all) {
    if (m.is_response() && m.command() == "stackTrace") {
      traces.push_back(&m);
    }
  }
  ASSERT_EQ(traces.size(), 4u);
  EXPECT_EQ(traces[0]->str("body.stackFrames.0.source.path"), "a.toy");
  EXPECT_EQ(traces[0]->num("body.stackFrames.0.line"), 2);
  EXPECT_EQ(traces[0]->num("body.totalFrames"), 1);
  EXPECT_EQ(traces[1]->num("body.stackFrames.0.line"), 10);  // stepped in
  EXPECT_EQ(traces[1]->num("body.stackFrames.1.line"), 2);
  // Ids are numbered from 1 and never reused: the first stop spent id 1.
  EXPECT_EQ(traces[0]->num("body.stackFrames.0.id"), 1);
  EXPECT_EQ(traces[1]->num("body.stackFrames.0.id"), 2);
  EXPECT_EQ(traces[1]->num("body.stackFrames.1.id"), 3);
  EXPECT_EQ(traces[1]->num("body.totalFrames"), 2);
  EXPECT_EQ(traces[2]->size("body.stackFrames"), 1u);  // levels: 1
  EXPECT_EQ(traces[2]->num("body.totalFrames"), 2);
  EXPECT_EQ(traces[2]->num("body.stackFrames.0.line"), 11);  // stepped over
  EXPECT_EQ(traces[3]->num("body.stackFrames.0.line"), 3);   // stepped out
  EXPECT_EQ(traces[3]->num("body.totalFrames"), 1);

  // Locals and scopes: names and values, then the caller's own after stepping in.
  std::vector<const dap::Msg *> variables;
  for (const dap::Msg & m : all) {
    if (m.is_response() && m.command() == "variables") {
      variables.push_back(&m);
    }
  }
  ASSERT_EQ(variables.size(), 5u);
  EXPECT_EQ(variables[0]->str("body.variables.0.name"), "x");
  EXPECT_EQ(variables[0]->str("body.variables.0.value"), "3");
  EXPECT_EQ(variables[0]->str("body.variables.1.name"), "y");
  EXPECT_EQ(variables[1]->str("body.variables.0.name"), "g");  // Globals
  EXPECT_EQ(variables[2]->str("body.variables.0.name"), "r");  // in f
  EXPECT_EQ(variables[3]->str("body.variables.0.name"), "x");  // the caller's
  EXPECT_EQ(variables[3]->str("body.variables.0.value"), "3");
  EXPECT_FALSE(variables[4]->success());  // the unknown reference
  const dap::Msg * scopes = dap::response_to(all, "scopes");
  ASSERT_NE(scopes, nullptr);
  EXPECT_EQ(scopes->str("body.scopes.0.name"), "Locals");
  EXPECT_EQ(scopes->str("body.scopes.1.name"), "Globals");

  // evaluate: a name works; an expression is refused with the reason.
  std::vector<const dap::Msg *> evaluates;
  for (const dap::Msg & m : all) {
    if (m.is_response() && m.command() == "evaluate") {
      evaluates.push_back(&m);
    }
  }
  ASSERT_EQ(evaluates.size(), 2u);
  EXPECT_TRUE(evaluates[0]->success());
  EXPECT_EQ(evaluates[0]->str("body.result"), "3");
  EXPECT_FALSE(evaluates[1]->success());
  EXPECT_EQ(evaluates[1]->message(), "evaluation of expressions is not supported");
  EXPECT_FALSE(dap::response_to(all, "disconnect") == nullptr);
}

#ifndef _WIN32

TEST(DapSession, OverASocketPairItIsTheSameTranscript) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  dap::Script script = session_script();
  std::vector<std::string> received;
  std::thread client([&] { received = lockstep_client(fds[1], script); close(fds[1]); });

  GRDBG_Transport * transport;
  ASSERT_EQ(grdbg_transport_create_fd(fds[0], fds[0], nullptr, &transport), GRDBG_OK);
  Served served = serve_on(*transport);
  grdbg_transport_destroy(transport);
  close(fds[0]);  // the client reads to end of stream
  client.join();

  expect_golden(transcript_of(received));
  EXPECT_TRUE(served.host.finished);
  EXPECT_EQ(served.output, unattended_output());
}

TEST(DapSession, OverLoopbackTcpTheTestBindsAndAcceptsItIsTheSameTranscript) {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listener, 0);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof address), 0);
  ASSERT_EQ(listen(listener, 1), 0);
  socklen_t length = sizeof address;
  ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length), 0);

  dap::Script script = session_script();
  std::vector<std::string> received;
  std::thread client([&] {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0) {
      received = lockstep_client(fd, script);
    }
    close(fd);
  });
  int connection = accept(listener, nullptr, nullptr);
  ASSERT_GE(connection, 0);

  GRDBG_Transport * transport;
  ASSERT_EQ(grdbg_transport_create_fd(connection, connection, nullptr, &transport), GRDBG_OK);
  Served served = serve_on(*transport);
  grdbg_transport_destroy(transport);
  close(connection);
  client.join();
  close(listener);

  expect_golden(transcript_of(received));
  EXPECT_TRUE(served.host.finished);
  EXPECT_EQ(served.output, unattended_output());
}

#else  // _WIN32

TEST(DapSession, OverASocketPairItIsTheSameTranscript) {
  GTEST_SKIP() << "descriptor transports do not exist on Windows (create_fd is a stub)";
}

TEST(DapSession, OverLoopbackTcpTheTestBindsAndAcceptsItIsTheSameTranscript) {
  GTEST_SKIP() << "descriptor transports do not exist on Windows (create_fd is a stub)";
}

#endif  // _WIN32

TEST(DapSession, ABrokenTransportEndsTheSessionAndTheDebuggedProgramRunsFree) {
  // The client goes away at the first stop (end of input after the first
  // requests): serve says detach, the debugger is disarmed, and the program
  // finishes with the output of an unattended run.
  dap::Script script;
  script.request("initialize", "{}").request("setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[{"line":2},{"line":11}]})").request("configurationDone");
  GRDBG_Transport * memory;
  ASSERT_EQ(grdbg_transport_create_memory(script.bytes().data(), script.bytes().size(), nullptr, &memory), GRDBG_OK);
  Served served = serve_on(*memory);
  grdbg_transport_destroy(memory);
  EXPECT_TRUE(served.host.finished);
  EXPECT_EQ(served.host.pauses, 1);
  EXPECT_EQ(served.output, unattended_output());
}

GRDBG_TEST_MAIN()
