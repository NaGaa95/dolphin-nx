// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace DolphinSwitch::Updater
{
enum class State
{
  Idle,
  Checking,
  UpdateAvailable,
  UpToDate,
  Downloading,
  ReadyToInstall,
  Installing,
  Installed,
  Cancelled,
  Error,
};

struct ReleaseInfo
{
  std::string tag;
  std::string name;
  std::string notes;
  std::string page_url;
  std::string asset_name;
  std::string asset_url;
  std::string asset_digest;
  std::uint64_t asset_size = 0;
};

struct Snapshot
{
  State state = State::Idle;
  ReleaseInfo release;
  std::string error;
  std::uint64_t downloaded = 0;
  std::uint64_t total = 0;
};

const char* BuiltReleaseTag();
bool IsNewer(const std::string& candidate, const std::string& installed);
std::string ResolveLauncherPath(std::string_view path);

bool StartCheck(const std::string& installed_tag);
bool StartDownload(const std::string& launcher_path);
bool InstallDownloaded(const std::string& launcher_path);
void Cancel();
void RequestInstallation();
bool ConsumeInstallationRequest();
Snapshot GetSnapshot();
void Shutdown();

bool RecoverInstallation(const std::string& launcher_path, std::string& error);
}  // namespace DolphinSwitch::Updater
