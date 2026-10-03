/**
 * @file
 *
 * Versions, result strings, the allocator and the limits.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>

TEST(Core, VersionStringAndNumberAgreeWithTheGeneratedHeader) {
  EXPECT_STREQ(grdbg_version_string(), GRDBG_VERSION_STRING);
  EXPECT_EQ(grdbg_version_number(), GRDBG_VERSION_NUMBER);
  EXPECT_EQ(GRDBG_VERSION_NUMBER,
      GRDBG_MAKE_VERSION(GRDBG_VERSION_MAJOR, GRDBG_VERSION_MINOR, GRDBG_VERSION_PATCH));
  EXPECT_EQ(GRDBG_MAKE_VERSION(1, 2, 3), 0x010203u);
}

TEST(Core, EveryResultHasADistinctNonEmptyString) {
  std::vector<std::string> seen;
  for (int r = 0; r < GRDBG_RESULT_COUNT; ++r) {
    const char * s = grdbg_result_string(static_cast<GRDBG_Result>(r));
    ASSERT_NE(s, nullptr);
    EXPECT_STRNE(s, "");
    EXPECT_STRNE(s, "Unknown error") << "result " << r << " has no string";
    for (const std::string & other : seen) {
      EXPECT_NE(other, s);
    }
    seen.push_back(s);
  }
  EXPECT_STREQ(grdbg_result_string(static_cast<GRDBG_Result>(GRDBG_RESULT_COUNT)), "Unknown error");
  EXPECT_STREQ(grdbg_result_string(static_cast<GRDBG_Result>(-1)), "Unknown error");
}

TEST(Core, TheResultVocabularyIsTheSuitesAndKeepsItsNumbers) {
  // CONVENTIONS.md section 5; the same numbering as runtime-core up to INTERNAL.
  EXPECT_EQ(GRDBG_OK, 0);
  EXPECT_EQ(static_cast<int>(GRDBG_ERR_IO), static_cast<int>(GRCORE_ERR_IO));
  EXPECT_EQ(static_cast<int>(GRDBG_ERR_FORMAT), static_cast<int>(GRCORE_ERR_FORMAT));
  EXPECT_EQ(static_cast<int>(GRDBG_ERR_UNSUPPORTED), static_cast<int>(GRCORE_ERR_UNSUPPORTED));
  EXPECT_EQ(static_cast<int>(GRDBG_ERR_LIMIT), static_cast<int>(GRCORE_ERR_LIMIT));
  EXPECT_EQ(static_cast<int>(GRDBG_ERR_CORRUPT), static_cast<int>(GRCORE_ERR_CORRUPT));
  EXPECT_EQ(static_cast<int>(GRDBG_ERR_OOM), static_cast<int>(GRCORE_ERR_OOM));
  EXPECT_EQ(static_cast<int>(GRDBG_ERR_INVALID), static_cast<int>(GRCORE_ERR_INVALID));
  EXPECT_EQ(static_cast<int>(GRDBG_ERR_INTERNAL), static_cast<int>(GRCORE_ERR_INTERNAL));
  EXPECT_EQ(static_cast<int>(GRDBG_RESULT_COUNT), static_cast<int>(GRDBG_ERR_INTERNAL) + 1);  // no ERR_GUEST
}

TEST(Core, TheDefaultAllocatorIsCutils) {
  EXPECT_EQ(grdbg_allocator_default(), gcu_allocator_default());
}

TEST(Limits, DefaultsAreTheDocumentedOnes) {
  GRDBG_Limits limits;
  std::memset(&limits, 0xff, sizeof limits);
  grdbg_limits_default(&limits);
  EXPECT_EQ(limits.max_header_bytes, 1024u);
  EXPECT_EQ(limits.max_message_bytes, 4u * 1024u * 1024u);
  EXPECT_EQ(limits.max_json_depth, 64u);
  EXPECT_EQ(limits.max_breakpoints, 1024u);
  EXPECT_EQ(limits.max_frames, 1000u);
  EXPECT_EQ(limits.max_variables, 1000u);
  grdbg_limits_default(nullptr);  // ignored
}

TEST(Limits, ANullLimitsAndAZeroFieldBothMeanTheDefaultAtAttach) {
  ToyWorld w(basic_program());
  // A zero max_breakpoints is the default (1024), not a cap of zero.
  GRDBG_Limits limits = {};
  ASSERT_EQ(w.attach(&limits), GRDBG_OK);
  std::vector<int> lines(1024, 1);
  std::vector<uint64_t> ids(1024);
  EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", lines.data(), lines.size(), ids.data()), GRDBG_OK);
  lines.push_back(1);
  ids.resize(lines.size());
  EXPECT_EQ(grdbg_debugger_set_breakpoints(w.dbg, "a.toy", lines.data(), lines.size(), ids.data()), GRDBG_ERR_LIMIT);
}

GRDBG_TEST_MAIN()
