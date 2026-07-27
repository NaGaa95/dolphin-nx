// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/UiAudio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "DolphinSwitch/Audio.h"

namespace DolphinSwitch
{
namespace
{
constexpr int SAMPLE_RATE = 48000;
std::vector<std::int16_t> s_navigate;
std::vector<std::int16_t> s_confirm;
std::vector<std::int16_t> s_back;

std::vector<std::int16_t> MakeTone(float start_hz, float end_hz, int milliseconds, float volume)
{
  const int frames = SAMPLE_RATE * milliseconds / 1000;
  std::vector<std::int16_t> samples(static_cast<std::size_t>(frames) * 2);
  double phase = 0.0;
  for (int frame = 0; frame < frames; ++frame)
  {
    const float progress = frames > 1 ? static_cast<float>(frame) / (frames - 1) : 0.0f;
    const float frequency = start_hz + (end_hz - start_hz) * progress;
    phase += 6.283185307179586 * frequency / SAMPLE_RATE;
    const float attack = std::min(1.0f, progress * 10.0f);
    const float release = std::max(0.0f, 1.0f - progress);
    const float envelope = attack * release * release;
    const auto value = static_cast<std::int16_t>(
        std::sin(phase) * envelope * volume * 32767.0f);
    samples[static_cast<std::size_t>(frame) * 2] = value;
    samples[static_cast<std::size_t>(frame) * 2 + 1] = value;
  }
  return samples;
}

const std::vector<std::int16_t>& SamplesFor(UiSound sound)
{
  switch (sound)
  {
  case UiSound::Navigate:
    return s_navigate;
  case UiSound::Confirm:
    return s_confirm;
  case UiSound::Back:
    return s_back;
  }
  return s_navigate;
}
}  // namespace

bool InitializeUiAudio()
{
  if (!Audio::InitializeFrontendAudio())
    return false;
  s_navigate = MakeTone(920.0f, 1040.0f, 18, 0.10f);
  s_confirm = MakeTone(620.0f, 980.0f, 42, 0.16f);
  s_back = MakeTone(760.0f, 420.0f, 48, 0.14f);
  return true;
}

void SetUiAudioEnabled(bool enabled)
{
  Audio::SetFrontendAudioEnabled(enabled);
}

void PlayUiSound(UiSound sound)
{
  const auto& samples = SamplesFor(sound);
  Audio::QueueFrontendAudio(samples.data(), samples.size());
}

void ShutdownUiAudio()
{
  Audio::ReleaseFrontendAudio();
  s_navigate.clear();
  s_confirm.clear();
  s_back.clear();
}
}  // namespace DolphinSwitch
