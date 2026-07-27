// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "VideoBackends/Vulkan/Constants.h"
#include "VideoBackends/Vulkan/LSFGControl.h"

namespace Vulkan::LSFG
{
// NVK reads NVK_DEBUG during instance creation.
void BeginSession();
void FinishInstanceCreation();
void EndSession();

bool IsSessionPrepared();
void DisableSession(const char* reason);

// Swapchain handles remain owned by Dolphin.
bool RegisterSwapChain(VkSwapchainKHR swapchain, VkExtent2D extent,
                       std::span<const VkImage> images);
void UnregisterSwapChain();

VkResult Present(VkQueue queue, const VkPresentInfoKHR& present_info);

}  // namespace Vulkan::LSFG
