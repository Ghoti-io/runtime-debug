/**
 * @file
 *
 * The transports this library makes: a memory buffer and a pair of POSIX file
 * descriptors.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <thread>

TEST(MemoryTransport, ReadsItsInputInPiecesThenEndsAndCollectsWhatIsWritten) {
  GRDBG_Transport * t;
  const char input[] = "hello world";
  ASSERT_EQ(grdbg_transport_create_memory(input, 11, nullptr, &t), GRDBG_OK);
  char buffer[8];
  size_t n = 99;
  ASSERT_EQ(t->read(t->user, buffer, 8, &n), GRDBG_OK);
  EXPECT_EQ(std::string(buffer, n), "hello wo");
  ASSERT_EQ(t->read(t->user, buffer, 8, &n), GRDBG_OK);
  EXPECT_EQ(std::string(buffer, n), "rld");
  ASSERT_EQ(t->read(t->user, buffer, 8, &n), GRDBG_OK);
  EXPECT_EQ(n, 0u);  // end of input
  EXPECT_EQ(t->close, nullptr);

  ASSERT_EQ(t->write(t->user, "ab", 2), GRDBG_OK);
  ASSERT_EQ(t->write(t->user, "cde", 3), GRDBG_OK);
  ASSERT_EQ(t->write(t->user, "", 0), GRDBG_OK);
  const uint8_t * out = nullptr;
  size_t length = 0;
  ASSERT_EQ(grdbg_transport_memory_output(t, &out, &length), GRDBG_OK);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(out), length), "abcde");
  grdbg_transport_destroy(t);
}

TEST(MemoryTransport, InputIsACopyAndEmptyInputIsAllowed) {
  std::string input = "abc";
  GRDBG_Transport * t;
  ASSERT_EQ(grdbg_transport_create_memory(input.data(), input.size(), nullptr, &t), GRDBG_OK);
  input = "xyz";
  char buffer[4];
  size_t n;
  ASSERT_EQ(t->read(t->user, buffer, 4, &n), GRDBG_OK);
  EXPECT_EQ(std::string(buffer, n), "abc");
  grdbg_transport_destroy(t);
  GRDBG_Transport * empty;
  ASSERT_EQ(grdbg_transport_create_memory(nullptr, 0, nullptr, &empty), GRDBG_OK);
  ASSERT_EQ(empty->read(empty->user, buffer, 4, &n), GRDBG_OK);
  EXPECT_EQ(n, 0u);
  grdbg_transport_destroy(empty);
}

TEST(MemoryTransport, ArgumentsAreCheckedAndOutputIsOnlyForMemoryTransports) {
  GRDBG_Transport * t = reinterpret_cast<GRDBG_Transport *>(0x1);
  EXPECT_EQ(grdbg_transport_create_memory(nullptr, 3, nullptr, &t), GRDBG_ERR_INVALID);
  EXPECT_EQ(t, reinterpret_cast<GRDBG_Transport *>(0x1));
  EXPECT_EQ(grdbg_transport_create_memory("x", 1, nullptr, nullptr), GRDBG_ERR_INVALID);
  const uint8_t * out;
  size_t length;
  EXPECT_EQ(grdbg_transport_memory_output(nullptr, &out, &length), GRDBG_ERR_INVALID);
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  GRDBG_Transport * fd;
  ASSERT_EQ(grdbg_transport_create_fd(fds[0], fds[1], nullptr, &fd), GRDBG_OK);
  EXPECT_EQ(grdbg_transport_memory_output(fd, &out, &length), GRDBG_ERR_INVALID);
  grdbg_transport_destroy(fd);
  close(fds[0]);
  close(fds[1]);
  grdbg_transport_destroy(nullptr);
}

TEST(MemoryTransport, AllocationFailureIsOomAndLeavesNothing) {
  for (long n = 1; n <= 3; ++n) {
    TrackingAllocator a;
    a.fail_at = n;
    GRDBG_Transport * t = reinterpret_cast<GRDBG_Transport *>(0x1);
    GRDBG_Result r = grdbg_transport_create_memory("abc", 3, a.get(), &t);
    if (r == GRDBG_OK) {
      grdbg_transport_destroy(t);
    }
    else {
      EXPECT_EQ(r, GRDBG_ERR_OOM);
      EXPECT_EQ(t, reinterpret_cast<GRDBG_Transport *>(0x1));
    }
    EXPECT_EQ(a.live, 0);
  }
}

TEST(FdTransport, MovesBytesBothWaysOverAPipePairAndLeavesTheDescriptorsOpen) {
  int to_lib[2], from_lib[2];
  ASSERT_EQ(pipe(to_lib), 0);
  ASSERT_EQ(pipe(from_lib), 0);
  GRDBG_Transport * t;
  ASSERT_EQ(grdbg_transport_create_fd(to_lib[0], from_lib[1], nullptr, &t), GRDBG_OK);
  ASSERT_EQ(write(to_lib[1], "request", 7), 7);
  char buffer[16];
  size_t n = 0;
  ASSERT_EQ(t->read(t->user, buffer, sizeof buffer, &n), GRDBG_OK);
  EXPECT_EQ(std::string(buffer, n), "request");
  ASSERT_EQ(t->write(t->user, "response", 8), GRDBG_OK);
  ASSERT_EQ(read(from_lib[0], buffer, sizeof buffer), 8);
  EXPECT_EQ(std::string(buffer, 8), "response");
  // End of input is a read of zero.
  close(to_lib[1]);
  ASSERT_EQ(t->read(t->user, buffer, sizeof buffer, &n), GRDBG_OK);
  EXPECT_EQ(n, 0u);
  grdbg_transport_destroy(t);
  // The descriptors are the host's and still open.
  EXPECT_EQ(write(from_lib[1], "!", 1), 1);
  close(to_lib[0]);
  close(from_lib[0]);
  close(from_lib[1]);
}

TEST(FdTransport, AWriteLargerThanThePipeIsContinuedUntilItIsAllThere) {
  int p[2];
  ASSERT_EQ(pipe(p), 0);
  int unused[2];
  ASSERT_EQ(pipe(unused), 0);
  GRDBG_Transport * t;
  ASSERT_EQ(grdbg_transport_create_fd(unused[0], p[1], nullptr, &t), GRDBG_OK);
  std::string big(1 << 20, 'q');  // far past a pipe's buffer: the kernel takes it short
  std::string got;
  std::thread reader([&] {
    char buffer[4096];
    while (got.size() < big.size()) {
      ssize_t n = read(p[0], buffer, sizeof buffer);
      if (n <= 0) {
        break;
      }
      got.append(buffer, static_cast<size_t>(n));
    }
  });
  EXPECT_EQ(t->write(t->user, big.data(), big.size()), GRDBG_OK);
  reader.join();
  EXPECT_EQ(got, big);
  grdbg_transport_destroy(t);
  close(p[0]);
  close(p[1]);
  close(unused[0]);
  close(unused[1]);
}

TEST(FdTransport, AWriteToASocketWhosePeerLeftIsIoNotASignal) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  GRDBG_Transport * t;
  ASSERT_EQ(grdbg_transport_create_fd(fds[0], fds[0], nullptr, &t), GRDBG_OK);
  close(fds[1]);
  // Without MSG_NOSIGNAL this would kill the test process with SIGPIPE.
  EXPECT_EQ(t->write(t->user, "gone", 4), GRDBG_ERR_IO);
  size_t n = 5;
  EXPECT_EQ(t->read(t->user, &n, 0, &n), GRDBG_OK);  // a zero-byte read of a closed peer is still a read
  grdbg_transport_destroy(t);
  close(fds[0]);
}

TEST(FdTransport, AReadFromABadDescriptorIsIo) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  GRDBG_Transport * t;
  ASSERT_EQ(grdbg_transport_create_fd(fds[0], fds[1], nullptr, &t), GRDBG_OK);
  close(fds[0]);
  char c;
  size_t n;
  EXPECT_EQ(t->read(t->user, &c, 1, &n), GRDBG_ERR_IO);
  grdbg_transport_destroy(t);
  close(fds[1]);
}

TEST(FdTransport, ArgumentsAreChecked) {
  GRDBG_Transport * t = reinterpret_cast<GRDBG_Transport *>(0x1);
  EXPECT_EQ(grdbg_transport_create_fd(-1, 1, nullptr, &t), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_transport_create_fd(0, -1, nullptr, &t), GRDBG_ERR_INVALID);
  EXPECT_EQ(grdbg_transport_create_fd(0, 1, nullptr, nullptr), GRDBG_ERR_INVALID);
  EXPECT_EQ(t, reinterpret_cast<GRDBG_Transport *>(0x1));
  TrackingAllocator a;
  a.fail_at = 1;
  EXPECT_EQ(grdbg_transport_create_fd(0, 1, a.get(), &t), GRDBG_ERR_OOM);
  EXPECT_EQ(a.live, 0);
}

GRDBG_TEST_MAIN()
