// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace DolphinSwitch
{
enum class UiSound
{
  Navigate,
  Confirm,
  Back,
};

bool InitializeUiAudio();
void SetUiAudioEnabled(bool enabled);
void PlayUiSound(UiSound sound);
void ShutdownUiAudio();
}  // namespace DolphinSwitch
