// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace DolphinSwitch
{
struct LaunchRequest
{
  std::string path;
  std::string game_id;
  unsigned revision = 0;
  std::optional<std::uint64_t> nand_title;
  std::string game_config_path;
};

std::optional<LaunchRequest> RunLauncher(std::string startup_message = {},
                                         std::string launcher_path = {});

bool RecordInstalledReleaseTag(std::string_view tag);

bool PrepareLaunchStorage(const std::string& path, std::string* resolved_path = nullptr);

void ShutdownLauncherStorage();
}  // namespace DolphinSwitch
