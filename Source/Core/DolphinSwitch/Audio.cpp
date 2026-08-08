// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/Audio.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "AudioCommon/Mixer.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"

namespace DolphinSwitch::Audio
{
namespace
{
constexpr int SAMPLE_RATE = 48000;
constexpr std::size_t MAX_FRONTEND_QUEUED_SAMPLES = SAMPLE_RATE;

using AudioTargetCallback = void (*)(void*, u8*, int);

SDL_AudioDeviceID s_device = 0;
bool s_audio_subsystem_initialized = false;
bool s_frontend_active = false;
bool s_frontend_enabled = true;
std::vector<s16> s_frontend_queue;
std::size_t s_frontend_queue_offset = 0;
void* s_target_userdata = nullptr;
AudioTargetCallback s_target_callback = nullptr;
std::atomic<bool> s_audio_thread_configured{false};

void SharedAudioCallback(void*, u8* stream, int length)
{
  if (s_target_callback)
  {
    s_target_callback(s_target_userdata, stream, length);
    return;
  }

  std::memset(stream, 0, static_cast<std::size_t>(length));
  if (!s_frontend_active || !s_frontend_enabled || s_frontend_queue_offset >= s_frontend_queue.size())
    return;

  const std::size_t requested_samples = static_cast<std::size_t>(length) / sizeof(s16);
  const std::size_t available_samples = s_frontend_queue.size() - s_frontend_queue_offset;
  const std::size_t copied_samples = std::min(requested_samples, available_samples);
  std::memcpy(stream, s_frontend_queue.data() + s_frontend_queue_offset,
              copied_samples * sizeof(s16));
  s_frontend_queue_offset += copied_samples;
  if (s_frontend_queue_offset == s_frontend_queue.size())
  {
    s_frontend_queue.clear();
    s_frontend_queue_offset = 0;
  }
}

bool EnsureSharedDevice()
{
  if (s_device != 0)
    return true;

  const bool initialized_here = !s_audio_subsystem_initialized;
  if (initialized_here && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
  {
    ERROR_LOG_FMT(AUDIO, "SDL audio initialization failed: {}", SDL_GetError());
    return false;
  }
  if (initialized_here)
    s_audio_subsystem_initialized = true;

  SDL_AudioSpec requested{};
  requested.freq = SAMPLE_RATE;
  requested.format = AUDIO_S16SYS;
  requested.channels = 2;
  requested.samples = 1024;
  requested.callback = SharedAudioCallback;

  SDL_AudioSpec obtained{};
  s_audio_thread_configured.store(false, std::memory_order_release);
  s_device = SDL_OpenAudioDevice(nullptr, 0, &requested, &obtained, 0);
  if (s_device == 0)
  {
    ERROR_LOG_FMT(AUDIO, "SDL_OpenAudioDevice failed: {}", SDL_GetError());
    if (initialized_here)
    {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      s_audio_subsystem_initialized = false;
    }
    return false;
  }

  SDL_PauseAudioDevice(s_device, 0);
  return true;
}

bool AttachTarget(void* userdata, AudioTargetCallback callback)
{
  if (!EnsureSharedDevice())
    return false;

  SDL_LockAudioDevice(s_device);
  const bool available = s_target_userdata == nullptr || s_target_userdata == userdata;
  if (available)
  {
    s_frontend_active = false;
    s_frontend_queue.clear();
    s_frontend_queue_offset = 0;
    s_target_userdata = userdata;
    s_target_callback = callback;
  }
  SDL_UnlockAudioDevice(s_device);
  if (available)
  {
    SDL_PauseAudioDevice(s_device, 0);
  }
  return available;
}

void DetachTarget(void* userdata)
{
  if (s_device == 0)
    return;

  SDL_LockAudioDevice(s_device);
  if (s_target_userdata == userdata)
  {
    s_target_callback = nullptr;
    s_target_userdata = nullptr;
  }
  SDL_UnlockAudioDevice(s_device);
}

void CloseSharedDevice(bool shutdown_subsystem)
{
  if (s_device != 0)
  {
    const SDL_AudioDeviceID device = s_device;
    SDL_LockAudioDevice(device);
    s_target_callback = nullptr;
    s_target_userdata = nullptr;
    s_frontend_active = false;
    s_frontend_queue.clear();
    s_frontend_queue_offset = 0;
    SDL_UnlockAudioDevice(device);
    s_device = 0;

    // Pausing immediately before close can deadlock SDL's libnx driver.
    SDL_CloseAudioDevice(device);
    s_audio_thread_configured.store(false, std::memory_order_release);
  }

  if (shutdown_subsystem && s_audio_subsystem_initialized)
  {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    s_audio_subsystem_initialized = false;
  }
}
}  // namespace

bool InitializeFrontendAudio()
{
  if (!EnsureSharedDevice())
    return false;

  SDL_LockAudioDevice(s_device);
  s_frontend_active = true;
  s_frontend_queue.clear();
  s_frontend_queue_offset = 0;
  SDL_UnlockAudioDevice(s_device);
  SDL_PauseAudioDevice(s_device, 0);
  return true;
}

void SetFrontendAudioEnabled(bool enabled)
{
  if (s_device == 0)
  {
    s_frontend_enabled = enabled;
    return;
  }

  SDL_LockAudioDevice(s_device);
  s_frontend_enabled = enabled;
  if (!enabled)
  {
    s_frontend_queue.clear();
    s_frontend_queue_offset = 0;
  }
  SDL_UnlockAudioDevice(s_device);
}

void QueueFrontendAudio(const s16* samples, std::size_t sample_count)
{
  if (s_device == 0 || !samples || sample_count == 0)
    return;

  SDL_LockAudioDevice(s_device);
  if (s_frontend_active && s_frontend_enabled && s_target_userdata == nullptr)
  {
    const std::size_t remaining_samples = s_frontend_queue.size() - s_frontend_queue_offset;
    if (remaining_samples > MAX_FRONTEND_QUEUED_SAMPLES)
    {
      s_frontend_queue.clear();
      s_frontend_queue_offset = 0;
    }
    else if (s_frontend_queue_offset != 0)
    {
      s_frontend_queue.erase(s_frontend_queue.begin(),
                             s_frontend_queue.begin() + s_frontend_queue_offset);
      s_frontend_queue_offset = 0;
    }
    s_frontend_queue.insert(s_frontend_queue.end(), samples, samples + sample_count);
  }
  SDL_UnlockAudioDevice(s_device);
}

void ReleaseFrontendAudio()
{
  if (s_device == 0)
    return;

  SDL_LockAudioDevice(s_device);
  s_frontend_active = false;
  s_frontend_queue.clear();
  s_frontend_queue_offset = 0;
  SDL_UnlockAudioDevice(s_device);
}

void ResumeSharedAudio()
{
  if (s_device != 0)
    SDL_PauseAudioDevice(s_device, 0);
}

void ResetSharedAudioDevice()
{
  // The libnx audio output can become stale after it has served launcher sounds. Reopen only the
  // device here: repeatedly quitting and reinitializing an SDL subsystem in the same Horizon
  // process can disturb the other SDL platform drivers used by the launcher.
  CloseSharedDevice(false);
}

void ShutdownSharedAudio()
{
  CloseSharedDevice(true);
}

SwitchStream::~SwitchStream()
{
  m_running.store(false, std::memory_order_release);
  if (m_attached)
  {
    DetachTarget(this);
    m_attached = false;
  }
  // Keep SDL's audio subsystem alive across game/launcher transitions. The process-level guard
  // performs the matching SDL_QuitSubSystem once when Dolphin actually exits.
  CloseSharedDevice(false);
}

bool SwitchStream::Init()
{
  if (!AttachTarget(this, AudioCallback))
  {
    ERROR_LOG_FMT(AUDIO, "Could not attach Dolphin mixer to the shared Switch audio device");
    return false;
  }
  m_attached = true;
  return true;
}

bool SwitchStream::SetRunning(bool running)
{
  m_running.store(running, std::memory_order_release);
  if (running && s_device != 0)
    SDL_PauseAudioDevice(s_device, 0);
  return true;
}

void SwitchStream::AudioCallback(void* userdata, u8* stream, int length)
{
  if (!s_audio_thread_configured.exchange(true, std::memory_order_acq_rel))
  {
    Common::SetCurrentThreadName("Dolphin audio");
    Common::SetCurrentThreadAffinity(2);
    Common::AdjustCurrentThreadPriority(-4);
  }

  auto* const self = static_cast<SwitchStream*>(userdata);
  if (!self->m_running.load(std::memory_order_acquire) || self->GetMixer() == nullptr)
  {
    std::memset(stream, 0, static_cast<std::size_t>(length));
    return;
  }

  constexpr int bytes_per_stereo_frame = sizeof(s16) * 2;
  const unsigned int frames = static_cast<unsigned int>(length / bytes_per_stereo_frame);
  self->GetMixer()->Mix(reinterpret_cast<s16*>(stream), frames);
}
}  // namespace DolphinSwitch::Audio
