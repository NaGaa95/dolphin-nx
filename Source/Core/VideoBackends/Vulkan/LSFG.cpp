// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/LSFG.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <vector>

#include "Core/Config/GraphicsSettings.h"

#include "VideoBackends/Vulkan/VulkanContext.h"

#include "lsfg_bridge.h"

namespace Vulkan::LSFG
{
namespace
{
struct SessionState
{
  std::mutex mutex;
  std::atomic_bool prepared{false};
  std::atomic_bool available{false};
  std::atomic_bool enabled{false};

  bool initialization_attempted = false;
  bool environment_modified = false;
  bool environment_had_value = false;
  std::string original_environment;
  std::string status = "Disabled in the launcher";

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkExtent2D extent{};
  std::vector<VkImage> images;
  LsfgNxRuntime* runtime = nullptr;

  std::uint64_t last_source_present_ns = 0;
  double source_interval_ns = 0.0;
  unsigned source_samples = 0;
  unsigned high_fps_slow_samples = 0;
  bool previous_requested = false;
  std::atomic_int rate_decision{-1};
};

SessionState s_state;

bool ContainsDebugToken(std::string_view value, std::string_view token)
{
  while (!value.empty())
  {
    const std::size_t separator = value.find(',');
    std::string_view item = value.substr(0, separator);
    while (!item.empty() && item.front() == ' ')
      item.remove_prefix(1);
    while (!item.empty() && item.back() == ' ')
      item.remove_suffix(1);
    if (item == token)
      return true;
    if (separator == std::string_view::npos)
      break;
    value.remove_prefix(separator + 1);
  }
  return false;
}

void DestroyRuntimeLocked()
{
  if (!s_state.runtime)
    return;

  lsfg_nx_destroy(s_state.runtime);
  s_state.runtime = nullptr;
}

void RestoreEnvironmentLocked()
{
  if (!s_state.environment_modified)
    return;

  if (s_state.environment_had_value)
    setenv("NVK_DEBUG", s_state.original_environment.c_str(), 1);
  else
    unsetenv("NVK_DEBUG");
  s_state.environment_modified = false;
  s_state.environment_had_value = false;
  s_state.original_environment.clear();
}

void DisableSessionLocked(const char* reason)
{
  DestroyRuntimeLocked();
  s_state.enabled.store(false, std::memory_order_release);
  s_state.available.store(false, std::memory_order_release);
  s_state.prepared.store(false, std::memory_order_release);
  s_state.initialization_attempted = false;
  s_state.status = reason && *reason ? reason : "Frame generation is unavailable";
}

VkResult PresentNormally(VkQueue queue, const VkPresentInfoKHR& present_info)
{
  return vkQueuePresentKHR(queue, &present_info);
}

void ResetRateTrackingLocked()
{
  s_state.last_source_present_ns = 0;
  s_state.source_interval_ns = 0.0;
  s_state.source_samples = 0;
  s_state.high_fps_slow_samples = 0;
  s_state.previous_requested = false;
  s_state.rate_decision = -1;
}

std::uint64_t ObserveSourcePresentLocked()
{
  const std::uint64_t now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  std::uint64_t interval = 0;
  if (s_state.last_source_present_ns != 0 && now > s_state.last_source_present_ns)
  {
    interval = now - s_state.last_source_present_ns;
    if (interval >= 4'000'000 && interval <= 100'000'000)
    {
      if (s_state.source_samples == 0)
        s_state.source_interval_ns = static_cast<double>(interval);
      else
        s_state.source_interval_ns =
            s_state.source_interval_ns * 0.875 + static_cast<double>(interval) * 0.125;
      if (s_state.source_samples < 120)
        ++s_state.source_samples;
    }
    else
    {
      interval = 0;
    }
  }
  s_state.last_source_present_ns = now;
  return interval;
}

bool SourceIsHighRateLocked()
{
  return s_state.source_samples >= 8 && s_state.source_interval_ns > 0.0 &&
         s_state.source_interval_ns < 30'000'000.0;
}
}  // namespace

bool IsDllInstalled()
{
  std::FILE* file = std::fopen(DLL_PATH, "rb");
  if (!file)
    return false;
  std::fclose(file);
  return true;
}

void BeginSession()
{
  std::lock_guard lock{s_state.mutex};

  DestroyRuntimeLocked();
  RestoreEnvironmentLocked();
  s_state.prepared.store(false, std::memory_order_release);
  s_state.available.store(false, std::memory_order_release);
  s_state.enabled.store(false, std::memory_order_release);
  s_state.initialization_attempted = false;
  s_state.swapchain = VK_NULL_HANDLE;
  s_state.extent = {};
  s_state.images.clear();
  ResetRateTrackingLocked();

  const bool requested = Config::Get(Config::GFX_LSFG_ENABLED);
  const bool dll_installed = IsDllInstalled();
  if (!requested)
  {
    s_state.status = "Disabled in the launcher";
    return;
  }
  if (!dll_installed)
  {
    s_state.status = "Lossless.dll is missing";
    return;
  }

  const char* current = std::getenv("NVK_DEBUG");
  const std::string_view current_view = current ? std::string_view{current} : std::string_view{};
  if (!ContainsDebugToken(current_view, "no_cbuf"))
  {
    s_state.environment_had_value = current != nullptr;
    if (current)
      s_state.original_environment = current;
    const std::string replacement = current_view.empty() ?
                                        std::string{"no_cbuf"} :
                                        std::string{current_view} + ",no_cbuf";
    if (setenv("NVK_DEBUG", replacement.c_str(), 1) != 0)
    {
      s_state.environment_had_value = false;
      s_state.original_environment.clear();
      s_state.status = "NVK could not be prepared for LSFG";
      return;
    }
    s_state.environment_modified = true;
  }

  s_state.prepared.store(true, std::memory_order_release);
  s_state.status = "Prepared; enable it from the in-game quick menu";
}

void FinishInstanceCreation()
{
  std::lock_guard lock{s_state.mutex};
  RestoreEnvironmentLocked();
}

void EndSession()
{
  std::lock_guard lock{s_state.mutex};
  DestroyRuntimeLocked();
  RestoreEnvironmentLocked();
  s_state.enabled.store(false, std::memory_order_release);
  s_state.available.store(false, std::memory_order_release);
  s_state.prepared.store(false, std::memory_order_release);
  s_state.initialization_attempted = false;
  s_state.swapchain = VK_NULL_HANDLE;
  s_state.extent = {};
  s_state.images.clear();
  ResetRateTrackingLocked();
  s_state.status = "Disabled in the launcher";
}

bool IsSessionPrepared()
{
  return s_state.prepared.load(std::memory_order_acquire);
}

void DisableSession(const char* reason)
{
  std::lock_guard lock{s_state.mutex};
  DisableSessionLocked(reason);
  RestoreEnvironmentLocked();
}

bool RegisterSwapChain(VkSwapchainKHR swapchain, VkExtent2D extent,
                       std::span<const VkImage> images)
{
  std::lock_guard lock{s_state.mutex};
  DestroyRuntimeLocked();
  s_state.initialization_attempted = false;
  s_state.available.store(false, std::memory_order_release);
  s_state.swapchain = VK_NULL_HANDLE;
  s_state.images.clear();

  if (!s_state.prepared.load(std::memory_order_acquire))
    return false;
  if (!g_vulkan_context || swapchain == VK_NULL_HANDLE || extent.width == 0 ||
      extent.height == 0 || images.size() < 3)
  {
    DisableSessionLocked("The Vulkan swapchain is not compatible with LSFG");
    return false;
  }
  if (g_vulkan_context->GetGraphicsQueue() != g_vulkan_context->GetPresentQueue() ||
      g_vulkan_context->GetGraphicsQueueFamilyIndex() !=
          g_vulkan_context->GetPresentQueueFamilyIndex())
  {
    DisableSessionLocked("Separate graphics and present queues are unsupported by LSFG");
    return false;
  }

  s_state.swapchain = swapchain;
  s_state.extent = extent;
  s_state.images.assign(images.begin(), images.end());
  ResetRateTrackingLocked();
  s_state.available.store(true, std::memory_order_release);
  s_state.status = s_state.enabled.load(std::memory_order_acquire) ?
                       "Available; frame generation will resume" :
                       "Available; currently Off";
  return true;
}

void UnregisterSwapChain()
{
  std::lock_guard lock{s_state.mutex};
  DestroyRuntimeLocked();
  s_state.available.store(false, std::memory_order_release);
  s_state.initialization_attempted = false;
  s_state.swapchain = VK_NULL_HANDLE;
  s_state.extent = {};
  s_state.images.clear();
  ResetRateTrackingLocked();
  if (s_state.prepared.load(std::memory_order_acquire))
    s_state.status = "Waiting for the Vulkan swapchain";
}

VkResult Present(VkQueue queue, const VkPresentInfoKHR& present_info)
{
  std::lock_guard lock{s_state.mutex};

  const bool enabled = s_state.enabled.load(std::memory_order_acquire);
  const bool compatible = s_state.available.load(std::memory_order_acquire) &&
                          present_info.swapchainCount == 1 && present_info.pSwapchains &&
                          present_info.pSwapchains[0] == s_state.swapchain;
  std::uint64_t source_interval = 0;
  if (compatible && (!enabled || s_state.rate_decision != 0))
    source_interval = ObserveSourcePresentLocked();

  if (compatible && enabled != s_state.previous_requested)
  {
    if (enabled)
    {
      s_state.rate_decision = s_state.source_samples >= 8 ?
                                  (SourceIsHighRateLocked() ? 1 : 0) :
                                  -1;
      s_state.high_fps_slow_samples = 0;
    }
    else
    {
      ResetRateTrackingLocked();
    }
    s_state.previous_requested = enabled;
  }

  if (!enabled || !compatible)
  {
    if (!enabled && s_state.runtime)
    {
      DestroyRuntimeLocked();
      s_state.initialization_attempted = false;
      if (s_state.available.load(std::memory_order_acquire))
        s_state.status = "Available; currently Off";
    }
    return PresentNormally(queue, present_info);
  }

  if (s_state.rate_decision < 0 && s_state.source_samples >= 8)
    s_state.rate_decision = SourceIsHighRateLocked() ? 1 : 0;

  if (s_state.rate_decision == 1)
  {
    s_state.status = "Frame generation On (native 50/60 FPS protected)";
    if (source_interval >= 31'500'000)
      ++s_state.high_fps_slow_samples;
    else if (source_interval != 0)
      s_state.high_fps_slow_samples = 0;

    if (s_state.high_fps_slow_samples < 16)
      return PresentNormally(queue, present_info);

    s_state.rate_decision = 0;
    s_state.high_fps_slow_samples = 0;
  }

  if (s_state.rate_decision < 0)
  {
    s_state.status = "Measuring the game's native frame rate";
    return PresentNormally(queue, present_info);
  }

  if (!s_state.runtime)
  {
    if (s_state.initialization_attempted || !g_vulkan_context)
    {
      s_state.enabled.store(false, std::memory_order_release);
      return PresentNormally(queue, present_info);
    }
    s_state.initialization_attempted = true;

    float flow_scale = Config::Get(Config::GFX_LSFG_FLOW_SCALE);
    if (flow_scale != 0.25f && flow_scale != 0.5f)
      flow_scale = 0.25f;

    const LsfgNxCreateInfo create_info{
        .instance = g_vulkan_context->GetVulkanInstance(),
        .physical_device = g_vulkan_context->GetPhysicalDevice(),
        .device = g_vulkan_context->GetDevice(),
        .queue = queue,
        .queue_family_index = g_vulkan_context->GetPresentQueueFamilyIndex(),
        .get_instance_proc_addr = vkGetInstanceProcAddr,
        .swapchain = s_state.swapchain,
        .extent = s_state.extent,
        .swapchain_images = s_state.images.data(),
        .swapchain_image_count = static_cast<u32>(s_state.images.size()),
        .shader_dll_path = DLL_PATH,
        .flow_scale = flow_scale,
        .performance_mode = Config::Get(Config::GFX_LSFG_PERFORMANCE_MODE),
    };
    s_state.runtime = lsfg_nx_create(&create_info);
    if (!s_state.runtime)
    {
      s_state.enabled.store(false, std::memory_order_release);
      s_state.available.store(false, std::memory_order_release);
      s_state.status = "LSFG initialization failed";
      // Creation did not consume the render-finished semaphore.
      return PresentNormally(queue, present_info);
    }
    s_state.status = "Frame generation On";
  }

  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  if (!lsfg_nx_present(s_state.runtime, queue, &present_info, &result))
  {
    s_state.status = "LSFG rejected the active swapchain";
    s_state.enabled.store(false, std::memory_order_release);
    s_state.available.store(false, std::memory_order_release);
    DestroyRuntimeLocked();
    // The bridge did not consume this presentation.
    return PresentNormally(queue, present_info);
  }

  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
  {
    s_state.status = "LSFG presentation failed; recreating the swapchain";
    s_state.enabled.store(false, std::memory_order_release);
    s_state.available.store(false, std::memory_order_release);
    DestroyRuntimeLocked();
  }
  return result;
}

bool IsAvailable()
{
  return s_state.available.load(std::memory_order_acquire);
}

bool IsEnabled()
{
  return s_state.enabled.load(std::memory_order_acquire);
}

bool IsHighFPSPassthrough()
{
  return s_state.enabled.load(std::memory_order_acquire) &&
         s_state.rate_decision.load(std::memory_order_acquire) == 1;
}

bool RequestEnabled(bool enabled)
{
  std::lock_guard lock{s_state.mutex};
  if (enabled && (!s_state.prepared.load(std::memory_order_acquire) ||
                  !s_state.available.load(std::memory_order_acquire)))
  {
    return false;
  }

  s_state.enabled.store(enabled, std::memory_order_release);
  s_state.status = enabled ? "Measuring the game's native frame rate" :
                             "Available; currently Off";
  return true;
}

std::string GetStatus()
{
  std::lock_guard lock{s_state.mutex};
  return s_state.status;
}
}  // namespace Vulkan::LSFG
