// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "DiscIO/Enums.h"

namespace DolphinSwitch
{
struct SystemLanguageDefaults
{
  DiscIO::Language wii_language = DiscIO::Language::English;
  int gamecube_language = 0;
  std::string display_name = "English";
  std::string locale = "en-US";
};

const SystemLanguageDefaults& GetSystemLanguageDefaults();

std::vector<std::string> GetSystemPreferredLocales();

bool IsGameCubeLanguageAuto();
bool IsWiiLanguageAuto();
void SetGameCubeLanguageAuto(bool enabled);
void SetWiiLanguageAuto(bool enabled);

bool ApplyAutoLanguageDefaults();
}  // namespace DolphinSwitch
