// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

namespace Core
{
class System;
}

namespace DolphinSwitch
{
// An SMB GCI folder is played from an SD copy, synced before boot and after Core::Shutdown. The
// baseline is written before boot, so changes made before a force close from HOME are synced the
// next time the folder is used.
class GciSyncSession final
{
public:
  GciSyncSession();
  ~GciSyncSession();

  GciSyncSession(const GciSyncSession&) = delete;
  GciSyncSession& operator=(const GciSyncSession&) = delete;

  // Must run after per-game configuration layers are loaded and before Core::Init.
  bool Prepare(Core::System& system, std::string* error);

  // Must run after Core::Shutdown so every local GCI write has been closed. Failure never removes
  // the local copy; the returned warning explains that synchronization remains pending.
  bool Finish(std::string* warning);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
}  // namespace DolphinSwitch
