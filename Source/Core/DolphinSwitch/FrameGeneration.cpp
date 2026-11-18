// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/FrameGeneration.h"
#include "DolphinSwitch/Deko3DName.h"

#include <atomic>
#include <mutex>

#include <lsfg/common/cache_format.hpp>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/LSFGControl.h"
#endif

namespace DolphinSwitch::FrameGeneration
{
namespace
{
std::atomic_bool s_available{false};
std::atomic_bool s_enabled{false};
std::atomic_bool s_passthrough{false};
std::atomic_bool s_high_refresh{false};
std::mutex s_mutex;
std::string s_status = "Disabled in the launcher";

bool IsDekoBackend()
{
  return Config::Get(Config::MAIN_GFX_BACKEND) == "Deko3D";
}
}

bool IsDllInstalled()
{
  return File::IsFile(DLL_DISPLAY_PATH);
}

bool AreDekoShadersPrepared()
{
  for (const char* mode : {"Performance", "Quality"})
  {
#ifdef __SWITCH2__
    // (DKFrameGenerationShaders.cpp)
    File::IOFile manifest(std::string("sdmc:/switch/dolphin/lsfg/Cache-nx2/") + mode + "/manifest.bin",
                          "rb");
#else
    File::IOFile manifest(std::string("sdmc:/switch/dolphin/lsfg/Cache/") + mode + "/manifest.bin",
                          "rb");
#endif
    lsfg::cache::ManifestHeader header{};
    if (!manifest || !manifest.ReadBytes(&header, sizeof(header)) ||
        !lsfg::succeeded(lsfg::cache::validate(header)) ||
        header.backend_abi_version != lsfg::cache::backend_abi_version)
      return false;
  }
  return true;
}

bool IsAvailable()
{
  if (IsDekoBackend())
    return s_available.load(std::memory_order_acquire);
#ifdef HAS_VULKAN
  if (Config::Get(Config::MAIN_GFX_BACKEND) == "Vulkan")
    return Vulkan::LSFG::IsAvailable();
#endif
  return false;
}

bool IsEnabled()
{
  if (IsDekoBackend())
    return s_enabled.load(std::memory_order_acquire);
#ifdef HAS_VULKAN
  if (Config::Get(Config::MAIN_GFX_BACKEND) == "Vulkan")
    return Vulkan::LSFG::IsEnabled();
#endif
  return false;
}

bool IsHighFPSPassthrough()
{
  if (IsDekoBackend())
    return s_passthrough.load(std::memory_order_acquire);
#ifdef HAS_VULKAN
  if (Config::Get(Config::MAIN_GFX_BACKEND) == "Vulkan")
    return Vulkan::LSFG::IsHighFPSPassthrough();
#endif
  return false;
}

// The docked 120 Hz output is for a 50/60 FPS game: asked once the game shows that rate (passed
// through at 60 Hz) while frame generation runs with the option on, then kept until frame
// generation stops, so a game that slows down for a while does not switch the TV back and forth.
// A 30 FPS game stays at the TV's resolution and 60 Hz.
bool WantsHighRefresh()
{
  if (!Config::Get(Config::GFX_LSFG_120HZ_OUTPUT) || !IsEnabled())
  {
    s_high_refresh.store(false, std::memory_order_relaxed);
    return false;
  }
  if (IsHighFPSPassthrough())
    s_high_refresh.store(true, std::memory_order_relaxed);
  return s_high_refresh.load(std::memory_order_relaxed);
}

bool RequestEnabled(bool enabled)
{
  if (IsDekoBackend())
  {
    std::lock_guard lock{s_mutex};
    if (enabled && !s_available.load(std::memory_order_acquire))
      return false;
    s_enabled.store(enabled, std::memory_order_release);
    s_passthrough.store(false, std::memory_order_release);
    s_status = enabled ? "Frame generation On" : "Available; currently Off";
    return true;
  }
#ifdef HAS_VULKAN
  if (Config::Get(Config::MAIN_GFX_BACKEND) == "Vulkan")
    return Vulkan::LSFG::RequestEnabled(enabled);
#endif
  return false;
}

std::string GetStatus()
{
  if (IsDekoBackend())
  {
    std::lock_guard lock{s_mutex};
    return s_status;
  }
#ifdef HAS_VULKAN
  if (Config::Get(Config::MAIN_GFX_BACKEND) == "Vulkan")
    return Vulkan::LSFG::GetStatus();
#endif
  return "Frame generation requires Vulkan or " DEKO3D_DISPLAY_NAME;
}

void BeginDekoSession()
{
  std::lock_guard lock{s_mutex};
  s_enabled.store(false, std::memory_order_release);
  s_passthrough.store(false, std::memory_order_release);
  const bool prepared = AreDekoShadersPrepared();
  const bool available = Config::Get(Config::GFX_LSFG_ENABLED) && prepared;
  s_available.store(available, std::memory_order_release);
  s_status = available                              ? "Available; currently Off" :
             !Config::Get(Config::GFX_LSFG_ENABLED) ? "Disabled in the launcher" :
             !IsDllInstalled()                      ? "Lossless.dll is missing" :
                                                      DEKO3D_DISPLAY_NAME " LSFG shaders could not be loaded";
}

void EndDekoSession()
{
  std::lock_guard lock{s_mutex};
  s_available.store(false, std::memory_order_release);
  s_enabled.store(false, std::memory_order_release);
  s_passthrough.store(false, std::memory_order_release);
  s_status = "Disabled in the launcher";
}

void SetDekoHighFPSPassthrough(bool passthrough)
{
  s_passthrough.store(s_enabled.load(std::memory_order_acquire) && passthrough,
                      std::memory_order_release);
}

void DisableDekoSession(const std::string& reason)
{
  std::lock_guard lock{s_mutex};
  s_available.store(false, std::memory_order_release);
  s_enabled.store(false, std::memory_order_release);
  s_passthrough.store(false, std::memory_order_release);
  s_status = reason;
}
}
