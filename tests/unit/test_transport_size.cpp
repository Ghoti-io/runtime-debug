/**
 * @file
 *
 * A transport states its own size (transport.h), so the struct can grow at the
 * end. No member has been added after `close` yet, so there is no older layout
 * to copy; what is tested is the rule that holds the door open: the
 * initialiser writes the size, the transports the library creates are written
 * at their full size, a size below the struct or off its alignment (zero is a
 * struct filled by assignment that forgot its size) is refused and its
 * callbacks are never called, and a transport from a newer header is accepted
 * with its unknown tail ignored. Each refusal has a full-size control, so a
 * session that refuses every transport cannot pass.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "dap_helpers.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {

struct Calls {
  int reads = 0;
  int writes = 0;
  int closes = 0;
};

GRDBG_Transport counting_transport(Calls * calls) {
  GRDBG_Transport t = GRDBG_TRANSPORT_INIT(calls,
      [](void * u, void *, size_t, size_t * out) -> GRDBG_Result {
        static_cast<Calls *>(u)->reads++;
        *out = 0;
        return GRDBG_OK;
      },
      [](void * u, const void *, size_t) -> GRDBG_Result {
        static_cast<Calls *>(u)->writes++;
        return GRDBG_OK;
      },
      [](void * u) -> GRDBG_Result {
        static_cast<Calls *>(u)->closes++;
        return GRDBG_OK;
      });
  return t;
}

} // namespace

TEST(TransportSize, TheInitialiserWritesTheSizeOfTheStructItWasCompiledAgainst) {
  static const GRDBG_Transport t =
      GRDBG_TRANSPORT_INIT(nullptr, nullptr, nullptr, nullptr);
  EXPECT_EQ(t.size, sizeof(GRDBG_Transport));
  EXPECT_GE(t.size, GRDBG_TRANSPORT_MIN_SIZE);
  EXPECT_TRUE(grdbg_transport_valid(&t));
  EXPECT_FALSE(grdbg_transport_valid(nullptr));
}

TEST(TransportSize, TheTransportsTheLibraryCreatesAreWrittenAtTheirFullSize) {
  GRDBG_Transport * memory = nullptr;
  ASSERT_EQ(grdbg_transport_create_memory(nullptr, 0, nullptr, &memory), GRDBG_OK);
  EXPECT_EQ(memory->size, sizeof(GRDBG_Transport));
  EXPECT_TRUE(grdbg_transport_valid(memory));
  const uint8_t * data = nullptr;
  size_t length = 99;
  EXPECT_EQ(grdbg_transport_memory_output(memory, &data, &length), GRDBG_OK);
  grdbg_transport_destroy(memory);
#ifndef _WIN32
  GRDBG_Transport * fd = nullptr;
  ASSERT_EQ(grdbg_transport_create_fd(0, 1, nullptr, &fd), GRDBG_OK);
  EXPECT_EQ(fd->size, sizeof(GRDBG_Transport));
  EXPECT_TRUE(grdbg_transport_valid(fd));
  grdbg_transport_destroy(fd);
#endif
}

TEST(TransportSize, ASizeBelowTheMinimumOrOffAlignmentIsRefusedAndNothingIsCalled) {
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  const size_t bad[] = {0, 1, GRDBG_TRANSPORT_MIN_SIZE - sizeof(void *),
      GRDBG_TRANSPORT_MIN_SIZE - 1, GRDBG_TRANSPORT_MIN_SIZE + 1,
      sizeof(GRDBG_Transport) + 1};
  for (size_t size : bad) {
    Calls calls;
    GRDBG_Transport t = counting_transport(&calls);
    t.size = size;
    EXPECT_FALSE(grdbg_transport_valid(&t)) << "size " << size;
    GRDBG_Dap * session = reinterpret_cast<GRDBG_Dap *>(0x1);
    EXPECT_EQ(grdbg_dap_create(w.dbg, &t, nullptr, &session), GRDBG_ERR_INVALID)
        << "size " << size;
    EXPECT_EQ(session, reinterpret_cast<GRDBG_Dap *>(0x1)) << "written only on success";
    EXPECT_EQ(calls.closes, 0);
  }
  // A memory transport that lost its size is not "a memory transport made here".
  GRDBG_Transport * memory = nullptr;
  ASSERT_EQ(grdbg_transport_create_memory(nullptr, 0, nullptr, &memory), GRDBG_OK);
  const uint8_t * data = nullptr;
  size_t length = 0;
  GRDBG_Transport forgot = *memory;
  forgot.size = 0;
  EXPECT_EQ(grdbg_transport_memory_output(&forgot, &data, &length), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_transport_memory_output(memory, &data, &length), GRDBG_OK);
  grdbg_transport_destroy(memory);
  // Controls: the smallest size and the full size are accepted, so the list
  // above is refused for its size and not for anything else about it.
  for (size_t size : {GRDBG_TRANSPORT_MIN_SIZE, sizeof(GRDBG_Transport)}) {
    Calls calls;
    GRDBG_Transport t = counting_transport(&calls);
    t.size = size;
    GRDBG_Dap * session = nullptr;
    ASSERT_EQ(grdbg_dap_create(w.dbg, &t, nullptr, &session), GRDBG_OK)
        << "size " << size;
    grdbg_dap_destroy(session);
    EXPECT_EQ(calls.closes, 1) << "the copy's close ran";
  }
}

TEST(TransportSize, ATransportFromANewerHeaderIsAcceptedAndItsUnknownTailIgnored) {
  // Decision: accepted, as for a key: a member this library does not know is
  // one whose absence is a defined degradation.
  ToyWorld w(basic_program());
  ASSERT_EQ(w.attach(), GRDBG_OK);
  const size_t extra = 2 * sizeof(void *);
  auto * block = static_cast<unsigned char *>(
      std::malloc(sizeof(GRDBG_Transport) + extra));
  Calls calls;
  GRDBG_Transport proto = counting_transport(&calls);
  std::memcpy(block, &proto, sizeof proto);
  std::memset(block + sizeof(GRDBG_Transport), 0xA5, extra);
  auto * t = reinterpret_cast<GRDBG_Transport *>(block);
  t->size = sizeof(GRDBG_Transport) + extra;
  EXPECT_TRUE(grdbg_transport_valid(t));
  GRDBG_Dap * session = nullptr;
  ASSERT_EQ(grdbg_dap_create(w.dbg, t, nullptr, &session), GRDBG_OK);
  std::free(block); // the session holds a copy, so the host's struct may go
  grdbg_dap_destroy(session);
  EXPECT_EQ(calls.closes, 1) << "the members it knows are used";
}

GRDBG_TEST_MAIN()
