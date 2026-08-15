// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace DolphinSwitch
{
struct LauncherLanguage
{
  std::string_view code;
  std::string_view name;
};

enum class LauncherFontFamily
{
  Standard,
  SimplifiedChinese,
  TraditionalChinese,
  Korean,
};

class Localization
{
public:
  bool SetLanguage(std::string_view preference);

  std::string_view Translate(std::string_view source) const;
  std::string_view GetPreference() const { return m_preference; }
  std::string_view GetResolvedCode() const { return m_resolved_code; }
  std::string GetDisplayName() const;
  LauncherFontFamily GetFontFamily() const;

  static std::span<const LauncherLanguage> GetLanguages();
  static int FindLanguage(std::string_view code);

private:
  struct TransparentHash
  {
    using is_transparent = void;

    std::size_t operator()(std::string_view value) const noexcept;
  };

  using TranslationMap =
      std::unordered_map<std::string, std::string, TransparentHash, std::equal_to<>>;

  void LoadMoCatalog(const std::string& path);
  void LoadJsonOverrides(const std::string& path);
  void AddLauncherTranslations();

  TranslationMap m_translations;
  std::string m_preference = "system";
  std::string m_resolved_code = "en";
};
}  // namespace DolphinSwitch
