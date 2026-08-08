// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>

#include "AudioCommon/SoundStream.h"
#include "Common/CommonTypes.h"

namespace DolphinSwitch::Audio
{
bool InitializeFrontendAudio();
void SetFrontendAudioEnabled(bool enabled);
void QueueFrontendAudio(const s16* samples, std::size_t sample_count);
void ReleaseFrontendAudio();
void ResumeSharedAudio();
void ResetSharedAudioDevice();
void ShutdownSharedAudio();

class SwitchStream final : public SoundStream
{
public:
  SwitchStream() = default;
  ~SwitchStream() override;

  bool Init() override;
  bool SetRunning(bool running) override;
  static bool IsValid() { return true; }

private:
  static void AudioCallback(void* userdata, u8* stream, int length);

  bool m_attached = false;
  std::atomic<bool> m_running{false};
};
}  // namespace DolphinSwitch::Audio
