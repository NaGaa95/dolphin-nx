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
// Keeps SMB-backed GCI folders safe by using an SD-resident working copy while emulation is
// running. The SMB folder is reconciled before boot and after Core::Shutdown. A durable baseline
// manifest is written before boot, so a HOME-menu force close leaves local changes pending rather
// than losing them; they are reconciled the next time the same folder is used.
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
