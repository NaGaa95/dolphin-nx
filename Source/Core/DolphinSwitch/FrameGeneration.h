// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace DolphinSwitch::FrameGeneration
{
constexpr const char* DLL_PATH = "/switch/dolphin/lsfg/Lossless.dll";
constexpr const char* DLL_DISPLAY_PATH = "sdmc:/switch/dolphin/lsfg/Lossless.dll";

bool IsDllInstalled();
bool AreDekoShadersPrepared();
bool IsAvailable();
bool IsEnabled();
bool IsHighFPSPassthrough();
// Switch 2: whether to ask for the docked 120 Hz output (nx2DisplaySetFrameGeneration), for the main loop.
bool WantsHighRefresh();
bool RequestEnabled(bool enabled);
std::string GetStatus();

void BeginDekoSession();
void EndDekoSession();
void SetDekoHighFPSPassthrough(bool passthrough);
void DisableDekoSession(const std::string& reason);
}
