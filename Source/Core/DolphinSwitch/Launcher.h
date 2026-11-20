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
  // The launcher identity a HOME Menu shortcut carries: it finds the game's current path after a
  // rename or a USB renumbering.
  std::string library_id;
};

std::optional<LaunchRequest> RunLauncher(std::string startup_message = {},
                                         std::string launcher_path = {});

bool RunAppletInstaller(std::string launcher_path = {});

bool LaunchEmbeddedRuntime(const std::string& launcher_path, const LaunchRequest& request,
                           std::string* error);

bool RecordInstalledReleaseTag(std::string_view tag);

bool PrepareLaunchStorage(const std::string& path, std::string* resolved_path = nullptr);

// Resolves a shortcut's library identity to the game's current path (the embedded path is the
// fallback for older launcher.ini data).
bool ResolveLibraryLaunchPath(std::string_view library_id, const std::string& fallback_path,
                              std::string* resolved_path = nullptr);

void ShutdownLauncherStorage();
}  // namespace DolphinSwitch
