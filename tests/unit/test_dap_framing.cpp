/**
 * @file
 *
 * The protocol adapter's framing, limits and handling of bad input: what is
 * read, what is refused, what ends a session and what only draws an error
 * response.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "dap_helpers.h"

namespace {

/// A world with a debugger and a session on a Wire, ready to serve `input`.
struct Fixture {
  ToyWorld w;
  dap::Wire wire;
  GRDBG_Dap * session = nullptr;

  explicit Fixture(const std::string & input, size_t chunk = 4096, toy::Program program = basic_program(),
      const GRDBG_Limits * limits = nullptr, const GRDBG_Limits * debugger_limits = nullptr)
      : w(std::move(program)) {
    wire.input = input;
    wire.chunk = chunk;
    EXPECT_EQ(w.attach(debugger_limits), GRDBG_OK);
    GRDBG_Transport t = wire.transport();
    EXPECT_EQ(grdbg_dap_create(w.dbg, &t, limits, &session), GRDBG_OK);
  }
  ~Fixture() { grdbg_dap_destroy(session); }

  /// Stops the program at `line` of a.toy first, as a client that attached
  /// at a breakpoint would find it.
  void stop_at(int line) {
    uint64_t id;
    ASSERT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", &line, 1, &id), GRDBG_OK);
    w.run_to_pause();
  }
  GRDBG_Result serve(GRDBG_ServeResult * out) { return grdbg_dap_serve(session, out); }
  /// Serves until something asks the host to proceed, or fails.
  GRDBG_Result serve_once() {
    GRDBG_ServeResult r;
    return serve(&r);
  }
  std::vector<dap::Msg> out() { return dap::messages(wire.output); }
};

std::string request(int64_t seq, const std::string & command, const std::string & arguments = "") {
  std::string body = "{\"seq\":" + std::to_string(seq) + ",\"type\":\"request\",\"command\":\"" + command + "\"";
  if (!arguments.empty()) {
    body += ",\"arguments\":" + arguments;
  }
  return dap::frame(body + "}");
}

}  // namespace

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

TEST(Framing, ARequestIsAnsweredWithItsSeqAndCommand) {
  Fixture f(request(7, "threads") + request(8, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_EQ(r, GRDBG_SERVE_DETACH);
  std::vector<dap::Msg> all = f.out();
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].num("request_seq"), 7);
  EXPECT_EQ(all[0].command(), "threads");
  EXPECT_TRUE(all[0].success());
  EXPECT_EQ(all[0].str("body.threads.0.name"), "main");
  EXPECT_EQ(all[0].num("body.threads.0.id"), 1);
  EXPECT_EQ(all[1].num("request_seq"), 8);
}

TEST(Framing, TheHeaderNameIsCaseInsensitiveAndOtherHeadersAreIgnored) {
  std::string body = R"({"seq":1,"type":"request","command":"threads"})";
  std::string message = "content-LENGTH:  " + std::to_string(body.size()) +
      "  \r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n" + body;
  Fixture f(message + request(2, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_NE(dap::response_to(f.out(), "threads"), nullptr);
}

TEST(Framing, TornReadsAndTornMessagesGiveTheSameAnswers) {
  std::string input = request(1, "initialize", R"({"linesStartAt1":true})") + request(2, "threads") +
      request(3, "disconnect");
  Fixture whole(input);
  GRDBG_ServeResult r;
  ASSERT_EQ(whole.serve(&r), GRDBG_OK);
  for (size_t chunk : {1u, 2u, 3u, 7u, 64u}) {
    Fixture torn(input, chunk);
    ASSERT_EQ(torn.serve(&r), GRDBG_OK) << "chunk " << chunk;
    EXPECT_EQ(torn.wire.output, whole.wire.output) << "chunk " << chunk;
    EXPECT_GT(torn.wire.reads, whole.wire.reads - 1);
  }
}

TEST(Framing, ASingleReadMayHoldSeveralMessages) {
  Fixture f(request(1, "threads") + request(2, "threads") + request(3, "threads") + request(4, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_EQ(f.out().size(), 4u);
}

TEST(Framing, AMissingContentLengthEndsTheSessionWithFormat) {
  Fixture f("Content-Type: x\r\n\r\n{}");
  GRDBG_ServeResult r = GRDBG_SERVE_RESUME;
  EXPECT_EQ(f.serve(&r), GRDBG_ERR_FORMAT);
  EXPECT_EQ(r, GRDBG_SERVE_RESUME);  // untouched on failure
  // The stream cannot be resynchronised: the session stays over.
  EXPECT_EQ(f.serve(&r), GRDBG_ERR_FORMAT);
}

TEST(Framing, ABadContentLengthIsFormatWhateverIsWrongWithIt) {
  const char * bad[] = {
      "Content-Length: abc\r\n\r\n", "Content-Length: -5\r\n\r\n", "Content-Length: +5\r\n\r\n",
      "Content-Length: 12x\r\n\r\n", "Content-Length: 1 2\r\n\r\n", "Content-Length:\r\n\r\n",
      "Content-Length: 0x10\r\n\r\n", "Content-Length: 5\r\nContent-Length: 5\r\n\r\n",
      "Content-Length 5\r\n\r\n", "\r\n\r\n", "Content-Length: 1.5\r\n\r\n"};
  for (const char * header : bad) {
    Fixture f(std::string(header) + "{}{}{}{}{}");
    GRDBG_ServeResult r;
    EXPECT_EQ(f.serve(&r), GRDBG_ERR_FORMAT) << header;
    EXPECT_EQ(f.wire.output, "") << header;  // nothing was answered
  }
}

TEST(Framing, AHeaderBlockPastTheLimitIsLimitAndNothingBeyondItIsRead) {
  // 1024 bytes of header with no end, then more. The adapter may read what the
  // allowance covers and no further.
  std::string endless(1024 + 4000, 'A');
  Fixture f("Content-Length: 5\r\nX: " + endless);
  GRDBG_ServeResult r;
  EXPECT_EQ(f.serve(&r), GRDBG_ERR_LIMIT);
  EXPECT_LE(f.wire.total_read, 1024u);
}

TEST(Framing, ATerminatorJustPastTheHeaderLimitIsLimitToo) {
  std::string padding(1100, 'z');
  Fixture f("Content-Length: 2\r\nX: " + padding + "\r\n\r\n{}", 4096);
  GRDBG_ServeResult r;
  EXPECT_EQ(f.serve(&r), GRDBG_ERR_LIMIT);
}

TEST(Framing, AHeaderExactlyAtTheLimitIsAccepted) {
  std::string head = "Content-Length: 2\r\nX: ";
  std::string tail = "\r\n\r\n";
  std::string filler(1024 - head.size() - tail.size(), 'z');
  Fixture f(head + filler + tail + "{}" + request(1, "disconnect"));
  GRDBG_ServeResult r;
  EXPECT_EQ(f.serve(&r), GRDBG_OK);
}

TEST(Framing, ALengthOverTheLimitIsLimitBeforeTheBodyIsRead) {
  Fixture f("Content-Length: 4194305\r\n\r\n" + std::string(100, 'x'));
  GRDBG_ServeResult r;
  EXPECT_EQ(f.serve(&r), GRDBG_ERR_LIMIT);
  EXPECT_LE(f.wire.total_read, 1024u);
  Fixture huge("Content-Length: 99999999999999999999999999\r\n\r\n");
  EXPECT_EQ(huge.serve(&r), GRDBG_ERR_LIMIT);
}

TEST(Framing, ALengthAtTheLimitIsAcceptedWhenTheBodyIsThere) {
  GRDBG_Limits limits;
  grdbg_limits_default(&limits);
  limits.max_message_bytes = 64;
  std::string body = R"({"seq":1,"type":"request","command":"threads"})";
  ASSERT_LE(body.size(), 64u);
  std::string padded = body + std::string(64 - body.size(), ' ');
  Fixture f(dap::frame(padded) + request(2, "disconnect"), 4096, basic_program(), &limits);
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_NE(dap::response_to(f.out(), "threads"), nullptr);
  Fixture over(dap::frame(padded + " "), 4096, basic_program(), &limits);
  EXPECT_EQ(over.serve(&r), GRDBG_ERR_LIMIT);
}

TEST(Framing, EndOfInputBeforeAnyMessageDetachesAndDisarms) {
  Fixture f("");
  int lines[] = {2};
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(f.w.dbg, "a.toy", lines, 1, &id), GRDBG_OK);
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_EQ(r, GRDBG_SERVE_DETACH);
  EXPECT_EQ(grdbg_debugger_breakpoint_count(f.w.dbg), 0u);
  EXPECT_FALSE(grdbg_debugger_armed(f.w.dbg));
  // And it stays detached.
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_EQ(r, GRDBG_SERVE_DETACH);
}

TEST(Framing, EndOfInputInTheMiddleOfAMessageDetaches) {
  std::string whole = request(1, "threads");
  for (size_t cut : {size_t(5), size_t(18), whole.size() - 3}) {
    Fixture f(whole.substr(0, cut));
    GRDBG_ServeResult r;
    ASSERT_EQ(f.serve(&r), GRDBG_OK) << cut;
    EXPECT_EQ(r, GRDBG_SERVE_DETACH) << cut;
    EXPECT_EQ(f.wire.output, "") << cut;
  }
}

TEST(Framing, ATransportThatFailsToReadIsIoAndEndsTheSession) {
  Fixture f("");
  f.wire.fail_read = true;
  GRDBG_ServeResult r;
  EXPECT_EQ(f.serve(&r), GRDBG_ERR_IO);
  EXPECT_EQ(f.serve(&r), GRDBG_ERR_IO);
}

TEST(Framing, ATransportThatFailsToWriteIsIoAndEndsTheSessionAndDisarms) {
  Fixture f(request(1, "threads"));
  int lines[] = {2};
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(f.w.dbg, "a.toy", lines, 1, &id), GRDBG_OK);
  f.wire.fail_write = true;
  GRDBG_ServeResult r;
  EXPECT_EQ(f.serve(&r), GRDBG_ERR_IO);
  EXPECT_EQ(f.serve(&r), GRDBG_ERR_IO);  // the session is over
  EXPECT_EQ(grdbg_debugger_breakpoint_count(f.w.dbg), 0u);
  EXPECT_EQ(grdbg_dap_notify_finished(f.session, 0), GRDBG_ERR_IO);
}

TEST(Framing, NotifyReportsAWriteFailureAsIo) {
  Fixture f("");
  f.stop_at(2);
  f.wire.fail_write = true;
  EXPECT_EQ(grdbg_dap_notify_stopped(f.session), GRDBG_ERR_IO);
  f.wire.fail_write = false;
  EXPECT_EQ(grdbg_dap_notify_stopped(f.session), GRDBG_ERR_IO);  // and it stays over
}

TEST(Framing, TheTransportIsClosedOnceByDestroy) {
  dap::Wire wire;
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  GRDBG_Transport t = wire.transport();
  GRDBG_Dap * session;
  ASSERT_EQ(grdbg_dap_create(w.dbg, &t, nullptr, &session), GRDBG_OK);
  EXPECT_FALSE(wire.closed);
  grdbg_dap_destroy(session);
  EXPECT_TRUE(wire.closed);
  grdbg_dap_destroy(nullptr);
}

TEST(Framing, ACreateNeedsAReadAndAWrite) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  GRDBG_Dap * session = reinterpret_cast<GRDBG_Dap *>(0x1);
  GRDBG_Transport none = {};
  EXPECT_EQ(grdbg_dap_create(w.dbg, &none, nullptr, &session), GRDBG_ERR_INVALID);
  EXPECT_EQ(session, reinterpret_cast<GRDBG_Dap *>(0x1));
  dap::Wire wire;
  GRDBG_Transport t = wire.transport();
  EXPECT_EQ(grdbg_dap_create(nullptr, &t, nullptr, &session), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_dap_create(w.dbg, nullptr, nullptr, &session), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_dap_create(w.dbg, &t, nullptr, nullptr), GRDBG_ERR_INVALID);
}

TEST(Framing, ANonOwnerThreadIsRefused) {
  Fixture f(request(1, "threads"));
  GRDBG_Result r = GRDBG_OK;
  std::thread([&] {
    GRDBG_ServeResult s;
    r = f.serve(&s);
  }).join();
  EXPECT_EQ(r, GRDBG_ERR_INVALID);
  EXPECT_EQ(f.wire.output, "");
}

// ---------------------------------------------------------------------------
// Well-framed, but not a request
// ---------------------------------------------------------------------------

TEST(NotARequest, BodiesThatAreNotJsonAreDroppedAndTheSessionContinues) {
  Fixture f(dap::frame("this is not json") + dap::frame("") + dap::frame("{\"seq\":") + request(2, "threads") +
      request(3, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  ASSERT_EQ(all.size(), 2u);  // threads and disconnect; the rest drew nothing
  EXPECT_EQ(all[0].command(), "threads");
}

TEST(NotARequest, JsonThatIsNotAnObjectOrHasNoSeqIsDropped) {
  Fixture f(dap::frame("[1,2,3]") + dap::frame("42") + dap::frame("\"threads\"") + dap::frame("null") +
      dap::frame(R"({"type":"request","command":"threads"})") +
      dap::frame(R"({"seq":"one","type":"request","command":"threads"})") + request(2, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_EQ(f.out().size(), 1u);  // only the disconnect
}

TEST(NotARequest, ASeqWithTheWrongTypeGetsAnErrorResponse) {
  Fixture f(dap::frame(R"({"seq":4,"type":"response","command":"threads"})") +
      dap::frame(R"({"seq":5,"command":"threads"})") + request(6, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  ASSERT_EQ(all.size(), 3u);
  for (int i = 0; i < 2; ++i) {
    EXPECT_FALSE(all[static_cast<size_t>(i)].success());
    EXPECT_EQ(all[static_cast<size_t>(i)].num("request_seq"), 4 + i);
    EXPECT_EQ(all[static_cast<size_t>(i)].message(), "not a request");
  }
}

TEST(NotARequest, ARequestWithNoCommandGetsAnErrorResponseNamingNothing) {
  Fixture f(dap::frame(R"({"seq":4,"type":"request"})") + dap::frame(R"({"seq":5,"type":"request","command":7})") +
      request(6, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  ASSERT_EQ(all.size(), 3u);
  EXPECT_FALSE(all[0].success());
  EXPECT_EQ(all[0].command(), "");
  EXPECT_EQ(all[0].message(), "the request has no command");
  EXPECT_FALSE(all[1].success());
}

TEST(NotARequest, AnUnknownCommandIsUnsupportedAndNamesIt) {
  Fixture f(request(1, "nonsense") + request(2, "restart") + request(3, "setVariable") + request(4, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  ASSERT_EQ(all.size(), 4u);
  EXPECT_FALSE(all[0].success());
  EXPECT_EQ(all[0].message(), "unsupported request: nonsense");
  EXPECT_EQ(all[0].command(), "nonsense");
  EXPECT_EQ(all[1].message(), "unsupported request: restart");
  EXPECT_EQ(all[2].message(), "unsupported request: setVariable");
}

TEST(NotARequest, ANestTooDeepForTheJsonLimitIsDroppedAndTheSessionContinues) {
  std::string deep;
  for (int i = 0; i < 70; ++i) {
    deep += "[";
  }
  for (int i = 0; i < 70; ++i) {
    deep += "]";
  }
  std::string body = R"({"seq":1,"type":"request","command":"threads","arguments":{"a":)" + deep + "}}";
  Fixture f(dap::frame(body) + request(2, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  ASSERT_EQ(f.out().size(), 1u);  // the nest drew nothing; disconnect was answered
  // The same message nested 60 deep is within the limit.
  deep.clear();
  for (int i = 0; i < 50; ++i) {
    deep += "[";
  }
  for (int i = 0; i < 50; ++i) {
    deep += "]";
  }
  body = R"({"seq":1,"type":"request","command":"threads","arguments":{"a":)" + deep + "}}";
  Fixture g(dap::frame(body) + request(2, "disconnect"));
  ASSERT_EQ(g.serve(&r), GRDBG_OK);
  EXPECT_EQ(g.out().size(), 2u);
}

TEST(NotARequest, AnEmbeddedNulOrNonStringCommandOrArgumentIsAnErrorNotAFault) {
  Fixture f(dap::frame(std::string(R"({"seq":1,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"a\u0000b"},"breakpoints":[{"line":1}]}})")) +
      dap::frame(R"({"seq":2,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":5},"breakpoints":[{"line":1}]}})") +
      dap::frame(R"({"seq":3,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":"a.toy"},"breakpoints":"all"}})") +
      dap::frame(R"({"seq":4,"type":"request","command":"setBreakpoints","arguments":"nope"})") + request(5, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  ASSERT_EQ(all.size(), 5u);
  for (int i = 0; i < 4; ++i) {
    EXPECT_FALSE(all[static_cast<size_t>(i)].success()) << all[static_cast<size_t>(i)].text();
  }
  EXPECT_EQ(grdbg_debugger_breakpoint_count(f.w.dbg), 0u);
}

// ---------------------------------------------------------------------------
// The surface
// ---------------------------------------------------------------------------

TEST(Surface, InitializeAnswersWithOnlyTheConfigurationDoneCapabilityThenInitialized) {
  Fixture f(request(1, "initialize", R"({"adapterID":"x"})") + request(2, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  ASSERT_GE(all.size(), 3u);
  EXPECT_EQ(all[0].command(), "initialize");
  EXPECT_EQ(all[0].text(),
      R"({"seq":1,"type":"response","request_seq":1,"success":true,"command":"initialize","body":{"supportsConfigurationDoneRequest":true}})");
  EXPECT_EQ(all[1].text(), R"({"seq":2,"type":"event","event":"initialized"})");
}

TEST(Surface, LaunchAttachAndExceptionBreakpointsAreAcceptedAndDoNothing) {
  Fixture f(request(1, "launch", "{}") + request(2, "attach", "{}") + request(3, "setExceptionBreakpoints", R"({"filters":["all"]})") +
      request(4, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_TRUE(all[i].success());
    EXPECT_FALSE(all[i].has("body"));
  }
  EXPECT_EQ(grdbg_debugger_breakpoint_count(f.w.dbg), 0u);
}

TEST(Surface, ConfigurationDoneAsksTheHostToResumeAfterItsResponseIsOut) {
  Fixture f(request(1, "configurationDone"));
  GRDBG_ServeResult r = GRDBG_SERVE_DETACH;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_EQ(r, GRDBG_SERVE_RESUME);
  ASSERT_EQ(f.out().size(), 1u);
  EXPECT_EQ(f.out()[0].command(), "configurationDone");
}

TEST(Surface, SetBreakpointsReportsEveryBreakpointVerifiedWithItsId) {
  Fixture f(request(1, "setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[{"line":2,"condition":"x>1"},{"line":4,"hitCondition":"3","logMessage":"hi"}]})") +
      request(2, "setBreakpoints", R"({"source":{"path":"a.toy"},"lines":[5]})") + request(3, "configurationDone"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  EXPECT_EQ(all[0].size("body.breakpoints"), 2u);
  EXPECT_TRUE(all[0].boolean("body.breakpoints.0.verified"));
  EXPECT_TRUE(all[0].boolean("body.breakpoints.1.verified"));
  EXPECT_EQ(all[0].num("body.breakpoints.0.id"), 1);
  EXPECT_EQ(all[0].num("body.breakpoints.1.id"), 2);
  EXPECT_EQ(all[0].num("body.breakpoints.1.line"), 4);
  EXPECT_EQ(all[1].num("body.breakpoints.0.id"), 3);  // the old lines argument, replacing the source's set
  EXPECT_EQ(grdbg_debugger_breakpoint_count(f.w.dbg), 1u);
  // Conditions, hit counts and log messages are not offered, and the
  // capabilities do not claim them.
  Fixture g(request(1, "initialize") + request(2, "disconnect"));
  ASSERT_EQ(g.serve(&r), GRDBG_OK);
  EXPECT_FALSE(g.out()[0].has("body.supportsConditionalBreakpoints"));
  EXPECT_FALSE(g.out()[0].has("body.supportsHitConditionalBreakpoints"));
  EXPECT_FALSE(g.out()[0].has("body.supportsLogPoints"));
}

TEST(Surface, MoreBreakpointsThanTheLimitIsAnErrorResponseAndNothingChanges) {
  std::string lines;
  for (int i = 1; i <= 1025; ++i) {
    lines += (i > 1 ? "," : "") + std::string("{\"line\":") + std::to_string(i) + "}";
  }
  Fixture f(request(1, "setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[{"line":3}]})") +
      request(2, "setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[)" + lines + "]}") +
      request(3, "configurationDone"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  EXPECT_TRUE(all[0].success());
  EXPECT_FALSE(all[1].success());
  EXPECT_EQ(all[1].message(), "too many breakpoints");
  EXPECT_EQ(grdbg_debugger_breakpoint_count(f.w.dbg), 1u);
  // 1024 is within the limit.
  lines.erase(lines.rfind(",{"));
  Fixture g(request(1, "setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[)" + lines + "]}") + request(2, "configurationDone"));
  ASSERT_EQ(g.serve(&r), GRDBG_OK);
  EXPECT_TRUE(g.out()[0].success());
  EXPECT_EQ(grdbg_debugger_breakpoint_count(g.w.dbg), 1024u);
}

TEST(Surface, ALineThatIsNotAPositiveIntegerIsAnErrorResponse) {
  Fixture f(request(1, "setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[{"line":0}]})") +
      request(2, "setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[{"line":-3}]})") +
      request(3, "setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[{"line":"2"}]})") +
      request(4, "setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[{"column":2}]})") +
      request(5, "setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[{"line":99999999999}]})") +
      request(6, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_FALSE(all[i].success()) << all[i].text();
  }
  EXPECT_EQ(grdbg_debugger_breakpoint_count(f.w.dbg), 0u);
}

TEST(Surface, LinesStartAtZeroAreShiftedAtTheEdgeBothWays) {
  Fixture f(request(1, "initialize", R"({"linesStartAt1":false,"columnsStartAt1":false})") +
      request(2, "setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[{"line":1}]})") +
      request(3, "configurationDone") + request(4, "stackTrace") + request(5, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_EQ(r, GRDBG_SERVE_RESUME);
  std::vector<dap::Msg> all = f.out();
  EXPECT_EQ(all[2].command(), "setBreakpoints");
  EXPECT_EQ(all[2].num("body.breakpoints.0.line"), 1);  // echoed in the client's base
  // The client's line 1 is the model's line 2.
  f.w.run_to_pause();
  EXPECT_EQ(here(f.w.dbg).line, 2);
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  all = f.out();
  EXPECT_EQ(all[4].command(), "stackTrace");
  EXPECT_EQ(all[4].num("body.stackFrames.0.line"), 1);    // and back again
  EXPECT_EQ(all[4].num("body.stackFrames.0.column"), 0);  // columns too
}

TEST(Surface, ALineOfZeroInAOneBasedClientIsRefusedButNotInAZeroBasedOne) {
  Fixture f(request(1, "initialize", R"({"linesStartAt1":false})") +
      request(2, "setBreakpoints", R"({"source":{"path":"a.toy"},"breakpoints":[{"line":0}]})") +
      request(3, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_TRUE(f.out()[2].success());
}

TEST(Surface, ColumnsAreReportedAsOne) {
  Fixture f(request(1, "stackTrace") + request(2, "disconnect"));
  f.stop_at(3);
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_EQ(f.out()[0].num("body.stackFrames.0.column"), 1);
}

TEST(Surface, ThreadsIsOneThreadCalledMain) {
  Fixture f(request(1, "threads") + request(2, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_EQ(f.out()[0].size("body.threads"), 1u);
}

TEST(Surface, EvaluateLooksANameUpInTheFramesScopesInnermostFirst) {
  toy::Program p;
  p.fn("main", "a.toy", {"g"}).set(1, "g", 5).gset(2, 11).nop(3);
  Fixture f(request(1, "evaluate", R"({"expression":"g","frameId":1})") + request(2, "evaluate", R"({"expression":"nothing"})") +
      request(3, "evaluate", R"({"expression":"g","frameId":7})") + request(4, "evaluate", R"({"frameId":1})") +
      request(5, "evaluate", R"({"expression":5,"frameId":1})") + request(6, "disconnect"),
      4096, std::move(p));
  f.stop_at(3);
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  EXPECT_TRUE(all[0].success());
  EXPECT_EQ(all[0].str("body.result"), "5");  // the local g hides the global
  EXPECT_FALSE(all[1].success());
  EXPECT_EQ(all[1].message(), "evaluation of expressions is not supported");
  EXPECT_FALSE(all[2].success());
  EXPECT_EQ(all[2].message(), "unknown frame id");
  EXPECT_FALSE(all[3].success());
  EXPECT_FALSE(all[4].success());
}

TEST(Surface, PauseIsAcceptedWhileStoppedAndRefusedWhileRunning) {
  Fixture before(request(1, "pause", R"({"threadId":1})") + request(2, "disconnect"));
  GRDBG_ServeResult r;
  ASSERT_EQ(before.serve(&r), GRDBG_OK);
  EXPECT_FALSE(before.out()[0].success());
  EXPECT_NE(before.out()[0].message().find("interrupt"), std::string::npos);
  Fixture stopped(request(1, "pause", R"({"threadId":1})") + request(2, "disconnect"));
  stopped.stop_at(2);
  ASSERT_EQ(stopped.serve(&r), GRDBG_OK);
  EXPECT_TRUE(stopped.out()[0].success());
}

TEST(Surface, RequestsThatNeedAStopAreRefusedBeforeOne) {
  const char * commands[] = {"stackTrace", "scopes", "variables", "continue", "next", "stepIn", "stepOut"};
  std::string input;
  int seq = 0;
  for (const char * c : commands) {
    input += request(++seq, c, R"({"frameId":1,"variablesReference":1,"threadId":1})");
  }
  input += request(++seq, "disconnect");
  Fixture f(input);
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  for (size_t i = 0; i < 7; ++i) {
    EXPECT_FALSE(all[i].success()) << commands[i];
    EXPECT_EQ(all[i].message(), "the program is not stopped") << commands[i];
  }
  EXPECT_FALSE(grdbg_debugger_armed(f.w.dbg));
}

TEST(Surface, ATerminateAnswersFirstThenAsksTheHostToTerminate) {
  Fixture f(request(1, "terminate"));
  GRDBG_ServeResult r = GRDBG_SERVE_RESUME;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_EQ(r, GRDBG_SERVE_TERMINATE);
  ASSERT_EQ(f.out().size(), 1u);
  EXPECT_EQ(f.out()[0].command(), "terminate");
  EXPECT_TRUE(f.out()[0].success());
}

TEST(Surface, DisconnectTerminatesTheDebuggeeOnlyWhenAsked) {
  GRDBG_ServeResult r = GRDBG_SERVE_RESUME;
  Fixture yes(request(1, "disconnect", R"({"terminateDebuggee":true})"));
  ASSERT_EQ(yes.serve(&r), GRDBG_OK);
  EXPECT_EQ(r, GRDBG_SERVE_TERMINATE);
  EXPECT_EQ(yes.out().size(), 1u);
  Fixture no(request(1, "disconnect", R"({"terminateDebuggee":false})"));
  int lines[] = {2};
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(no.w.dbg, "a.toy", lines, 1, &id), GRDBG_OK);
  ASSERT_EQ(no.serve(&r), GRDBG_OK);
  EXPECT_EQ(r, GRDBG_SERVE_DETACH);
  EXPECT_EQ(grdbg_debugger_breakpoint_count(no.w.dbg), 0u);  // disarmed: the host runs free
  Fixture bare(request(1, "disconnect"));
  ASSERT_EQ(bare.serve(&r), GRDBG_OK);
  EXPECT_EQ(r, GRDBG_SERVE_DETACH);
  // After a disconnect nothing more is written.
  EXPECT_EQ(grdbg_dap_notify_finished(bare.session, 0), GRDBG_OK);
  EXPECT_EQ(bare.out().size(), 1u);
}

// ---------------------------------------------------------------------------
// Ids, stale and unknown
// ---------------------------------------------------------------------------

TEST(Ids, AnUnknownFrameOrReferenceIsAnErrorResponseNeverARead) {
  Fixture f(request(1, "scopes", R"({"frameId":0})") + request(2, "scopes", R"({"frameId":2})") +
      request(3, "scopes", R"({"frameId":-1})") + request(4, "scopes", R"({"frameId":"1"})") +
      request(5, "scopes", "{}") + request(6, "variables", R"({"variablesReference":1})") +
      request(7, "variables", R"({"variablesReference":0})") + request(8, "variables", R"({"variablesReference":-4})") +
      request(9, "variables", R"({"variablesReference":9223372036854775807})") + request(10, "variables", "{}") +
      request(11, "disconnect"));
  f.stop_at(3);
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  for (size_t i = 0; i < 10; ++i) {
    EXPECT_FALSE(all[i].success()) << all[i].text();
  }
  EXPECT_EQ(all[0].message(), "unknown frame id");
  EXPECT_EQ(all[5].message(), "unknown variablesReference");
}

TEST(Ids, AReferenceFromBeforeAResumeIsForgotten) {
  Fixture f(request(1, "scopes", R"({"frameId":1})") + request(2, "variables", R"({"variablesReference":1})") +
      request(3, "continue") + request(4, "variables", R"({"variablesReference":1})") + request(5, "disconnect"));
  f.stop_at(3);
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  EXPECT_EQ(r, GRDBG_SERVE_RESUME);
  ASSERT_EQ(f.serve(&r), GRDBG_OK);  // the program has not been resumed: still stopped
  std::vector<dap::Msg> all = f.out();
  EXPECT_TRUE(all[1].success());
  EXPECT_TRUE(all[2].success());
  EXPECT_FALSE(all[3].success());
  EXPECT_EQ(all[3].message(), "unknown variablesReference");
}

TEST(Ids, ScopesForTheSameFrameKeepTheirReferenceWithinAStop) {
  Fixture f(request(1, "scopes", R"({"frameId":1})") + request(2, "scopes", R"({"frameId":1})") + request(3, "disconnect"));
  f.stop_at(3);
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  EXPECT_EQ(all[0].num("body.scopes.0.variablesReference"), all[1].num("body.scopes.0.variablesReference"));
  EXPECT_EQ(all[0].num("body.scopes.0.variablesReference"), 1);
}

TEST(Ids, VariablesPageWithStartAndCountAndAreCappedByTheLimit) {
  toy::Program p;
  p.fn("main", "a.toy", {"a", "b", "c", "d", "e"}).nop(1).nop(2);
  GRDBG_Limits small;
  grdbg_limits_default(&small);
  small.max_variables = 3;
  Fixture f(request(1, "scopes", R"({"frameId":1})") + request(2, "variables", R"({"variablesReference":1})") +
      request(3, "variables", R"({"variablesReference":1,"start":3})") +
      request(4, "variables", R"({"variablesReference":1,"start":1,"count":2})") +
      request(5, "variables", R"({"variablesReference":1,"start":99})") + request(6, "disconnect"),
      4096, std::move(p), &small);
  f.stop_at(2);
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  EXPECT_EQ(all[1].size("body.variables"), 3u);  // capped
  EXPECT_EQ(all[1].str("body.variables.2.name"), "c");
  EXPECT_EQ(all[2].size("body.variables"), 2u);  // d and e
  EXPECT_EQ(all[2].str("body.variables.0.name"), "d");
  EXPECT_EQ(all[3].size("body.variables"), 2u);
  EXPECT_EQ(all[3].str("body.variables.0.name"), "b");
  EXPECT_EQ(all[4].size("body.variables"), 0u);
}

TEST(Ids, AStackOfTenThousandFramesReturnsTheCapAndReportsTheTotal) {
  toy::Program p;
  p.fn("main", "a.toy").gset(1, 10000).call(2, "deep");
  p.fn("deep", "d.toy").gadd(5, -1).jg(6, 3).jmp(7, 4).call(8, "deep").nop(9);
  Fixture f(request(1, "stackTrace") + request(2, "stackTrace", R"({"startFrame":998,"levels":5})") +
      request(3, "stackTrace", R"({"startFrame":5000})") + request(4, "disconnect"),
      4096, std::move(p));
  int line = 9;
  uint64_t id;
  ASSERT_EQ(grdbg_debugger_set_breakpoints(f.w.dbg, "d.toy", &line, 1, &id), GRDBG_OK);
  f.w.run_to_pause();
  GRDBG_ServeResult r;
  ASSERT_EQ(f.serve(&r), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  EXPECT_EQ(all[0].size("body.stackFrames"), 1000u);
  EXPECT_EQ(all[0].num("body.totalFrames"), 10001);
  EXPECT_EQ(all[0].num("body.stackFrames.999.id"), 1000);
  EXPECT_EQ(all[1].size("body.stackFrames"), 2u);  // 998 and 999 of the 1000 recorded
  EXPECT_EQ(all[1].num("body.totalFrames"), 10001);
  EXPECT_EQ(all[2].size("body.stackFrames"), 0u);
  EXPECT_EQ(all[2].num("body.totalFrames"), 10001);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

TEST(Events, AStopForABreakpointCarriesItsIds) {
  Fixture f("");
  int lines[] = {2, 2};
  uint64_t ids[2];
  ASSERT_EQ(grdbg_debugger_set_breakpoints(f.w.dbg, "a.toy", lines, 2, ids), GRDBG_OK);
  f.w.run_to_pause();
  ASSERT_EQ(grdbg_dap_notify_stopped(f.session), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].str("body.reason"), "breakpoint");
  EXPECT_EQ(all[0].size("body.hitBreakpointIds"), 2u);
  EXPECT_EQ(all[0].num("body.hitBreakpointIds.0"), static_cast<int64_t>(ids[0]));
  EXPECT_EQ(all[0].num("body.threadId"), 1);
  EXPECT_TRUE(all[0].boolean("body.allThreadsStopped"));
}

TEST(Events, ABudgetPauseIsAPauseThatNamesTheKeyAndContinuingWithoutRaisingItPausesAgain) {
  ToyWorld w(basic_program(), 3);  // fuel for three statements
  ASSERT_EQ(w.attach(), GRDBG_OK);
  dap::Script script;
  script.request("configurationDone").request("continue").request("continue").request("disconnect", R"({"terminateDebuggee":true})");
  GRDBG_Transport * memory;
  ASSERT_EQ(grdbg_transport_create_memory(script.bytes().data(), script.bytes().size(), nullptr, &memory), GRDBG_OK);
  GRDBG_Dap * session;
  ASSERT_EQ(grdbg_dap_create(w.dbg, memory, nullptr, &session), GRDBG_OK);
  dap::HostResult h = dap::drive(w, session);
  // The host's policy: it does not raise the budget, so each `continue` runs one
  // statement and pauses again on the same exhausted fuel.
  EXPECT_EQ(h.pauses, 3);
  const uint8_t * out;
  size_t length;
  ASSERT_EQ(grdbg_transport_memory_output(memory, &out, &length), GRDBG_OK);
  std::vector<dap::Msg> all = dap::messages(std::string(reinterpret_cast<const char *>(out), length));
  std::vector<const dap::Msg *> stops = dap::events(all, "stopped");
  ASSERT_GE(stops.size(), 2u);
  EXPECT_EQ(stops[0]->str("body.reason"), "pause");
  EXPECT_EQ(stops[0]->str("body.description"), "paused by fuel");
  EXPECT_FALSE(stops[0]->has("body.hitBreakpointIds"));
  EXPECT_EQ(stops[1]->str("body.description"), "paused by fuel");
  grdbg_dap_destroy(session);
  grdbg_transport_destroy(memory);
}

TEST(Events, AnInterruptIsAPauseNamingTheInterruptKey) {
  Fixture f("");
  GRCORE_Port * port;
  ASSERT_EQ(grcore_context_port(f.w.ctx, &port), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port, GRCORE_REQUEST_INTERRUPT), GRCORE_OK);
  f.w.run_to_pause();
  grcore_port_release(port);
  ASSERT_EQ(grdbg_dap_notify_stopped(f.session), GRDBG_OK);
  EXPECT_EQ(f.out()[0].str("body.reason"), "pause");
  EXPECT_EQ(f.out()[0].str("body.description"), "paused by interrupt");
}

TEST(Events, TheDebuggersOwnPauseRequestIsAPauseWithoutADescription) {
  Fixture f("");
  ASSERT_EQ(grdbg_debugger_request_pause(f.w.dbg), GRDBG_OK);
  f.w.run_to_pause();
  ASSERT_EQ(grdbg_dap_notify_stopped(f.session), GRDBG_OK);
  EXPECT_EQ(f.out()[0].str("body.reason"), "pause");
  EXPECT_FALSE(f.out()[0].has("body.description"));
}

TEST(Events, NotifyStoppedNeedsAPausedContext) {
  Fixture f("");
  EXPECT_EQ(grdbg_dap_notify_stopped(f.session), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_dap_notify_stopped(nullptr), GRDBG_ERR_INVALID);
  EXPECT_EQ(f.wire.output, "");
}

TEST(Events, FinishedIsExitedThenTerminatedAndContinuedIsNeverSent) {
  Fixture f("");
  ASSERT_EQ(grdbg_dap_notify_finished(f.session, 3), GRDBG_OK);
  std::vector<dap::Msg> all = f.out();
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].event(), "exited");
  EXPECT_EQ(all[0].num("body.exitCode"), 3);
  EXPECT_EQ(all[1].event(), "terminated");
  EXPECT_EQ(grdbg_dap_notify_finished(nullptr, 0), GRDBG_ERR_INVALID);
}

GRDBG_TEST_MAIN()
