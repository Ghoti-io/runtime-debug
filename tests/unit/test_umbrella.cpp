/**
 * @file
 *
 * The umbrella header, included first and alone from C++.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/runtime-debug/runtime-debug.h>

#include <gtest/gtest.h>

#include <cstring>

TEST(Umbrella, DeclaresTheWholeSurfaceFromCxx) {
  EXPECT_STRNE(grdbg_version_string(), "");
  EXPECT_STREQ(grdbg_result_string(GRDBG_OK), "No error");
  GRDBG_Limits limits;
  grdbg_limits_default(&limits);
  EXPECT_EQ(limits.max_frames, 1000u);
  EXPECT_NE(grdbg_allocator_default(), nullptr);
  EXPECT_NE(grdbg_debugger_key(), nullptr);
  // Every area has an entry point that is reachable and refuses a NULL.
  EXPECT_EQ(grdbg_debugger_get(nullptr), nullptr);
  EXPECT_EQ(grdbg_debugger_continue(nullptr), GRDBG_ERR_INVALID);
  GRDBG_Dap * dap = nullptr;
  EXPECT_EQ(grdbg_dap_create(nullptr, nullptr, nullptr, &dap), GRDBG_ERR_INVALID);
  GRDBG_Transport * transport = nullptr;
  EXPECT_EQ(grdbg_transport_create_memory(nullptr, 0, nullptr, &transport), GRDBG_OK);
  grdbg_transport_destroy(transport);
  GRDBG_ServeResult serve = GRDBG_SERVE_RESUME;
  EXPECT_EQ(grdbg_dap_serve(nullptr, &serve), GRDBG_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
