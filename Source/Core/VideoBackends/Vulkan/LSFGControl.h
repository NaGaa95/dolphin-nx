// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace Vulkan::LSFG
{
// Lossless Scaling ships the shaders separately. Dolphin never bundles the proprietary DLL.
constexpr const char* DLL_PATH = "/switch/dolphin/lsfg/Lossless.dll";
constexpr const char* DLL_DISPLAY_PATH = "sdmc:/switch/dolphin/lsfg/Lossless.dll";

bool IsDllInstalled();
bool IsAvailable();
bool IsEnabled();
bool IsHighFPSPassthrough();
bool RequestEnabled(bool enabled);
std::string GetStatus();
}  // namespace Vulkan::LSFG
