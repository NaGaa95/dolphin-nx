// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "DiscIO/GameModDescriptor.h"

namespace
{
TEST(GameModDescriptor, KeepsDevoptabAndUriPathsRooted)
{
  constexpr std::string_view descriptor_json = R"json(
  {
    "type": "dolphin-game-mod-descriptor",
    "version": 1,
    "base-file": "sdmc:/games/New Super Mario Bros. Wii.rvz",
    "banner": "ums0:/art/banner.png",
    "riivolution": {
      "patches": [
        {
          "xml": "dsmb_example:/riivolution/NewerSMBW.xml",
          "root": "sdmc:"
        },
        {
          "xml": "content://provider/document/patch.xml",
          "root": "ums1:/"
        }
      ]
    }
  })json";

  const auto descriptor = DiscIO::ParseGameModDescriptorString(
      descriptor_json, "sdmc:/switch/dolphin/Load/Riivolution/Presets/test.json");
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->base_file, "sdmc:/games/New Super Mario Bros. Wii.rvz");
  EXPECT_EQ(descriptor->banner, "ums0:/art/banner.png");
  ASSERT_TRUE(descriptor->riivolution.has_value());
  ASSERT_EQ(descriptor->riivolution->patches.size(), 2u);
  EXPECT_EQ(descriptor->riivolution->patches[0].xml,
            "dsmb_example:/riivolution/NewerSMBW.xml");
  EXPECT_EQ(descriptor->riivolution->patches[0].root, "sdmc:");
  EXPECT_EQ(descriptor->riivolution->patches[1].xml,
            "content://provider/document/patch.xml");
  EXPECT_EQ(descriptor->riivolution->patches[1].root, "ums1:/");
}
}  // namespace
