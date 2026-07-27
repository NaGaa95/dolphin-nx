// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/SystemLanguage.h"

#include <switch.h>

#include "Common/Config/Config.h"
#include "Common/ScopeGuard.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"

namespace DolphinSwitch
{
namespace
{
const Config::Info<bool> SWITCH_AUTO_GAMECUBE_LANGUAGE{
    {Config::System::Main, "Core", "SwitchAutoGameCubeLanguage"}, true};
const Config::Info<bool> SWITCH_AUTO_WII_LANGUAGE{
    {Config::System::Main, "Core", "SwitchAutoWiiLanguage"}, true};

SystemLanguageDefaults MapLanguage(SetLanguage language)
{
  using Language = DiscIO::Language;

  switch (language)
  {
  case SetLanguage_JA:
    return {Language::Japanese, 0, "Japanese", "ja-JP"};
  case SetLanguage_ENUS:
    return {Language::English, 0, "English (United States)", "en-US"};
  case SetLanguage_FR:
    return {Language::French, DiscIO::ToGameCubeLanguage(Language::French), "French", "fr-FR"};
  case SetLanguage_DE:
    return {Language::German, DiscIO::ToGameCubeLanguage(Language::German), "German", "de-DE"};
  case SetLanguage_IT:
    return {Language::Italian, DiscIO::ToGameCubeLanguage(Language::Italian), "Italian", "it-IT"};
  case SetLanguage_ES:
    return {Language::Spanish, DiscIO::ToGameCubeLanguage(Language::Spanish), "Spanish", "es-ES"};
  case SetLanguage_ZHCN:
  case SetLanguage_ZHHANS:
    return {Language::SimplifiedChinese, 0, "Chinese (Simplified)", "zh-Hans-CN"};
  case SetLanguage_KO:
    return {Language::Korean, 0, "Korean", "ko-KR"};
  case SetLanguage_NL:
    return {Language::Dutch, DiscIO::ToGameCubeLanguage(Language::Dutch), "Dutch", "nl-NL"};
  case SetLanguage_PT:
    return {Language::English, 0, "Portuguese", "pt-PT"};
  case SetLanguage_RU:
    return {Language::English, 0, "Russian", "ru-RU"};
  case SetLanguage_ZHTW:
  case SetLanguage_ZHHANT:
    return {Language::TraditionalChinese, 0, "Chinese (Traditional)", "zh-Hant-TW"};
  case SetLanguage_ENGB:
    return {Language::English, 0, "English (United Kingdom)", "en-GB"};
  case SetLanguage_FRCA:
    return {Language::French, DiscIO::ToGameCubeLanguage(Language::French), "French (Canada)",
            "fr-CA"};
  case SetLanguage_ES419:
    return {Language::Spanish, DiscIO::ToGameCubeLanguage(Language::Spanish),
            "Spanish (Latin America)", "es-419"};
  case SetLanguage_PTBR:
    return {Language::English, 0, "Portuguese (Brazil)", "pt-BR"};
  case SetLanguage_Total:
    break;
  }

  return {};
}

SystemLanguageDefaults QuerySystemLanguage()
{
  if (R_FAILED(setInitialize()))
    return {};
  Common::ScopeGuard set_guard([] { setExit(); });

  u64 language_code = 0;
  SetLanguage language = SetLanguage_ENUS;
  if (R_FAILED(setGetSystemLanguage(&language_code)) ||
      R_FAILED(setMakeLanguage(language_code, &language)))
  {
    return {};
  }

  return MapLanguage(language);
}
}  // namespace

const SystemLanguageDefaults& GetSystemLanguageDefaults()
{
  static const SystemLanguageDefaults defaults = QuerySystemLanguage();
  return defaults;
}

std::vector<std::string> GetSystemPreferredLocales()
{
  return {GetSystemLanguageDefaults().locale};
}

bool IsGameCubeLanguageAuto()
{
  return Config::GetBase(SWITCH_AUTO_GAMECUBE_LANGUAGE);
}

bool IsWiiLanguageAuto()
{
  return Config::GetBase(SWITCH_AUTO_WII_LANGUAGE);
}

void SetGameCubeLanguageAuto(bool enabled)
{
  Config::SetBase(SWITCH_AUTO_GAMECUBE_LANGUAGE, enabled);
}

void SetWiiLanguageAuto(bool enabled)
{
  Config::SetBase(SWITCH_AUTO_WII_LANGUAGE, enabled);
}

bool ApplyAutoLanguageDefaults()
{
  const SystemLanguageDefaults& defaults = GetSystemLanguageDefaults();
  bool changed = false;

  if (IsGameCubeLanguageAuto() &&
      Config::GetBase(Config::MAIN_GC_LANGUAGE) != defaults.gamecube_language)
  {
    Config::SetBase(Config::MAIN_GC_LANGUAGE, defaults.gamecube_language);
    changed = true;
  }

  const u32 wii_language = static_cast<u32>(defaults.wii_language);
  if (IsWiiLanguageAuto() && Config::GetBase(Config::SYSCONF_LANGUAGE) != wii_language)
  {
    Config::SetBase(Config::SYSCONF_LANGUAGE, wii_language);
    changed = true;
  }

  return changed;
}
}  // namespace DolphinSwitch
