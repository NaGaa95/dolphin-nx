// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/Launcher.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/IniFile.h"
#include "Common/ScopeGuard.h"
#include "Core/AchievementManager.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/PowerPC/PowerPC.h"
#include "DiscIO/DirectoryBlob.h"
#include "DiscIO/Enums.h"
#include "DolphinSwitch/CoverDownload.h"
#include "DolphinSwitch/DolphinTools.h"
#include "DolphinSwitch/Forwarder.h"
#include "DolphinSwitch/Localization.h"
#include "DolphinSwitch/Storage.h"
#include "DolphinSwitch/SystemLanguage.h"
#include "DolphinSwitch/UiAudio.h"
#include "DolphinSwitch/Updater.h"
#include "InputCommon/ControllerInterface/Switch/Switch.h"
#include "UICommon/GameFile.h"
#include "UICommon/GameFileCache.h"
#include "VideoBackends/Vulkan/LSFGControl.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/VideoConfig.h"

namespace DolphinSwitch
{
namespace
{
constexpr std::string_view DATA_DIRECTORY = "sdmc:/switch/dolphin";
constexpr std::string_view CONFIG_PATH = "sdmc:/switch/dolphin/launcher.ini";
constexpr std::string_view COVER_DIRECTORY = "sdmc:/switch/dolphin/covers";
constexpr std::string_view LSFG_DIRECTORY = "sdmc:/switch/dolphin/lsfg";
constexpr int COVER_CACHE_LIMIT = 64;
constexpr int COVER_REQUEST_BUDGET = 48;
constexpr int COVER_UPLOAD_BUDGET = 2;
constexpr std::size_t COVER_JOB_LIMIT = 96;
constexpr std::size_t COVER_READY_LIMIT = 4;
constexpr std::size_t TEXT_CACHE_LIMIT = 512;
constexpr std::size_t TEXT_CACHE_BYTES = 12 * 1024 * 1024;
constexpr std::size_t METRIC_CACHE_LIMIT = 2048;
constexpr std::size_t ELLIPSIS_CACHE_LIMIT = 512;
constexpr std::size_t BUSY_TASK_STACK_SIZE = 2 * 1024 * 1024;

// SDL's B button is the physical A button on Switch.
constexpr SDL_GameControllerButton BUTTON_CONFIRM = SDL_CONTROLLER_BUTTON_B;
constexpr SDL_GameControllerButton BUTTON_CANCEL = SDL_CONTROLLER_BUTTON_A;
constexpr SDL_GameControllerButton BUTTON_SETTINGS = SDL_CONTROLLER_BUTTON_Y;

struct BusyTaskThreadContext
{
  const std::function<void()>* task = nullptr;
  std::atomic<bool>* complete = nullptr;
  std::atomic_bool* cancel = nullptr;
};

void BusyTaskThreadEntry(void* userdata)
{
  auto* context = static_cast<BusyTaskThreadContext*>(userdata);
  (*context->task)();
  context->complete->store(true, std::memory_order_release);
  SDL_Event wake{};
  wake.type = SDL_USEREVENT;
  wake.user.code = 0x42555359;  // BUSY: cancellable launcher task completed.
  SDL_PushEvent(&wake);
}

std::string Trim(std::string value)
{
  const std::size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const std::size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string Lower(std::string value)
{
  std::ranges::transform(value, value.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool EnsureDirectory(std::string_view path)
{
  const std::string owned(path);
  if (::mkdir(owned.c_str(), 0777) == 0)
    return true;
  if (errno != EEXIST)
    return false;
  struct stat info{};
  return ::stat(owned.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool RegularFileExists(const std::string& path)
{
  struct stat info{};
  return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool QueryRegularFile(const std::string& path, bool* exists)
{
  if (!exists)
    return false;
  struct stat info{};
  if (::stat(path.c_str(), &info) == 0)
  {
    *exists = true;
    return S_ISREG(info.st_mode);
  }
  *exists = false;
  return errno == ENOENT;
}

bool RecoverAtomicFile(const std::string& path)
{
  const std::string temporary = path + ".tmp";
  const std::string backup = path + ".old";
  bool current_exists = false;
  bool backup_exists = false;
  bool temporary_exists = false;
  if (!QueryRegularFile(path, &current_exists) || !QueryRegularFile(backup, &backup_exists) ||
      !QueryRegularFile(temporary, &temporary_exists))
    return false;
  if (!current_exists && backup_exists)
  {
    if (std::rename(backup.c_str(), path.c_str()) != 0)
      return false;
    fsdevCommitDevice("sdmc");
    current_exists = true;
    backup_exists = false;
  }
  if (temporary_exists && std::remove(temporary.c_str()) != 0)
    return false;
  if (current_exists && backup_exists && std::remove(backup.c_str()) != 0)
    return false;
  if (temporary_exists || backup_exists)
    fsdevCommitDevice("sdmc");
  return true;
}

struct ControllerTarget
{
  std::string path;
  std::string section;
  bool inherited = false;
};

std::string ControllerConfigPath(bool wii)
{
  return File::GetUserPath(D_CONFIG_IDX) + (wii ? "WiimoteNew.ini" : "GCPadNew.ini");
}

std::string ControllerSectionName(bool wii, int port)
{
  return std::string(wii ? "Wiimote" : "GCPad") + std::to_string(port + 1);
}

std::string ControllerProfileDirectory(bool wii)
{
  return File::GetUserPath(D_CONFIG_IDX) + "Profiles/" + (wii ? "Wiimote/" : "GCPad/");
}

std::string SwitchControllerDevice(int player)
{
  return "Switch/" + std::to_string(std::clamp(player, 0, 3)) + "/Joypad";
}

using ControllerSectionValues = Common::IniFile::Section::SectionMap;
std::unordered_map<std::string, ControllerSectionValues> s_controller_section_cache;

std::string ControllerSectionCacheKey(const ControllerTarget& target)
{
  return target.path + '\n' + target.section;
}

void InvalidateControllerValueCache(const std::string& path)
{
  std::erase_if(s_controller_section_cache, [&](const auto& entry) {
    return entry.first.starts_with(path) && entry.first.size() > path.size() &&
           entry.first[path.size()] == '\n';
  });
}

void ClearControllerValueCache()
{
  s_controller_section_cache.clear();
}

std::string ReadControllerValue(const ControllerTarget& target, std::string_view key,
                                std::string_view fallback = {})
{
  const std::string cache_key = ControllerSectionCacheKey(target);
  auto iterator = s_controller_section_cache.find(cache_key);
  if (iterator == s_controller_section_cache.end())
  {
    ControllerSectionValues values;
    Common::IniFile ini;
    if (ini.Load(target.path))
    {
      if (const Common::IniFile::Section* section = ini.GetSection(target.section))
        values = section->GetValues();
    }
    iterator = s_controller_section_cache.emplace(cache_key, std::move(values)).first;
  }
  const auto value = iterator->second.find(std::string(key));
  return value == iterator->second.end() ? std::string(fallback) : value->second;
}

bool WriteControllerValue(const ControllerTarget& target, std::string_view key,
                          const std::optional<std::string>& value)
{
  Common::IniFile ini;
  ini.Load(target.path);
  Common::IniFile::Section* section = ini.GetOrCreateSection(target.section);
  if (value)
    section->Set(std::string(key), *value);
  else
    section->Delete(key);
  const bool saved = ini.Save(target.path);
  if (saved)
    InvalidateControllerValueCache(target.path);
  return saved;
}

bool ResetControllerTarget(const ControllerTarget& target)
{
  Common::IniFile ini;
  ini.Load(target.path);
  ini.DeleteSection(target.section);
  const bool saved = ini.Save(target.path);
  if (saved)
    InvalidateControllerValueCache(target.path);
  return saved;
}

bool CopyControllerTarget(const ControllerTarget& source, const ControllerTarget& destination)
{
  Common::IniFile source_ini;
  source_ini.Load(source.path);
  const Common::IniFile::Section* source_section = source_ini.GetSection(source.section);

  Common::IniFile destination_ini;
  destination_ini.Load(destination.path);
  destination_ini.DeleteSection(destination.section);
  Common::IniFile::Section* destination_section =
      destination_ini.GetOrCreateSection(destination.section);
  if (source_section)
  {
    for (const auto& [key, value] : source_section->GetValues())
      destination_section->Set(key, value);
  }
  const bool saved = destination_ini.Save(destination.path);
  if (saved)
    InvalidateControllerValueCache(destination.path);
  return saved;
}

std::string SafeProfileName(std::string_view input)
{
  std::string result;
  result.reserve(input.size());
  for (const unsigned char character : input)
  {
    if (std::isalnum(character) || character == '-' || character == '_')
      result += static_cast<char>(character);
    else if (result.empty() || result.back() != '_')
      result += '_';
  }
  while (!result.empty() && result.back() == '_')
    result.pop_back();
  return result.empty() ? "Profile" : result;
}

std::string ExistingProfileName(std::string_view input)
{
  std::string result = Trim(std::string(input.substr(0, input.find(','))));
  if (result.empty() || result == "." || result == ".." || result.find('/') != std::string::npos ||
      result.find('\\') != std::string::npos || result.find(':') != std::string::npos)
  {
    return {};
  }
  return result;
}

std::string GeneratedControllerProfileName(std::string_view game_key, bool wii, int port)
{
  return "DolphinNX_" + SafeProfileName(game_key) + (wii ? "_Wii" : "_GC") +
         std::to_string(port + 1);
}

std::vector<std::string> ListControllerProfiles(bool wii)
{
  std::vector<std::string> profiles;
  const std::string directory = ControllerProfileDirectory(wii);
  DIR* handle = ::opendir(directory.c_str());
  if (!handle)
    return profiles;
  while (dirent* entry = ::readdir(handle))
  {
    const std::string name = entry->d_name;
    if (name.size() <= 4 || Lower(name.substr(name.size() - 4)) != ".ini")
      continue;
    profiles.push_back(name.substr(0, name.size() - 4));
  }
  ::closedir(handle);
  std::ranges::sort(profiles, {}, [](const std::string& name) { return Lower(name); });
  return profiles;
}

std::string BindingDisplayName(std::string expression)
{
  expression = Trim(std::move(expression));
  if (expression.empty())
    return "Unmapped";
  expression.erase(std::remove(expression.begin(), expression.end(), '`'), expression.end());
  const std::size_t qualifier = expression.rfind(':');
  if (qualifier != std::string::npos)
    expression.erase(0, qualifier + 1);
  return expression;
}

std::optional<std::string> SwitchExpressionForButton(Uint8 button)
{
  switch (button)
  {
  case SDL_CONTROLLER_BUTTON_B:
    return "`A`";
  case SDL_CONTROLLER_BUTTON_A:
    return "`B`";
  case SDL_CONTROLLER_BUTTON_Y:
    return "`X`";
  case SDL_CONTROLLER_BUTTON_X:
    return "`Y`";
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    return "`L`";
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    return "`R`";
  case SDL_CONTROLLER_BUTTON_START:
    return "`Start`";
  case SDL_CONTROLLER_BUTTON_BACK:
    return "`Select`";
  case SDL_CONTROLLER_BUTTON_DPAD_UP:
    return "`Up`";
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
    return "`Down`";
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    return "`Left`";
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    return "`Right`";
  case SDL_CONTROLLER_BUTTON_LEFTSTICK:
    return "`L3`";
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
    return "`R3`";
  default:
    return std::nullopt;
  }
}

std::optional<std::string> SwitchExpressionForAxis(Uint8 axis, Sint16 value)
{
  if (std::abs(static_cast<int>(value)) < 20000)
    return std::nullopt;
  switch (axis)
  {
  case SDL_CONTROLLER_AXIS_LEFTX:
    return value < 0 ? "`X0-`" : "`X0+`";
  case SDL_CONTROLLER_AXIS_LEFTY:
    // Native Switch input uses positive Y for up.
    return value < 0 ? "`Y0+`" : "`Y0-`";
  case SDL_CONTROLLER_AXIS_RIGHTX:
    return value < 0 ? "`X1-`" : "`X1+`";
  case SDL_CONTROLLER_AXIS_RIGHTY:
    return value < 0 ? "`Y1+`" : "`Y1-`";
  case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
    return "`Z`";
  case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
    return "`R2`";
  default:
    return std::nullopt;
  }
}

class Store
{
public:
  bool Load(const std::string& path)
  {
    m_values.clear();
    if (!RecoverAtomicFile(path))
      return false;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
      return false;
    char line[4096];
    while (std::fgets(line, sizeof(line), file))
    {
      std::string text = Trim(line);
      if (text.empty() || text.front() == '#' || text.front() == ';' || text.front() == '[')
        continue;
      const std::size_t separator = text.find('=');
      if (separator == std::string::npos)
        continue;
      std::string key = Trim(text.substr(0, separator));
      if (!key.empty())
        m_values[std::move(key)] = Trim(text.substr(separator + 1));
    }
    return std::fclose(file) == 0;
  }

  bool Save(const std::string& path) const
  {
    if (!RecoverAtomicFile(path))
      return false;
    const std::string temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file)
      return false;
    bool ok = std::fputs("# Dolphin NX SDL launcher\n", file) >= 0;
    for (const auto& [key, value] : m_values)
    {
      if (!ok || std::fprintf(file, "%s = %s\n", key.c_str(), value.c_str()) < 0)
      {
        ok = false;
        break;
      }
    }
    if (std::fflush(file) != 0)
      ok = false;
    if (::fsync(::fileno(file)) != 0)
      ok = false;
    if (std::fclose(file) != 0)
      ok = false;
    if (!ok)
    {
      std::remove(temporary.c_str());
      return false;
    }
    const std::string backup = path + ".old";
    std::remove(backup.c_str());
    const bool had_current = RegularFileExists(path);
    if (had_current && std::rename(path.c_str(), backup.c_str()) != 0)
    {
      std::remove(temporary.c_str());
      return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
      if (had_current)
        std::rename(backup.c_str(), path.c_str());
      std::remove(temporary.c_str());
      return false;
    }
    fsdevCommitDevice("sdmc");
    if (had_current)
    {
      if (std::remove(backup.c_str()) == 0)
        fsdevCommitDevice("sdmc");
    }
    return true;
  }

  std::string Get(std::string_view key, std::string_view fallback = {}) const
  {
    const auto iterator = m_values.find(std::string(key));
    return iterator == m_values.end() ? std::string(fallback) : iterator->second;
  }

  int GetInt(std::string_view key, int fallback) const
  {
    const std::string value = Get(key);
    if (value.empty())
      return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return end != value.c_str() && *end == '\0' ? static_cast<int>(parsed) : fallback;
  }

  bool GetBool(std::string_view key, bool fallback) const
  {
    const std::string value = Lower(Get(key));
    if (value == "true" || value == "1" || value == "yes")
      return true;
    if (value == "false" || value == "0" || value == "no")
      return false;
    return fallback;
  }

  void Set(std::string key, std::string value) { m_values[std::move(key)] = std::move(value); }
  void SetInt(std::string key, int value) { Set(std::move(key), std::to_string(value)); }
  void SetBool(std::string key, bool value) { Set(std::move(key), value ? "true" : "false"); }
  void Remove(std::string_view key) { m_values.erase(std::string(key)); }

  void RemovePrefix(std::string_view prefix)
  {
    for (auto iterator = m_values.begin(); iterator != m_values.end();)
    {
      if (iterator->first.starts_with(prefix))
        iterator = m_values.erase(iterator);
      else
        ++iterator;
    }
  }

private:
  std::map<std::string, std::string> m_values;
};

std::string NormalizePath(std::string path)
{
  path = Trim(path);
  std::ranges::replace(path, '\\', '/');
  const std::size_t colon = path.find(':');
  const std::size_t protected_length = colon == std::string::npos ? 1 : colon + 2;
  for (std::size_t index = 1; index < path.size();)
  {
    if (path[index] == '/' && path[index - 1] == '/')
      path.erase(index, 1);
    else
      ++index;
  }
  while (path.size() > protected_length && path.back() == '/')
    path.pop_back();
  if (colon != std::string::npos && path.size() == colon + 1)
    path += '/';
  return path;
}

std::string JoinPath(const std::string& base, std::string_view child)
{
  if (base.empty())
    return std::string(child);
  return base.back() == '/' ? base + std::string(child) : base + "/" + std::string(child);
}

std::string ParentPath(const std::string& input)
{
  const std::string path = NormalizePath(input);
  const std::size_t colon = path.find(':');
  if (colon == std::string::npos)
  {
    if (path == "/")
      return {};
  }
  else
  {
    bool root = true;
    for (std::size_t index = colon + 1; index < path.size(); ++index)
    {
      if (path[index] != '/')
      {
        root = false;
        break;
      }
    }
    if (root)
      return {};
  }
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return {};
  if (colon == std::string::npos && slash == 0)
    return "/";
  if (colon != std::string::npos && slash <= colon + 1)
    return path.substr(0, colon + 2);
  return path.substr(0, slash);
}

std::string FileName(const std::string& path)
{
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string DeviceName(const std::string& path)
{
  const std::size_t colon = path.find(':');
  return colon == std::string::npos ? std::string{} : Lower(path.substr(0, colon));
}

bool IsUsbStoragePath(const std::string& path)
{
  const std::size_t colon = path.find(':');
  if (colon < 4)
    return false;
  if (std::tolower(static_cast<unsigned char>(path[0])) != 'u' ||
      std::tolower(static_cast<unsigned char>(path[1])) != 'm' ||
      std::tolower(static_cast<unsigned char>(path[2])) != 's')
    return false;
  for (std::size_t index = 3; index < colon; ++index)
  {
    if (!std::isdigit(static_cast<unsigned char>(path[index])))
      return false;
  }
  return true;
}

std::string UnavailableUsbSourcePath(std::string_view id, std::string_view relative)
{
  // A disconnected stable source must not keep its old mutable umsN: alias: another drive can
  // legitimately receive that alias during startup. This non-filesystem placeholder remains
  // unique and is replaced as soon as the bound volume is available again.
  std::string path = "usbsource:" + std::string(id);
  if (!relative.empty())
    path = JoinPath(path, relative);
  return NormalizePath(std::move(path));
}

bool PathAtOrBelow(const std::string& candidate, const std::string& root)
{
  const std::string normalized_candidate = Lower(NormalizePath(candidate));
  const std::string normalized_root = Lower(NormalizePath(root));
  if (normalized_root.empty())
    return false;
  return normalized_candidate == normalized_root ||
         (normalized_candidate.size() > normalized_root.size() &&
          normalized_candidate.starts_with(normalized_root) &&
          (normalized_root.back() == '/' || normalized_candidate[normalized_root.size()] == '/'));
}

bool IsGamePath(std::string_view path)
{
  constexpr std::array<std::string_view, 14> extensions = {".gcm", ".tgc",  ".bin", ".iso", ".ciso",
                                                           ".gcz", ".wbfs", ".wia", ".rvz", ".nfs",
                                                           ".wad", ".dol",  ".elf", ".json"};
  const std::size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos)
    return false;
  const std::string extension = Lower(std::string(path.substr(dot)));
  return std::ranges::contains(extensions, extension);
}

// The generic game-path helper returns one large vector after a complete recursive traversal.
// This Switch-specific walker publishes entries as readdir discovers them and observes
// cancellation between every directory entry. d_type avoids a network stat for normal SMB
// entries; filesystems which report DT_UNKNOWN still get the required correctness fallback.
bool WalkGamePaths(std::string root, const std::atomic_bool& cancel,
                   const std::function<void(std::string)>& found)
{
  root = NormalizePath(std::move(root));
  struct stat root_info{};
  if (::lstat(root.c_str(), &root_info) == 0 && !S_ISDIR(root_info.st_mode))
  {
    if (S_ISREG(root_info.st_mode) && IsGamePath(root))
      found(std::move(root));
    return true;
  }

  std::vector<std::string> pending{std::move(root)};
  while (!pending.empty())
  {
    if (cancel.load(std::memory_order_acquire))
      return false;
    std::string directory_path = std::move(pending.back());
    pending.pop_back();
    DIR* directory = ::opendir(directory_path.c_str());
    if (!directory)
      continue;
    while (dirent* entry = ::readdir(directory))
    {
      if (cancel.load(std::memory_order_acquire))
      {
        ::closedir(directory);
        return false;
      }
      if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
        continue;
      std::string path = JoinPath(directory_path, entry->d_name);
      bool is_directory = entry->d_type == DT_DIR;
      bool is_file = entry->d_type == DT_REG;
      if (entry->d_type == DT_UNKNOWN)
      {
        struct stat info{};
        if (::lstat(path.c_str(), &info) != 0)
          continue;
        is_directory = S_ISDIR(info.st_mode);
        is_file = S_ISREG(info.st_mode);
      }
      if (is_directory)
        pending.emplace_back(std::move(path));
      else if (is_file && IsGamePath(path))
        found(std::move(path));
    }
    ::closedir(directory);
  }
  return true;
}

bool IsFilesystemRoot(const std::string& input)
{
  const std::string path = NormalizePath(input);
  if (path.empty() || path == "/")
    return true;
  const std::size_t colon = path.find(':');
  if (colon == std::string::npos)
    return false;
  return std::ranges::all_of(path.substr(colon + 1), [](char value) { return value == '/'; });
}

bool ValidEntryName(std::string_view name)
{
  if (name.empty() || name == "." || name == ".." || name.size() > 255)
    return false;
  return std::ranges::none_of(name, [](unsigned char value) {
    return value < ' ' || value == '/' || value == '\\' || value == ':';
  });
}

std::string HumanBytes(std::uint64_t value)
{
  constexpr std::array<const char*, 5> units = {"B", "KiB", "MiB", "GiB", "TiB"};
  double amount = static_cast<double>(value);
  std::size_t unit = 0;
  while (amount >= 1024.0 && unit + 1 < units.size())
  {
    amount /= 1024.0;
    ++unit;
  }
  char text[64];
  std::snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.1f %s", amount, units[unit]);
  return text;
}

std::uint64_t HashPath(std::string_view path)
{
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char value : path)
  {
    hash ^= value;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string Hex64(std::uint64_t value)
{
  char text[17]{};
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

std::string StableIdStem(std::string_view game_id, std::string_view fingerprint)
{
  std::string prefix;
  for (const unsigned char character : game_id)
  {
    if (std::isalnum(character))
      prefix += static_cast<char>(std::tolower(character));
  }
  if (prefix.empty())
    prefix = "game";
  return prefix + "-" + Hex64(HashPath(fingerprint));
}

enum class Theme
{
  Xmb,
  Bubbles,
  Glow,
  Classic,
  Oled,
};

enum class SortMode
{
  Alphabetical,
  RecentlyPlayed,
  RecentlyAdded,
};

struct Game
{
  std::shared_ptr<const UICommon::GameFile> metadata;
  std::string path;
  std::string title;
  // Stable launcher identity.  Unlike the legacy key, this never contains the file path and is
  // retained in launcher.ini when an image is renamed or a USB device receives another umsN:.
  std::string key;
  std::string legacy_key;
  std::string fingerprint;
  // A deliberately content-insensitive identity tuple. Patched/repacked images of the same game
  // retain it, while a different title replacing a file at the same path does not inherit state.
  std::string base_identity;
  std::string canonical_path;
  std::string storage_id;
  std::string game_id;
  std::string game_tdb_id;
  std::string platform;
  std::string config_override_path;
  std::uint64_t title_id = 0;
  std::uint16_t revision = 0;
  std::int64_t modified = 0;
  std::int64_t played = 0;
  DiscIO::Region region = DiscIO::Region::Unknown;
  bool has_game_config = false;
  bool has_custom_title = false;
  bool installed_nand = false;
  bool allow_legacy_path_migration = true;
  // True when GameFileCache rebuilt this entry because its source/dependencies changed.  The
  // stable fingerprint is intentionally content-insensitive for normal discs, so this separately
  // invalidates metadata-derived artwork without forcing a full-image hash.
  bool metadata_refreshed = false;
  SDL_Texture* cover = nullptr;
  std::uint64_t cover_use = 0;
  std::uint64_t cover_request = 0;
  Uint32 cover_loaded_at = 0;
  bool cover_attempted = false;
  bool cover_queued = false;
};

struct LibraryIdentityRecord
{
  std::string id;
  std::string fingerprint;
  std::string base_identity;
  std::string canonical_path;
  // Current case-preserving filesystem path.  Forwarders resolve this through the stable record
  // after a rename; canonical_path remains the mount-independent matching key.
  std::string current_path;
  // Exact prior filesystem paths are only used to find forwarders made before stable IDs existed.
  // Keep this bounded: paths may include mutable umsN aliases and are never launch candidates.
  std::vector<std::string> previous_paths;
  // A different title replaced the path formerly owned by this record. Retired records remain
  // available for same-volume fingerprint recovery, but forwarders must never resolve them.
  bool retired = false;
};

constexpr std::size_t MAX_PREVIOUS_LIBRARY_PATHS = 4;

std::string LegacyBaseIdentityFromFingerprint(std::string_view fingerprint)
{
  // Fingerprints written by the first stable-ID implementation were
  // game-id:revision:disc:platform:size:sync-hash.  The prefix is sufficient to migrate those
  // records without treating a patched image as another game.
  std::size_t end = 0;
  for (int separator = 0; separator < 4; ++separator)
  {
    end = fingerprint.find(':', end);
    if (end == std::string_view::npos)
      return {};
    ++end;
  }
  return std::string(fingerprint.substr(0, end - 1));
}

void RememberPreviousLibraryPath(LibraryIdentityRecord* record, std::string_view path)
{
  if (!record)
    return;
  const std::string normalized = NormalizePath(std::string(path));
  if (normalized.empty())
    return;
  std::erase_if(record->previous_paths, [&](const std::string& previous) {
    return Lower(NormalizePath(previous)) == Lower(normalized);
  });
  record->previous_paths.emplace_back(normalized);
  if (record->previous_paths.size() > MAX_PREVIOUS_LIBRARY_PATHS)
    record->previous_paths.erase(record->previous_paths.begin(),
                                 record->previous_paths.end() - MAX_PREVIOUS_LIBRARY_PATHS);
}

std::string LibraryIdentityScope(std::string_view canonical_path)
{
  const std::string canonical = Lower(NormalizePath(std::string(canonical_path)));
  if (canonical.starts_with("usb:") || canonical.starts_with("smb:"))
  {
    const std::size_t slash = canonical.find('/', 4);
    return canonical.substr(0, slash);
  }
  const std::size_t colon = canonical.find(':');
  return colon == std::string::npos ? std::string{} : canonical.substr(0, colon + 1);
}

struct Collection
{
  std::string name;
  std::unordered_set<std::string> members;
};

struct LibraryScanState
{
  std::atomic_bool cancel{false};
  std::atomic_bool complete{false};
  std::mutex mutex;
  std::deque<Game> ready;
  std::atomic<std::size_t> discovered{0};
  std::atomic<std::size_t> processed{0};
  bool full = true;
  bool cache_changed = false;
  std::size_t unsorted_published = 0;
  std::unordered_set<std::string> target_usb_ids;
};

struct CoverDecodeJob
{
  std::string key;
  std::string custom_path;
  std::shared_ptr<const UICommon::GameFile> metadata;
  std::uint64_t request = 0;
  std::uint64_t epoch = 0;
};

struct CoverDecodeResult
{
  std::string key;
  std::uint64_t request = 0;
  std::uint64_t epoch = 0;
  int width = 0;
  int height = 0;
  std::vector<Uint8> pixels;
};

struct SmbAutoMountState
{
  std::atomic_bool cancel{false};
  std::atomic_bool complete{false};
  std::mutex mutex;
  std::deque<std::string> mounted_roots;
};

struct UsbInitializationState
{
  std::atomic_bool complete{false};
  bool success = false;
  std::string error;
};

struct Row
{
  std::string label;
  std::string value;
  bool enabled = true;
  bool destructive = false;
  bool adjustable = true;
  bool localize_label = true;
  bool localize_value = true;
};

struct SettingHelpEntry
{
  std::string_view label;
  std::string_view kind;
  std::string_view description;
};

struct SettingHelpInfo
{
  std::string_view kind;
  std::string description;
};

static constexpr SettingHelpEntry SETTING_HELP[] = {
    {"Launcher", "Settings group",
     "Controls the SDL launcher's theme, game-grid layout, animations and navigation sounds. These "
     "options do not change emulation."},
    {"Language", "Launcher language",
     "Changes the language used by the SDL launcher. System follows the console language. "
     "Translation overrides can be placed in switch/dolphin/i18n on the SD card."},
    {"Library & storage", "Settings group",
     "Manages game folders, save data, installed titles, WAD content, and cover artwork used by "
     "the launcher."},
    {"RetroAchievements", "Online service",
     "Signs in to RetroAchievements and controls achievement, hardcore-mode and progress "
     "notifications for supported game hashes."},
    {"Frame Generation", "Settings group",
     "Configures Vulkan LSFG 2x frame generation. It inserts display frames but does not increase "
     "the emulated game's speed."},
    {"CPU / Emulation", "Settings group",
     "Contains Dolphin's CPU engine, threading, speed, MMU, cache and timing controls."},
    {"Graphics", "Settings group",
     "Contains the Vulkan renderer, internal resolution, presentation, shader compilation, "
     "enhancements and graphics hacks."},
    {"Audio", "Settings group",
     "Controls DSP emulation, output volume and the buffers used to keep audio stable when frame "
     "times vary."},
    {"GameCube & Wii", "Settings group",
     "Configures console language and video flags, Wii system options, the emulated SD card and "
     "GameCube expansion slots."},
    {"Controller / Input", "Settings group",
     "Configures four GameCube ports and four emulated Wii Remotes, including mappings, "
     "extensions, motion and rumble."},
    {"Online & accounts", "Settings group",
     "Configures Wii networking, GameCube Broadband Adapter networking and NAND certificates used "
     "by online services."},
    {"Patches / AR / Gecko / Riivolution", "Settings group",
     "Manages Dolphin patches, Action Replay and Gecko codes, and launches the game with a "
     "Riivolution XML patch set."},

    {"CPU engine", "CPU emulation",
     "Selects how Dolphin executes PowerPC code. JIT ARM64 is the normal high-performance choice; "
     "the interpreters are much slower and are mainly useful for diagnosis."},
    {"Dual Core", "CPU threading",
     "Runs the emulated CPU and GPU work on separate host threads. It usually improves "
     "performance, but a small number of games require it disabled for correct timing."},
    {"Enable cheats", "Cheat engine",
     "Allows enabled Action Replay, Gecko and patch codes to run. Individual codes are selected "
     "from the game's patches and cheats page."},
    {"Fast disc speed", "Disc timing",
     "Removes most emulated optical-drive transfer delays. It can shorten loading, but games that "
     "depend on accurate disc timing may behave incorrectly."},
    {"MMU emulation", "CPU accuracy",
     "Emulates the PowerPC memory-management unit. It is required by a few titles and software "
     "environments, but has a substantial CPU cost."},
    {"Emulation speed", "Speed limit",
     "Sets Dolphin's normal speed target. Lower values slow the whole emulated system; Unlimited "
     "removes the limiter but cannot make a CPU- or GPU-limited game reach full speed."},
    {"Advanced CPU & timing", "Settings group",
     "Opens expert cache, clock and frame-timing controls. Defaults are recommended unless a game "
     "has a documented need."},
    {"Accurate CPU write-back cache", "CPU accuracy",
     "Emulates the PowerPC data cache and locked-cache behavior more accurately. It fixes software "
     "that relies on cache semantics, at a large performance cost."},
    {"Correct time drift", "Timing accuracy",
     "Keeps the emulated time base aligned with elapsed emulation time instead of allowing timing "
     "errors to accumulate."},
    {"Precision frame timing", "Frame pacing",
     "Uses more precise host timing for frame presentation. It can improve pacing, while adding a "
     "small amount of scheduling overhead."},
    {"Rush frame presentation", "Frame pacing",
     "Presents completed frames as early as possible to reduce latency. It can make pacing less "
     "even when performance is unstable."},
    {"Smooth early presentation", "Frame pacing",
     "Smooths early frame presentation to trade a little latency for more consistent pacing."},
    {"Emulated CPU clock override", "CPU clock override",
     "Enables a custom emulated GameCube or Wii CPU clock. This changes guest timing and can fix "
     "or break game logic; it is not a host overclock."},
    {"CPU clock percentage", "CPU clock override",
     "Sets the emulated CPU clock relative to the console default. Lower values can reduce "
     "emulation work but also reduce a game's internal performance."},
    {"VBI frequency override", "Video timing override",
     "Enables a custom vertical-blank frequency. It changes the rate at which the emulated console "
     "advances video timing and can affect gameplay speed."},
    {"VBI frequency percentage", "Video timing override",
     "Sets vertical-blank frequency relative to the game's normal rate. Use 100% unless testing a "
     "title-specific timing adjustment."},

    {"Video backend", "Graphics backend",
     "Selects the Mesa graphics driver used for the next game. Vulkan (NVK) is the default; "
     "OpenGL can use the native NVC0 driver or Zink over NVK, globally or per game."},
    {"GLThread", "OpenGL command threading",
     "Lets Mesa build OpenGL command streams on a worker thread. It can improve CPU-limited "
     "games, but may reduce performance or expose compatibility issues in others. This setting "
     "only affects the OpenGL backend and can be selected per game."},
    {"Internal resolution", "Resolution / performance",
     "Sets the resolution used for 3D rendering. Higher values improve clarity but increase GPU "
     "load and memory use; 1x is the original console resolution."},
    {"Aspect ratio", "Display geometry",
     "Chooses how Dolphin determines the final image shape. Auto follows the game, forced modes "
     "override it, and Stretch fills the display by distorting the image."},
    {"VSync", "Presentation",
     "Synchronizes frame presentation to the Switch display to prevent tearing. It can add latency "
     "or reduce performance headroom when emulation cannot keep up."},
    {"Shader compilation", "Shader stutter / accuracy",
     "Controls how Dolphin handles new graphics pipelines. Asynchronous modes reduce compilation "
     "stutter; synchronous modes avoid missing effects while a shader is being built."},
    {"Wait for shaders before starting", "Shader compilation",
     "Builds known shaders before gameplay begins. Startup takes longer, but fewer shaders need to "
     "compile during the first minutes of play."},
    {"Crop to aspect ratio", "Display geometry",
     "Crops pixels outside the selected aspect ratio instead of showing overscan or unused "
     "borders."},
    {"Show FPS", "Performance display",
     "Shows Dolphin's frame-rate overlay during gameplay. The same display can be toggled from the "
     "in-game overlay."},
    {"Enhancements", "Settings group",
     "Opens resolution-independent visual enhancements, filtering, color correction and "
     "stereoscopic options."},
    {"Hacks", "Settings group",
     "Opens graphics workarounds that trade accuracy for performance. Dolphin's defaults are "
     "recommended for most games."},

    {"LSFG 2x (Vulkan only)", "Frame generation",
     "Makes LSFG 2x available for the launch. It creates an intermediate display frame between "
     "rendered frames, adding smoothness without increasing emulation speed."},
    {"Flow resolution", "Frame generation quality",
     "Sets the optical-flow working resolution. Half can retain more motion detail, while Quarter "
     "uses less GPU time and memory and is recommended on Switch."},
    {"Performance mode", "Frame generation performance",
     "Uses LSFG's lighter performance-oriented path. Disable it only when testing quality with "
     "enough GPU headroom."},
    {"Lossless.dll", "Required component",
     "Shows whether the LSFG runtime is installed in Dolphin's frame-generation folder. Frame "
     "generation cannot start while it is missing."},

    {"Anti-aliasing", "Image quality / performance",
     "Smooths polygon edges. MSAA increases GPU and memory cost; SSAA is substantially more "
     "expensive because it supersamples the rendered image."},
    {"Texture filtering", "Texture filtering",
     "Controls anisotropic and forced texture filtering. Higher anisotropy sharpens angled "
     "surfaces; forcing filtering can blur 2D artwork or alter effects."},
    {"Output resampling", "Output scaling",
     "Selects the filter used when Dolphin scales the final image to the display. Sharper filters "
     "may emphasize aliasing; softer filters can reduce shimmer."},
    {"Post-processing effect", "Post-processing",
     "Applies a shader to the final image after emulation rendering. Effects can change color or "
     "sharpness and add GPU work."},
    {"Scaled EFB copy", "Visual enhancement",
     "Creates EFB copies at the internal resolution instead of native resolution. This improves "
     "many effects at higher resolutions but can break effects that expect exact native copies."},
    {"Per-pixel lighting", "Visual enhancement",
     "Calculates lighting per pixel instead of approximating it per vertex. It can improve some "
     "scenes but may alter a game's intended lighting and costs GPU time."},
    {"Widescreen hack", "Display enhancement",
     "Expands the 3D projection for widescreen without a game patch. It can reveal objects outside "
     "the intended view and does not fix 2D elements."},
    {"Disable fog", "Visual modification",
     "Removes emulated fog. This may make distant scenes clearer, but changes the intended image "
     "and can break effects that use fog creatively."},
    {"Force 24-bit color", "Color accuracy",
     "Uses 24-bit color for EFB output to reduce banding. It can differ from console behavior and "
     "adds some GPU or memory cost."},
    {"Disable copy filter", "Visual enhancement",
     "Disables the console's copy filter, often producing a sharper image. Some games use the "
     "filter for intentional smoothing or effects."},
    {"Arbitrary mipmap detection", "Texture enhancement",
     "Detects custom mip levels generated in unusual ways so higher-resolution rendering can "
     "preserve them. Detection adds overhead and may misidentify textures."},
    {"Correct color space", "Color correction",
     "Converts the console's output color space more accurately for a modern display. It changes "
     "final colors but not emulated lighting."},
    {"Game color space", "Color correction",
     "Selects the color-space interpretation used for the game's output when color correction is "
     "enabled."},
    {"Correct SDR gamma", "Gamma correction",
     "Applies gamma correction appropriate for SDR output instead of passing the game's encoded "
     "values directly."},
    {"Game gamma", "Gamma correction",
     "Sets the gamma curve assumed for the game's output. The default follows Dolphin's normal "
     "console-color interpretation."},
    {"Display gamma", "Gamma correction",
     "Selects the target display gamma used when converting the game's image for the Switch "
     "screen."},
    {"Custom display gamma", "Gamma correction",
     "Sets a manual target gamma when the custom display-gamma option is selected."},
    {"HDR post-processing", "HDR output",
     "Enables Dolphin's HDR post-processing path. It is useful only with a compatible HDR output "
     "chain and adds GPU work."},
    {"HDR paper white", "HDR output",
     "Sets the reference brightness used for SDR-white content in the HDR conversion."},
    {"Stereoscopic 3D mode", "Stereoscopic rendering",
     "Selects a stereoscopic output mode. Rendering two views increases GPU work and requires a "
     "compatible viewing method."},
    {"Stereoscopic depth", "Stereoscopic rendering",
     "Sets the separation between left- and right-eye views. Excessive depth can be uncomfortable "
     "or expose rendering outside the intended view."},
    {"Stereoscopic convergence", "Stereoscopic rendering",
     "Moves the plane where left- and right-eye images meet, changing which objects appear in "
     "front of or behind the display."},
    {"Swap stereo eyes", "Stereoscopic rendering",
     "Exchanges the left- and right-eye images when the selected display method presents them in "
     "the opposite order."},
    {"Full resolution per eye", "Stereoscopic rendering",
     "Renders each stereoscopic eye at full internal resolution. It improves clarity but "
     "approximately doubles relevant GPU and memory work."},

    {"Skip EFB access from CPU", "Graphics hack",
     "Ignores CPU reads and writes to the embedded frame buffer. This is faster, but games using "
     "EFB access for effects, visibility or gameplay can break."},
    {"Ignore EFB format changes", "Graphics hack",
     "Keeps the current EFB pixel format when a game requests a change. It can avoid costly "
     "conversions but may produce incorrect colors or effects."},
    {"Store EFB copies to texture only", "Graphics hack",
     "Keeps EFB copies on the GPU instead of copying them to emulated RAM. This is much faster, "
     "but games that read those copies with the CPU may break."},
    {"Defer EFB copies to RAM", "Graphics hack",
     "Delays EFB copies until the emulated CPU actually needs them. This reduces synchronization "
     "but can be less accurate for unusual access patterns."},
    {"Deferred EFB-access invalidation", "Graphics synchronization",
     "Defers invalidation caused by EFB CPU access so several accesses can share one "
     "synchronization point. It normally improves performance with high compatibility."},
    {"GPU texture decoding", "CPU / GPU trade-off",
     "Decodes supported console texture formats on the GPU. It can reduce CPU work, while adding "
     "GPU work and requiring backend support."},
    {"Texture cache accuracy", "Graphics accuracy / performance",
     "Controls how often Dolphin checks emulated memory for changed textures. Fast is the normal "
     "default; higher accuracy fixes unusual updates at additional CPU cost."},
    {"Store XFB copies to texture only", "Graphics hack",
     "Keeps external frame-buffer copies on the GPU. It is faster, but software that reads or "
     "modifies XFB data in RAM can display incorrectly."},
    {"Immediately present XFB", "Presentation hack",
     "Presents a newly written XFB without waiting for normal video timing. It can reduce latency "
     "in some games but may cause pacing or duplicate-frame issues."},
    {"Skip presenting duplicate frames", "Presentation performance",
     "Avoids presenting identical XFB frames. Emulation still runs normally; this can reduce "
     "presentation work for 25 or 30 FPS games."},
    {"Fast depth calculation", "Graphics hack",
     "Uses a faster approximation for depth values. It is normally safe, but disabling it can fix "
     "depth precision or layering problems."},
    {"Disable bounding box", "Graphics hack",
     "Skips bounding-box emulation. This saves synchronization work, but games that use "
     "bounding-box results for effects or gameplay may break."},
    {"Vertex rounding", "Rendering workaround",
     "Rounds projected vertex positions to reduce gaps and shaking at higher internal resolutions. "
     "It can change geometry placement in some games."},
    {"Save texture cache to state", "Save-state behavior",
     "Includes more texture-cache data in save states. States become larger, but visual "
     "restoration after loading can be more accurate."},
    {"VBI skip", "Performance hack",
     "Skips selected vertical-blank work when emulation falls behind. It can improve apparent "
     "speed but may cause timing, audio or gameplay problems."},
    {"Manual texture sampling", "Graphics accuracy",
     "Emulates console texture sampling explicitly in shaders. It can fix edge and filtering "
     "behavior, with a significant GPU cost."},

    {"Volume", "Audio output",
     "Sets Dolphin's final output volume. It does not change a game's own sound settings."},
    {"DSP emulation", "Audio emulation",
     "Selects how Dolphin emulates the console DSP. HLE is faster; LLE is more accurate and can be "
     "required by unusual audio software."},
    {"Audio latency", "Audio latency / stability",
     "Sets the target audio latency. Lower values respond faster but are more likely to crackle "
     "when emulation frame times fluctuate."},
    {"Audio buffer size", "Audio latency / stability",
     "Sets the number of samples buffered by Dolphin's audio mixer. Larger buffers tolerate stalls "
     "better but increase audible delay."},
    {"Fill audio gaps", "Audio stability",
     "Synthesizes short missing sections when audio production falls behind, reducing sharp pops "
     "at the cost of exact output."},
    {"Preserve pitch", "Audio processing",
     "Keeps audio pitch near normal when emulation speed changes. The time-stretch processing adds "
     "a small amount of CPU work and latency."},
    {"Mute when disabling speed limit", "Audio behavior",
     "Mutes audio while Dolphin runs without the normal speed limiter, avoiding very fast or "
     "unstable sound."},

    {"GameCube language", "Console language",
     "Sets the language exposed to GameCube software. Auto follows the Switch user language when "
     "supported and otherwise falls back to English."},
    {"Wii language", "Console language",
     "Sets the Wii system language in SYSCONF. Auto follows the Switch user language when "
     "supported and otherwise falls back to English."},
    {"Wii widescreen", "Console video setting",
     "Sets the Wii's 16:9 system flag. Games that support widescreen use it to choose their own "
     "layout; it does not force unsupported games widescreen."},
    {"Progressive scan", "Console video setting",
     "Enables the console's progressive-scan system flag for software that supports 480p output."},
    {"PAL60", "Console video setting",
     "Enables 60 Hz output for compatible PAL software instead of the normal 50 Hz PAL mode."},
    {"Wii system settings", "Settings group",
     "Opens Wii sound, SD card, sensor-bar, Wii Remote speaker and rumble settings."},
    {"GameCube Slot A / B", "Settings group",
     "Opens the GameCube IPL and expansion-interface device configuration for slots A and B."},
    {"Sound mode", "Wii system audio",
     "Sets the Wii system sound mode reported to games: mono, stereo or surround."},
    {"Insert SD card", "Wii SD card",
     "Inserts Dolphin's emulated SD or SDHC card into the Wii. Games and the Wii Menu can access "
     "it as removable storage."},
    {"Allow writes to SD card", "Wii SD card",
     "Allows Wii software to modify the emulated SD card. Disable it to make the card effectively "
     "read-only."},
    {"SD card image path", "Wii SD card",
     "Selects a raw SD-card image. Leaving it on Dolphin default uses the standard file in the "
     "user directory."},
    {"Automatically sync SD folder", "Wii SD card",
     "Synchronizes a normal host folder with the emulated SD image so files can be managed without "
     "editing the image directly."},
    {"SD sync folder", "Wii SD card",
     "Selects the host folder used by automatic SD-card synchronization."},
    {"SD card file size", "Wii SD card",
     "Sets the capacity of a newly created emulated SD image. Cards above 2 GiB use SDHC "
     "behavior."},
    {"Sensor bar position", "Wii input",
     "Reports whether the emulated sensor bar is above or below the display, matching the Wii "
     "system setting used by pointer calculations."},
    {"IR sensitivity", "Wii input",
     "Sets the Wii sensor-bar sensitivity level reported to software. This is separate from "
     "Dolphin's gyro-pointer sensitivity."},
    {"Enable Wii Remote speaker", "Wii Remote audio",
     "Allows games to send sound to the emulated Wii Remote speaker and routes it through Switch "
     "audio output."},
    {"Wii Remote speaker volume", "Wii Remote audio",
     "Sets the emulated Wii Remote speaker volume stored in Wii system configuration."},
    {"Enable Wii Remote rumble", "Wii input",
     "Allows Wii software to request vibration from emulated Wii Remotes."},

    {"Skip GameCube Main Menu", "GameCube IPL",
     "Boots games directly instead of entering the GameCube IPL menu. If a matching IPL ROM is "
     "unavailable, Dolphin falls back to direct boot."},
    {"Slot A", "GameCube expansion slot",
     "Opens the device attached to GameCube expansion slot A, normally a memory card or GCI "
     "folder."},
    {"Slot B", "GameCube expansion slot",
     "Opens the device attached to GameCube expansion slot B."},
    {"Device", "GameCube expansion device",
     "Selects the emulated EXI device in this slot, such as a memory card, GCI folder, USB Gecko, "
     "Advance Game Port or microphone."},
    {"Memory card path", "GameCube save storage",
     "Selects the raw GameCube memory-card file used by this slot. Dolphin's default is "
     "region-aware."},
    {"GCI folder path", "GameCube save storage",
     "Selects a folder where each GameCube save is stored as an individual GCI file."},
    {"GBA cartridge path", "GameCube peripheral",
     "Selects the GBA cartridge image exposed through the emulated Advance Game Port device."},

    {"Emulated device", "GameCube controller",
     "Selects the GameCube peripheral presented to the game on this port: standard controller, "
     "steering wheel, dance mat or DK Bongos."},
    {"Switch player", "Physical controller",
     "Chooses which connected Switch controller supplies input to this emulated controller port."},
    {"Joy-Con layout", "Physical controller",
     "Selects Dual Joy-Con, single left, single right or automatic handling for the assigned "
     "Switch player."},
    {"Interactive mapping", "Controller mapping",
     "Opens Dolphin's press-to-bind screen for GameCube buttons, sticks and triggers."},
    {"Wii Remote mapping", "Controller mapping",
     "Opens the press-to-bind screen for the emulated Wii Remote buttons and pointer controls."},
    {"Extension mapping", "Controller mapping",
     "Opens mappings for the selected Wii Remote extension, such as Nunchuk or Classic "
     "Controller."},
    {"Motion & pointer", "Motion input",
     "Opens orientation, MotionPlus, gyroscope pointer, sensitivity and calibration controls."},
    {"Orientation hotkeys", "Wii Remote orientation",
     "Opens hold and toggle bindings that temporarily reverse the Sideways or Upright Wii Remote "
     "orientation."},
    {"Extension", "Wii Remote extension",
     "Selects the extension attached to the emulated Wii Remote. Choose None unless the game "
     "expects a specific accessory."},
    {"Rumble strength", "Controller feedback",
     "Sets how strongly emulated rumble is sent to the assigned Switch controller. Zero disables "
     "vibration for this port."},
    {"Control Stick dead zone", "Analog input",
     "Ignores small movement around the GameCube Control Stick center. Raise it only enough to "
     "hide physical stick drift."},
    {"C-Stick dead zone", "Analog input",
     "Ignores small movement around the GameCube C-Stick center."},
    {"Trigger dead zone", "Analog input",
     "Sets how far an analog trigger must move before Dolphin begins reporting pressure."},
    {"Extension stick dead zone", "Analog input",
     "Sets the center dead zone for the selected Nunchuk or Classic Controller stick."},
    {"Profiles", "Controller profiles",
     "Loads or saves reusable Dolphin controller mappings for this emulated port."},
    {"Active profile", "Controller profiles",
     "Shows whether this port uses its current mapping, a saved profile or the global mapping."},
    {"Load profile", "Controller profiles",
     "Loads a previously saved Dolphin controller profile into this port or assigns it to this "
     "game."},
    {"Save as profile", "Controller profiles",
     "Saves the current port mapping as a reusable Dolphin controller profile."},
    {"Use global mapping", "Per-game controller override",
     "Removes this game's controller-profile override so the port follows the global mapping "
     "again."},
    {"Reset Switch defaults", "Controller mapping",
     "Restores Dolphin NX's default Switch bindings for this controller port only."},
    {"Reset game mapping", "Per-game controller override",
     "Removes this game's mapping override and returns the port to its global controller profile."},
    {"Sideways Wii Remote", "Wii Remote orientation",
     "Rotates the emulated Wii Remote for games designed to hold it sideways."},
    {"Upright Wii Remote", "Wii Remote orientation",
     "Forces the emulated Wii Remote to be treated as upright."},
    {"Attach MotionPlus", "Motion input",
     "Attaches the emulated Wii MotionPlus gyroscope used by games that require or support it."},
    {"Gyro pointer", "Motion input",
     "Uses Switch gyroscope motion to control the emulated Wii infrared pointer."},
    {"Touchscreen Wii pointer", "Pointer input",
     "Uses the Switch touchscreen as an absolute Wii Remote pointer in handheld mode. Touches "
     "outside the rendered game area are ignored."},
    {"Pointer sensitivity", "Motion input",
     "Sets how far the on-screen pointer moves for a given controller rotation."},
    {"Gyro dead zone", "Motion input",
     "Ignores very small gyroscope rates to reduce cursor drift while the controller is still."},
    {"Auto-calibration", "Motion calibration",
     "Sets how long the controller must remain still before Dolphin updates its gyroscope bias "
     "estimate."},
    {"Accelerometer influence", "Motion input",
     "Controls how strongly accelerometer orientation corrects the gyro-based pointer. Higher "
     "values resist long-term drift but can react to movement."},
    {"Right-stick pointer dead zone", "Pointer input",
     "Sets the center dead zone when the right stick contributes to Wii pointer movement."},
    {"IR relative input", "Pointer input",
     "Makes the right stick move the Wii pointer from its current position instead of directly "
     "selecting an absolute position."},
    {"IR total yaw", "Pointer range",
     "Sets the emulated horizontal Wii Remote rotation covered by the full right-stick pointer "
     "range."},
    {"IR total pitch", "Pointer range",
     "Sets the emulated vertical Wii Remote rotation covered by the full right-stick pointer "
     "range."},
    {"Pointer recenter", "Controller mapping",
     "Assigns the input used to recenter the gyro pointer's yaw and pitch during play."},
    {"Calibration guide", "Motion calibration",
     "Shows the recommended procedure for learning gyroscope bias and keeping pointer input "
     "stable."},

    {"Download Gecko codes", "Game modification",
     "Downloads available Gecko codes for this game from the configured online database and merges "
     "new entries into Dolphin's game configuration."},
    {"Launch with Riivolution XML", "Game modification",
     "Chooses a Riivolution XML patch definition and launches the game with its replacement files "
     "and options."},
    {"Cheat engine", "Cheat engine",
     "Enables or disables Dolphin's patch, Action Replay and Gecko execution for this game."},

    {"Theme", "Launcher appearance",
     "Changes the launcher's background and visual style. It has no effect on Dolphin's in-game "
     "renderer or performance."},
    {"Games per row", "Library layout",
     "Sets how many game covers appear across each library row. More columns make every cover "
     "smaller."},
    {"Rows per page", "Library layout",
     "Sets how many cover rows are visible on one library page."},
    {"Show game titles", "Library layout",
     "Shows or hides game names below cover artwork in the launcher library."},
    {"Show region flags", "Library layout",
     "Shows or hides the region flag in the top-left corner of each game cover."},
    {"Show custom settings badges", "Library layout",
     "Shows or hides the square badge on games that have per-game settings. The settings "
     "themselves are not changed."},
    {"UI animations", "Launcher appearance",
     "Enables launcher transitions, animated highlights and moving theme elements."},
    {"Sound effects", "Launcher audio",
     "Enables SDL launcher navigation, confirmation and back sound effects."},
    {"SteamGridDB API key", "Artwork service",
     "Edits the API key used to search and download SteamGridDB covers and shortcut icons. Leave "
     "it blank to remove the saved key."},
    {"Download from SteamGridDB", "Artwork service",
     "Searches SteamGridDB for this game and replaces its custom cover with the selected online "
     "artwork."},
    {"Import cover from file", "Local artwork",
     "Imports a PNG, JPEG, WebP or BMP image from SD, USB or SMB storage and stores it as this "
     "game's custom cover."},
    {"Remove custom cover", "Artwork management",
     "Deletes this game's custom cover. Dolphin falls back to embedded game artwork when "
     "available."},
};

SettingHelpInfo SettingHelpFor(std::string_view title, const Row& row)
{
  for (const SettingHelpEntry& entry : SETTING_HELP)
  {
    if (entry.label == row.label)
      return {entry.kind, std::string(entry.description)};
  }

  if (row.label.starts_with("GameCube controller "))
    return {"Controller port",
            "Opens this GameCube controller port and shows its emulated device, assigned Switch "
            "player, mappings, dead zones, rumble and profiles."};
  if (row.label.starts_with("Wii Remote "))
    return {"Controller port", "Opens this emulated Wii Remote and shows its assigned Switch "
                               "player, extension, mappings, motion, rumble and profiles."};
  if (row.label.starts_with("Patch ") || row.label.starts_with("AR ") ||
      row.label.starts_with("Gecko "))
    return {"Game modification", "Enables or disables this game-specific code. Its effect is "
                                 "defined by the patch, Action Replay or Gecko entry."};
  if (row.value == ">")
    return {"Settings group", "Opens this group of Dolphin settings."};
  if (!row.adjustable)
    return {"Management action",
            "Opens this Dolphin management action or a file-selection screen."};
  if (!title.empty())
    return {"Dolphin setting", "Changes this Dolphin option. Keep the default value when "
                               "troubleshooting an unexpected game-specific problem."};
  return {"Setting", "Changes this launcher option."};
}

std::string_view SettingScope(std::string_view title, std::string_view context)
{
  if (title.starts_with("Game ") || title == "Patches, cheats & Riivolution" ||
      title == "Cover settings")
    return "Per-game setting";
  if (!context.empty() &&
      (title.starts_with("GameCube controller ") || title.starts_with("Wii Remote ")))
    return "Per-game setting";
  return "Global setting";
}

struct InputBinding
{
  std::string label;
  std::string key;
  std::string default_expression;
};

struct TransferState
{
  std::atomic<std::uint64_t> total{0};
  std::atomic<std::uint64_t> done{0};
  std::atomic<bool> cancelled{false};
  std::atomic<bool> destination_created{false};
  std::string current;
  std::string error;
  std::vector<unsigned char> buffer = std::vector<unsigned char>(256 * 1024);
  std::mutex detail_mutex;
};

void SetTransferDetail(TransferState* state, const std::string& current,
                       const std::string& error = {})
{
  if (!state)
    return;
  std::lock_guard lock(state->detail_mutex);
  if (!current.empty())
    state->current = current;
  if (!error.empty())
    state->error = error;
}

void UsbStatusWake(void*)
{
  SDL_Event wake{};
  wake.type = SDL_USEREVENT;
  wake.user.code = 0x55534248;  // USBH: USB hotplug state changed.
  SDL_PushEvent(&wake);
}

std::string TransferError(TransferState* state)
{
  if (!state)
    return {};
  std::lock_guard lock(state->detail_mutex);
  return state->error;
}

struct TextKey
{
  TTF_Font* font = nullptr;
  Uint32 color = 0;
  std::string text;
  bool operator==(const TextKey&) const = default;
};

struct TextKeyHash
{
  std::size_t operator()(const TextKey& key) const
  {
    std::size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font*>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<Uint32>{}(key.color) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct TextTexture
{
  SDL_Texture* texture = nullptr;
  int width = 0;
  int height = 0;
  std::size_t bytes = 0;
  std::uint64_t use = 0;
};

struct MetricKey
{
  TTF_Font* font = nullptr;
  std::string text;
  bool operator==(const MetricKey&) const = default;
};

struct MetricKeyHash
{
  std::size_t operator()(const MetricKey& key) const
  {
    std::size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font*>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct MetricEntry
{
  int width = 0;
  std::uint64_t use = 0;
};

struct EllipsisKey
{
  TTF_Font* font = nullptr;
  int max_width = 0;
  std::string text;
  bool operator==(const EllipsisKey&) const = default;
};

struct EllipsisKeyHash
{
  std::size_t operator()(const EllipsisKey& key) const
  {
    std::size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font*>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<int>{}(key.max_width) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct EllipsisEntry
{
  std::string text;
  std::uint64_t use = 0;
};

enum class TouchKind
{
  None,
  Tap,
  SwipeLeft,
  SwipeRight,
  ScrollUp,
  ScrollDown,
};

struct TouchGesture
{
  bool active = false;
  bool vertical = false;
  SDL_FingerID finger = 0;
  float start_x = 0.0f;
  float start_y = 0.0f;
  float last_y = 0.0f;
  Uint32 started_at = 0;
};

class Launcher
{
public:
  Launcher(std::string startup_message, std::string launcher_path)
      : m_startup_message(std::move(startup_message)), m_launcher_path(std::move(launcher_path))
  {
  }
  std::optional<LaunchRequest> Run();
  bool RunAppletInstaller();
  ~Launcher();

private:
  bool Initialize(bool applet_installer = false);
  void Shutdown();
  bool ConfirmApplicationExit();
  void PrepareApplicationExit();
  void LoadDefaults();
  void MarkConfigDirty();
  void MarkStoreDirty();
  void FlushPendingSaves();
  bool LoadFonts();
  void ClearTextCaches();
  void ApplyAppearance();
  void LoadSourcesAndShares();
  void SaveSources();
  void SaveShares();
  void StartAutoMountShares();
  void StopAutoMountShares();
  void PumpAutoMountShares();
  void StartUsbInitialization();
  void StopUsbInitialization();
  void PumpUsbInitialization();
  bool RefreshConfiguredUsbSources();
  void LoadLibraryIdentities();
  void SaveLibraryIdentities();
  std::string CanonicalLibraryPath(std::string_view path) const;
  bool LibraryIdentityPathExists(const LibraryIdentityRecord& record) const;
  std::string GameFingerprint(const UICommon::GameFile& metadata) const;
  std::string GameBaseIdentity(const UICommon::GameFile& metadata) const;
  void AssignStableIdentity(Game* game);
  void MigrateLegacyGameState(Game* game);
  void LoadLibraryOrganization();
  void SaveCollections();
  void RebuildVisibleGames();
  Game* VisibleGame(int index);
  void StartGameScan(std::vector<std::string> sources, bool replace);
  void StopGameScan();
  void PumpGameScan();
  void ScanGames();
  [[maybe_unused]] void ScanGamesLegacy();
  void SortGames();

  void ClearBackground();
  void DrawBubbles(float time);
  void DrawXmb(float time);
  void DrawXmbRibbon(float time, float center, float amplitude, float frequency, float slope,
                     float phase, int half_width, SDL_Color color);
  void DrawXmbFilament(float time, float center, float amplitude, float frequency, float slope,
                       float phase, SDL_Color color);
  void DrawXmbSparkles(float time);
  void EnsureGlowTexture();
  bool HasAnimatedBackground() const;
  void FillRect(int x, int y, int width, int height, SDL_Color color);
  void Border(int x, int y, int width, int height, int thickness, SDL_Color color);
  void FillCircle(int center_x, int center_y, int radius, SDL_Color color);
  void GlassPanel(int x, int y, int width, int height);
  SDL_Texture* MakeGlyph(std::string_view label, bool pill);
  SDL_Texture* ButtonGlyph(std::string_view button) const;
  SDL_Texture* MakeFlagTexture(DiscIO::Region region, int width, int height);
  void InitializeUiTextures();
  void DestroyUiTextures();
  void DrawText(TTF_Font* font, int x, int y, std::string_view text, SDL_Color color);
  void DrawTextCentered(TTF_Font* font, int center_x, int y, std::string_view text,
                        SDL_Color color);
  void DrawTextRight(TTF_Font* font, int right_x, int y, std::string_view text, SDL_Color color);
  int TextWidth(TTF_Font* font, std::string_view text);
  std::string Ellipsize(TTF_Font* font, std::string_view text, int max_width);
  void DrawScrollingTextLeft(TTF_Font* font, int x, int y, int max_width, std::string_view text,
                             SDL_Color color);
  void DrawScrollingTextRight(TTF_Font* font, int right_x, int y, int max_width,
                              std::string_view text, SDL_Color color);
  std::vector<std::string> WrapText(TTF_Font* font, int max_width, int max_lines,
                                    std::string_view text);
  void DrawWrapped(TTF_Font* font, int x, int y, int max_width, int line_height, int max_lines,
                   std::string_view text, SDL_Color color);
  void DrawWrappedCentered(TTF_Font* font, int center_x, int y, int max_width, int line_height,
                           int max_lines, std::string_view text, SDL_Color color);
  void DrawTitleCell(int center_x, int width, int y, const Game& game, bool selected,
                     SDL_Color color);
  void DrawHeader(std::string_view title, std::string_view context = {});
  void DrawFooter(std::span<const std::pair<std::string_view, std::string_view>> hints,
                  int center_y = -1);
  int FooterHitTest(int x, int y) const;
  void DrawButtonHint(int x, int y, std::string_view button, std::string_view label);
  void DrawSettingsFooter(std::string_view text);
  void BeginScreenFx();
  void DrawFadeIn();
  void ShowInfoCard(std::string_view section, std::string_view title, std::string_view kind,
                    std::string_view description, std::string_view current, std::string_view scope,
                    bool localize_title = true, bool localize_current = false);
  void RenderMessage(std::string_view title, std::span<const std::string> lines,
                     bool localize_lines = false);
  void Toast(std::string message, int milliseconds = 900);
  bool Confirm(std::string_view title, std::span<const std::string> lines,
               bool localize_lines = false);
  int Dropdown(std::string_view title, const std::vector<std::string>& choices, int current,
               bool localize_title = true, bool localize_choices = true);
  int SelectChoice(std::string_view title, std::span<const std::string_view> choices, int current,
                   int delta);
  bool PromptText(std::string_view header, std::string_view initial, std::string* output,
                  bool password = false, bool allow_empty = false, std::string_view subtext = {},
                  std::string_view guide = {});

  bool BeginFrame();
  bool PollEvent(SDL_Event* event);
  void WaitForNextFrame(bool force_animation = false);
  bool FrameNeedsAnimation();
  TouchKind FeedTouch(const SDL_Event& event, int* x, int* y);
  bool TouchScrollList(TouchKind kind, int* selection, int* top, int count, int visible);
  int EventNavigation(const SDL_Event& event) const;
  void QueueNavigationRepeat();
  int RunRows(std::string_view title, std::string_view context,
              const std::function<std::vector<Row>()>& rows,
              const std::function<bool(int, int)>& action, bool touch_activates_full_row = false,
              std::function<bool(int)> reset = {}, std::function<bool(int)> resettable = {});

  template <typename T>
  void ResetConfigSetting(const Config::Info<T>& setting)
  {
    Config::SetBase(setting, setting.GetDefaultValue());
    MarkConfigDirty();
  }

  CoverDecodeResult DecodeCover(const CoverDecodeJob& job);
  void CoverDecodeThread();
  void StartCoverDecodeWorker();
  void StopCoverDecodeWorker();
  void CancelQueuedCoverDecodes();
  void QueueCoverDecode(Game* game, bool priority);
  void PumpCoverDecodeResults();
  SDL_Texture* UploadCoverTexture(const CoverDecodeResult& result);
  SDL_Texture* LoadScaledTexture(const std::string& path, int width, int height);
  void EnsureCover(Game* game, bool priority = false);
  void ReloadCover(Game* game);
  void EvictCover();
  std::string CoverPath(const Game& game) const;
  void RenderGrid(int selection);
  int GridColumns() const;
  int GridRows() const;
  int GridPageSize() const;
  int GridNavigate(int selection, int dx, int dy) const;
  int GridPage(int selection, int direction) const;
  int GridHitTest(int x, int y, int page_start) const;

  void SettingsRoot();
  void PerGameSettingsRoot(Game* game);
  void EmulationSettings(bool per_game, Game* game = nullptr);
  void AdvancedEmulationSettings(bool per_game, Game* game = nullptr);
  void GraphicsSettings(bool per_game, Game* game = nullptr);
  void FrameGenerationSettings(bool per_game, Game* game = nullptr);
  void GraphicsEnhancementsSettings(bool per_game, Game* game = nullptr);
  void GraphicsHacksSettings(bool per_game, Game* game = nullptr);
  void AudioSettings(bool per_game = false, Game* game = nullptr);
  void ConsoleSettings(bool per_game, Game* game = nullptr);
  void WiiSystemSettings();
  void GameCubeDeviceSettings();
  void GameCubeSlotSettings(int slot);
  void ControllerSettings(bool per_game = false, Game* game = nullptr);
  void ControllerPortSettings(bool wii, int port, bool per_game, Game* game);
  void ControllerMappingSettings(bool wii, int port, bool per_game, Game* game,
                                 bool extension_only = false, bool triforce = false,
                                 bool orientation_hotkeys = false);
  void ControllerProfileSettings(bool wii, int port, bool per_game, Game* game);
  void WiiMotionSettings(int port, bool per_game, Game* game);
  void GameModsSettings(Game* game);
  void SaveDataSettings();
  void GCSaveManager(int slot);
  void WiiSaveManager();
  void InstallWAD();
  void InstalledContentManager();
  void NANDManager();
  void ExtractCertificatesFromNAND();
  void AchievementSettings();
  void NetworkSettings();
  void GameCubeNetworkSettings();
  void CreateHomeShortcut(Game* game);
  bool ChooseForwarderIcon(Game* game, std::string* output_path);
  ControllerTarget GetControllerTarget(bool wii, int port, bool per_game, Game* game, bool create);
  std::vector<InputBinding> GetControllerBindings(bool wii, std::string_view extension,
                                                  bool extension_only, bool triforce,
                                                  bool orientation_hotkeys) const;
  std::optional<std::string> CaptureControllerInput(const InputBinding& binding, int position,
                                                    int count, std::string_view current);
  void RenderControllerCapture(const InputBinding& binding, int position, int count, bool releasing,
                               std::string_view current, std::string_view status = {});
  void AppearanceSettings();
  void UpdateScreen();
  std::string InstalledReleaseTag() const;
  std::string UpdateStatusText() const;
  std::vector<std::string> WrapUpdateNotes(std::string_view text, int max_width);
  void PollUpdateNotification();
  void DrawUpdateNotification();
  void LibrarySettings();
  void LibraryFilterMenu();
  void ManageCollections();
  void EditGameOrganization(Game* game);
  void DownloadCovers();
  bool EjectUsbLocation(std::string_view stable_id);
  void GameSourcesScreen();
  void NetworkSharesScreen();
  bool EditSmbShare(Storage::SmbShare* share, bool creating);
  void FileManager();
  std::string FileBrowser(const std::string& start, bool select_folder, bool select_game,
                          bool manage, std::span<const std::string_view> extensions = {},
                          std::string_view selection_title = {});
  void PerGameMenu(Game* game, bool* launch, bool* rescan);
  void CoverSettings(Game* game);
  void ImportCoverFromFile(Game* game);
  void DownloadCover(Game* game);
  int ChooseCoverArtwork(const std::vector<CoverDownload::Artwork>& artwork,
                         std::string_view game_name);

  bool DeleteTree(const std::string& path, const std::atomic_bool* cancel = nullptr);
  bool MeasureTree(const std::string& path, TransferState* state);
  bool CopyTree(const std::string& source, const std::string& destination, TransferState* state);
  bool RenderTransfer(TransferState* state);
  void RunBusyTask(std::string_view title, std::string_view detail,
                   const std::function<void()>& task, std::atomic_bool* cancel = nullptr);
  bool ExecutePaste(const std::string& folder);
  bool RenamePath(const std::string& path);
  void FileActions(const std::string& path);
  void ReplaceSavedPathPrefix(const std::string& old_path, const std::string& new_path);
  void RemoveSavedPathsBelow(const std::string& root);
  void EnsureSourceMountedAtStartup(const std::string& path);
  std::string GameLocationLabel(const Game& game) const;

  std::string SharedGameIniPath(const Game& game) const;
  std::string EntryGameIniPath(const Game& game) const;
  std::string GameIniPath(const Game& game) const;
  std::optional<std::string> GetGameSetting(const Game& game, std::string_view section,
                                            std::string_view key) const;
  bool SetGameSetting(const Game& game, std::string_view section, std::string_view key,
                      const std::optional<std::string>& value);
  bool
  SetGameSettings(const Game& game,
                  std::initializer_list<
                      std::tuple<std::string_view, std::string_view, std::optional<std::string>>>
                      edits);
  void InvalidateGameSettingCache(const Game& game) const;
  std::string GlobalValueLabel(std::string_view value) const;
  std::string UseGlobalValueLabel(std::string_view value) const;
  std::string PerGameBoolLabel(const Game& game, std::string_view section, std::string_view key,
                               bool global, bool inverted = false) const;
  void EditPerGameBool(Game& game, std::string_view title, std::string_view section,
                       std::string_view key, bool global, int delta,
                       std::string_view on_label = "On", std::string_view off_label = "Off",
                       bool inverted = false);

  Store m_store;
  Localization m_localization;
  std::vector<std::string> m_sources;
  std::vector<Storage::SmbShare> m_shares;
  std::vector<Game> m_games;
  std::vector<std::size_t> m_visible_games;
  std::vector<LibraryIdentityRecord> m_library_identities;
  bool m_library_identities_dirty = false;
  std::unordered_set<std::string> m_claimed_library_ids;
  // Existing canonical paths are reserved during a progressive scan so an earlier, identical
  // renamed image cannot steal their IDs through the fingerprint fallback.
  std::unordered_set<std::string> m_reserved_library_ids;
  std::unordered_set<std::string> m_favorites;
  std::vector<Collection> m_collections;
  std::string m_search_query;
  std::string m_active_collection;
  std::vector<Storage::Location> m_usb_locations;
  // Keyed by the normalized current source path; value is {stable device id, path on device}.
  std::unordered_map<std::string, std::pair<std::string, std::string>> m_usb_source_bindings;
  std::shared_ptr<LibraryScanState> m_library_scan;
  std::thread m_library_scan_thread;
  std::vector<std::string> m_pending_scan_sources;
  bool m_pending_nand_reconciliation = false;
  std::unordered_set<std::string> m_unavailable_usb_ids;
  std::shared_ptr<SmbAutoMountState> m_smb_auto_mount;
  std::thread m_smb_auto_mount_thread;
  std::shared_ptr<UsbInitializationState> m_usb_initialization;
  std::thread m_usb_initialization_thread;
  UICommon::GameFileCache m_game_cache;
  std::optional<LaunchRequest> m_pending_launch;

  SDL_Window* m_window = nullptr;
  SDL_Renderer* m_renderer = nullptr;
  SDL_GameController* m_controller = nullptr;
  TTF_Font* m_font_small = nullptr;
  TTF_Font* m_font = nullptr;
  TTF_Font* m_font_large = nullptr;
  SDL_Texture* m_logo = nullptr;
  SDL_Texture* m_glow = nullptr;
  std::array<SDL_Texture*, 10> m_glyphs{};
  std::array<SDL_Texture*, 4> m_flags{};
  bool m_sdl_ready = false;
  bool m_ttf_ready = false;
  bool m_image_ready = false;
  bool m_font_service_ready = false;
  bool m_cover_download_ready = false;
  bool m_config_dirty = false;
  bool m_store_dirty = false;
  bool m_library_refresh_requested = false;
  bool m_shutdown = false;
  bool m_running = true;
  bool m_user_exit_requested = false;
  bool m_application_exit_prepared = false;
  int m_width = 1280;
  int m_height = 720;
  Theme m_theme = Theme::Bubbles;
  SortMode m_sort_mode = SortMode::Alphabetical;
  bool m_animations = true;
  bool m_show_titles = true;
  bool m_show_region_flags = true;
  bool m_show_custom_settings_badges = true;
  int m_grid_columns = 5;
  int m_grid_rows = 2;
  int m_cover_decode_budget = 0;
  std::uint64_t m_cover_use = 0;
  std::mutex m_cover_decode_mutex;
  std::condition_variable m_cover_decode_condition;
  std::deque<CoverDecodeJob> m_cover_decode_jobs;
  std::deque<CoverDecodeResult> m_cover_decode_ready;
  std::thread m_cover_decode_thread;
  bool m_cover_decode_started = false;
  bool m_cover_decode_stop = false;
  std::uint64_t m_cover_decode_epoch = 1;
  std::uint64_t m_cover_request_serial = 0;
  std::uint64_t m_usb_generation = 0;
  Uint32 m_usb_refresh_at = 0;
  Uint32 m_screen_fx_start = 0;
  float m_highlight_y = -1.0f;
  float m_last_frame_highlight_y = -1.0f;
  Uint32 m_next_frame_deadline = 0;
  Uint32 m_frame_interval = 0;
  Uint32 m_interaction_animation_until = 0;
  bool m_frame_has_scrolling_text = false;
  Updater::State m_scheduler_update_state = Updater::State::Idle;
  std::uint64_t m_scheduler_update_downloaded = 0;
  std::uint64_t m_scheduler_update_total = 0;
  std::string m_scheduler_update_tag;
  std::deque<SDL_Event> m_waited_events;
  int m_navigation_held = 0;
  Uint32 m_navigation_since = 0;
  Uint32 m_navigation_last = 0;
  bool m_stick_x_latched = false;
  bool m_stick_y_latched = false;
  TouchGesture m_touch;
  int m_touch_scroll_steps = 1;
  std::array<SDL_Rect, 10> m_footer_hits{};
  int m_footer_hit_count = 0;
  std::string m_clipboard_path;
  bool m_clipboard_move = false;
  std::string m_startup_message;
  std::string m_launcher_path;
  std::string m_update_notice_tag;
  std::string m_update_notified_tag;
  Uint32 m_update_notice_until = 0;
  std::unordered_map<TextKey, TextTexture, TextKeyHash> m_text_cache;
  std::unordered_map<MetricKey, MetricEntry, MetricKeyHash> m_metric_cache;
  std::unordered_map<EllipsisKey, EllipsisEntry, EllipsisKeyHash> m_ellipsis_cache;
  std::unordered_map<std::string, std::pair<int, int>> m_row_positions;
  mutable std::unordered_map<std::string, std::unique_ptr<Common::IniFile>> m_game_ini_cache;
  std::size_t m_text_cache_bytes = 0;
  std::uint64_t m_text_use = 0;

  SDL_Color m_background{0, 8, 16, 255};
  SDL_Color m_text{235, 248, 255, 255};
  SDL_Color m_dim{143, 192, 216, 255};
  SDL_Color m_highlight{118, 222, 255, 255};
  SDL_Color m_value{194, 239, 255, 255};
  SDL_Color m_selection{61, 183, 235, 255};
  SDL_Color m_panel{4, 31, 50, 190};
  SDL_Color m_card{5, 35, 56, 218};
  SDL_Color m_focus{12, 76, 108, 220};
};

void Launcher::LoadDefaults()
{
  if (!m_store.Get("Launcher/Initialized").empty())
    return;
  m_store.Set("Launcher/Initialized", "1");
  m_store.Set("Launcher/Theme", "bubbles");
  m_store.Set("Launcher/Language", "system");
  m_store.SetBool("Launcher/Animations", true);
  m_store.SetBool("Launcher/Sounds", true);
  m_store.SetBool("Launcher/ShowTitles", true);
  m_store.SetBool("Launcher/ShowRegionFlags", true);
  m_store.SetBool("Launcher/ShowCustomSettingsBadges", true);
  m_store.SetBool("Launcher/CheckUpdatesAtBoot", true);
  m_store.Set("Launcher/InstalledReleaseTag", Updater::BuiltReleaseTag());
  m_store.SetInt("Launcher/GridColumns", 5);
  m_store.SetInt("Launcher/GridRows", 2);
  m_store.SetInt("Launcher/SortMode", 0);
  m_store.Set("Network/SteamGridDBKey", "");
  m_sources.clear();
  SaveSources();
  MarkStoreDirty();
}

void Launcher::MarkConfigDirty()
{
  m_config_dirty = true;
}

void Launcher::MarkStoreDirty()
{
  m_store_dirty = true;
}

void Launcher::FlushPendingSaves()
{
  if (m_config_dirty)
  {
    Config::Save();
    m_config_dirty = false;
  }
  if (m_store_dirty)
  {
    if (m_store.Save(std::string(CONFIG_PATH)))
      m_store_dirty = false;
  }
}

void Launcher::ClearTextCaches()
{
  for (auto& [key, value] : m_text_cache)
    SDL_DestroyTexture(value.texture);
  m_text_cache.clear();
  m_metric_cache.clear();
  m_ellipsis_cache.clear();
  m_text_cache_bytes = 0;
  m_text_use = 0;
}

bool Launcher::LoadFonts()
{
  PlSharedFontType font_type = PlSharedFontType_Standard;
  switch (m_localization.GetFontFamily())
  {
  case LauncherFontFamily::SimplifiedChinese:
    font_type = PlSharedFontType_ChineseSimplified;
    break;
  case LauncherFontFamily::TraditionalChinese:
    font_type = PlSharedFontType_ChineseTraditional;
    break;
  case LauncherFontFamily::Korean:
    font_type = PlSharedFontType_KO;
    break;
  case LauncherFontFamily::Standard:
    break;
  }

  PlFontData font_data{};
  if (R_FAILED(plGetSharedFontByType(&font_data, font_type)) || !font_data.address ||
      font_data.size == 0 || font_data.size > INT_MAX)
  {
    return false;
  }

  const bool large = m_height >= 1080;
  const auto open_font = [&](int size) {
    SDL_RWops* stream = SDL_RWFromConstMem(font_data.address, static_cast<int>(font_data.size));
    return stream ? TTF_OpenFontRW(stream, 1, size) : nullptr;
  };
  TTF_Font* const small = open_font(large ? 26 : 20);
  TTF_Font* const normal = open_font(large ? 32 : 26);
  TTF_Font* const large_font = open_font(large ? 52 : 40);
  if (!small || !normal || !large_font)
  {
    if (small)
      TTF_CloseFont(small);
    if (normal)
      TTF_CloseFont(normal);
    if (large_font)
      TTF_CloseFont(large_font);
    return false;
  }

  ClearTextCaches();
  if (m_font_small)
    TTF_CloseFont(m_font_small);
  if (m_font)
    TTF_CloseFont(m_font);
  if (m_font_large)
    TTF_CloseFont(m_font_large);
  m_font_small = small;
  m_font = normal;
  m_font_large = large_font;
  return true;
}

bool Launcher::Initialize(bool applet_installer)
{
  ClearControllerValueCache();
  constexpr std::array<std::string_view, 2> required_directories = {"sdmc:/switch", DATA_DIRECTORY};
  for (const std::string_view path : required_directories)
  {
    if (!EnsureDirectory(path))
      return false;
  }
  if (!applet_installer && (!EnsureDirectory(COVER_DIRECTORY) || !EnsureDirectory(LSFG_DIRECTORY)))
  {
    return false;
  }

  (void)m_store.Load(std::string(CONFIG_PATH));
  LoadDefaults();
  m_localization.SetLanguage(m_store.Get("Launcher/Language", "system"));
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
  {
    std::fprintf(stderr, "[Dolphin Switch] SDL initialization failed: %s\n", SDL_GetError());
    return false;
  }
  m_sdl_ready = true;
  InitializeUiAudio();
  SetUiAudioEnabled(m_store.GetBool("Launcher/Sounds", true));
  if (TTF_Init() != 0)
    return false;
  m_ttf_ready = true;
  const int image_flags = IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP;
  if ((IMG_Init(image_flags) & image_flags) != image_flags)
    return false;
  m_image_ready = true;

  if (appletGetOperationMode() == AppletOperationMode_Console)
  {
    m_width = 1920;
    m_height = 1080;
  }
  m_window = SDL_CreateWindow("Dolphin", 0, 0, m_width, m_height, SDL_WINDOW_FULLSCREEN);
  if (!m_window)
  {
    std::fprintf(stderr, "[Dolphin Switch] SDL window creation failed: %s\n", SDL_GetError());
    return false;
  }
  m_renderer =
      SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!m_renderer)
  {
    std::fprintf(stderr, "[Dolphin Switch] SDL renderer creation failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
  SDL_GetRendererOutputSize(m_renderer, &m_width, &m_height);

  const Result pl_result = plInitialize(PlServiceType_User);
  if (R_FAILED(pl_result))
    return false;
  m_font_service_ready = true;
  if (!LoadFonts())
    return false;

  InitializeUiTextures();

  if (SDL_Surface* surface = IMG_Load("romfs:/Resources/dolphin_logo.png"))
  {
    m_logo = SDL_CreateTextureFromSurface(m_renderer, surface);
    SDL_FreeSurface(surface);
  }
  for (int index = 0; index < SDL_NumJoysticks(); ++index)
  {
    if (SDL_IsGameController(index))
    {
      m_controller = SDL_GameControllerOpen(index);
      break;
    }
  }
  m_cover_download_ready = false;
  ApplyAppearance();
  if (!applet_installer)
  {
    StartCoverDecodeWorker();
    // Present the launcher as soon as SDL, fonts and the theme are ready. Source restoration,
    // network startup and scanning happen after this frame so the user never waits on black.
    ClearBackground();
    DrawHeader("Dolphin");
    DrawWrappedCentered(m_font_large, m_width / 2, m_height / 2 - 48, m_width - 120, 48, 2,
                        m_localization.Translate("Loading game library..."), m_value);
    DrawWrappedCentered(
        m_font_small, m_width / 2, m_height / 2 + 26, m_width - 120, 32, 2,
        m_localization.Translate("The first page will appear as soon as it is ready."), m_dim);
    SDL_RenderPresent(m_renderer);
    LoadSourcesAndShares();
    m_cover_download_ready = CoverDownload::Initialize();
  }
  if (!applet_installer)
    Storage::SetUsbStatusCallback(UsbStatusWake);
  return true;
}

void Launcher::Shutdown()
{
  if (m_shutdown)
    return;
  m_shutdown = true;
  // The USB callback uses SDL_PushEvent, so fence it before the SDL event subsystem is torn down.
  Storage::SetUsbStatusCallback(nullptr);
  StopGameScan();
  StopUsbInitialization();
  StopAutoMountShares();
  StopCoverDecodeWorker();
  Updater::Shutdown();
  FlushPendingSaves();

  for (Game& game : m_games)
  {
    if (game.cover)
      SDL_DestroyTexture(game.cover);
    game.cover = nullptr;
  }
  for (auto& [key, value] : m_text_cache)
    SDL_DestroyTexture(value.texture);
  m_text_cache.clear();
  m_metric_cache.clear();
  m_ellipsis_cache.clear();
  m_text_cache_bytes = 0;
  m_text_use = 0;
  DestroyUiTextures();
  if (m_logo)
    SDL_DestroyTexture(m_logo);
  if (m_glow)
    SDL_DestroyTexture(m_glow);
  m_logo = m_glow = nullptr;

  m_games.clear();
  m_game_cache.Clear(UICommon::GameFileCache::DeleteOnDisk::No);

  if (m_font_small)
    TTF_CloseFont(m_font_small);
  if (m_font)
    TTF_CloseFont(m_font);
  if (m_font_large)
    TTF_CloseFont(m_font_large);
  m_font_small = m_font = m_font_large = nullptr;
  if (m_font_service_ready)
    plExit();
  m_font_service_ready = false;

  ShutdownUiAudio();
  if (m_controller)
    SDL_GameControllerClose(m_controller);
  m_controller = nullptr;

  if (m_renderer)
    SDL_DestroyRenderer(m_renderer);
  if (m_window)
    SDL_DestroyWindow(m_window);
  m_renderer = nullptr;
  m_window = nullptr;
  if (m_image_ready)
    IMG_Quit();
  if (m_ttf_ready)
    TTF_Quit();
  if (m_sdl_ready)
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
  m_image_ready = m_ttf_ready = m_sdl_ready = false;

  if (m_cover_download_ready)
    CoverDownload::Shutdown();
  m_cover_download_ready = false;
}

bool Launcher::ConfirmApplicationExit()
{
  if (!Confirm("Exit Dolphin?",
               std::array<std::string, 2>{
                   std::string(m_localization.Translate(
                       "Active scans and network operations will be cancelled safely.")),
                   std::string(m_localization.Translate("Return to the HOME Menu?"))}))
  {
    BeginScreenFx();
    return false;
  }
  m_user_exit_requested = true;
  m_running = false;
  return true;
}

void Launcher::PrepareApplicationExit()
{
  if (m_application_exit_prepared)
    return;
  m_application_exit_prepared = true;

  // Keep presenting a real frame while cancellable scan/network workers drain. A saved custom
  // collection can become interactive long before a full-library scan is finished; immediately
  // blocking in join() in that state made the Switch compositor fall back to a black frame.
  const auto render_closing = [&] {
    ClearBackground();
    DrawHeader("Dolphin");
    DrawWrappedCentered(m_font_large, m_width / 2, m_height / 2 - 48, m_width - 120, 48, 2,
                        m_localization.Translate("Closing Dolphin..."), m_value);
    DrawWrappedCentered(m_font_small, m_width / 2, m_height / 2 + 30, m_width - 120, 32, 2,
                        m_localization.Translate("Finishing background operations safely."), m_dim);
    SDL_RenderPresent(m_renderer);
  };
  render_closing();

  Storage::SetUsbStatusCallback(nullptr);
  if (m_library_scan)
    m_library_scan->cancel.store(true, std::memory_order_release);
  if (m_smb_auto_mount)
    m_smb_auto_mount->cancel.store(true, std::memory_order_release);

  Uint32 next_closing_frame = SDL_GetTicks() + 100;
  while (
      (m_library_scan && !m_library_scan->complete.load(std::memory_order_acquire)) ||
      (m_usb_initialization && !m_usb_initialization->complete.load(std::memory_order_acquire)) ||
      (m_smb_auto_mount && !m_smb_auto_mount->complete.load(std::memory_order_acquire)))
  {
    SDL_PumpEvents();
    const Uint32 now = SDL_GetTicks();
    if (SDL_TICKS_PASSED(now, next_closing_frame))
    {
      render_closing();
      next_closing_frame = now + 100;
    }
    if (!appletMainLoop())
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  StopGameScan();
  StopUsbInitialization();
  StopAutoMountShares();
  StopCoverDecodeWorker();
  Updater::Shutdown();
  if (m_cover_download_ready)
  {
    CoverDownload::Shutdown();
    m_cover_download_ready = false;
  }
  FlushPendingSaves();
  // On a game launch storage must stay mounted, but on an explicit application exit it should be
  // retired before SDL disappears so open SMB/USB registrations cannot prolong a black teardown.
  Storage::Shutdown();
}

Launcher::~Launcher()
{
  Shutdown();
}

void Launcher::ApplyAppearance()
{
  const Theme previous_theme = m_theme;
  const std::string theme = Lower(m_store.Get("Launcher/Theme", "bubbles"));
  m_theme = theme == "xmb"     ? Theme::Xmb :
            theme == "glow"    ? Theme::Glow :
            theme == "classic" ? Theme::Classic :
            theme == "oled"    ? Theme::Oled :
                                 Theme::Bubbles;
  m_animations = m_store.GetBool("Launcher/Animations", true);
  m_show_titles = m_store.GetBool("Launcher/ShowTitles", true);
  m_show_region_flags = m_store.GetBool("Launcher/ShowRegionFlags", true);
  m_show_custom_settings_badges = m_store.GetBool("Launcher/ShowCustomSettingsBadges", true);
  m_grid_columns = std::clamp(m_store.GetInt("Launcher/GridColumns", 5), 3, 8);
  m_grid_rows = std::clamp(m_store.GetInt("Launcher/GridRows", 2), 1, 3);
  m_sort_mode = static_cast<SortMode>(std::clamp(m_store.GetInt("Launcher/SortMode", 0), 0, 2));
  if (m_theme == Theme::Xmb)
  {
    m_background = {2, 35, 92, 255};
    m_text = {246, 250, 255, 255};
    m_dim = {176, 207, 233, 255};
    m_highlight = {151, 229, 255, 255};
    m_value = {255, 255, 255, 255};
    m_selection = {116, 218, 255, 255};
    m_panel = {4, 28, 73, 164};
    m_card = {5, 36, 86, 196};
    m_focus = {20, 91, 148, 214};
  }
  else if (m_theme == Theme::Classic)
  {
    m_background = {22, 24, 30, 255};
    m_text = {228, 230, 235, 255};
    m_dim = {150, 155, 165, 255};
    m_highlight = {96, 200, 255, 255};
    m_value = {255, 210, 100, 255};
    m_selection = {255, 170, 0, 255};
    m_panel = {28, 31, 40, 255};
    m_card = {24, 26, 34, 255};
    m_focus = {66, 56, 30, 235};
  }
  else if (m_theme == Theme::Oled)
  {
    m_background = {0, 0, 0, 255};
    m_text = {245, 247, 249, 255};
    m_dim = {145, 151, 158, 255};
    m_highlight = {105, 220, 255, 255};
    m_value = {255, 255, 255, 255};
    m_selection = {0, 210, 190, 255};
    m_panel = {4, 4, 5, 248};
    m_card = {8, 8, 10, 250};
    m_focus = {0, 58, 53, 245};
  }
  else if (m_theme == Theme::Glow)
  {
    m_background = {8, 12, 24, 255};
    m_text = {235, 239, 247, 255};
    m_dim = {151, 163, 184, 255};
    m_highlight = {100, 211, 255, 255};
    m_value = {255, 215, 120, 255};
    m_selection = {116, 200, 255, 255};
    m_panel = {16, 23, 39, 184};
    m_card = {22, 30, 49, 214};
    m_focus = {28, 69, 92, 208};
  }
  else
  {
    m_background = {3, 82, 120, 255};
    m_text = {245, 252, 255, 255};
    m_dim = {187, 229, 243, 255};
    m_highlight = {220, 248, 255, 255};
    m_value = {255, 255, 255, 255};
    m_selection = {111, 224, 249, 255};
    m_panel = {0, 67, 101, 180};
    m_card = {2, 75, 110, 207};
    m_focus = {17, 133, 169, 218};
  }
  if (previous_theme != m_theme && m_renderer)
  {
    for (auto& [key, value] : m_text_cache)
      SDL_DestroyTexture(value.texture);
    m_text_cache.clear();
    m_metric_cache.clear();
    m_ellipsis_cache.clear();
    m_text_cache_bytes = 0;
    m_text_use = 0;
  }
}

void Launcher::FillRect(int x, int y, int width, int height, SDL_Color color)
{
  SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
  SDL_Rect rectangle{x, y, width, height};
  SDL_RenderFillRect(m_renderer, &rectangle);
}

void Launcher::Border(int x, int y, int width, int height, int thickness, SDL_Color color)
{
  SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
  for (int index = 0; index < thickness; ++index)
  {
    SDL_Rect rectangle{x - index, y - index, width + index * 2, height + index * 2};
    SDL_RenderDrawRect(m_renderer, &rectangle);
  }
}

bool Launcher::HasAnimatedBackground() const
{
  return m_theme == Theme::Xmb || m_theme == Theme::Bubbles || m_theme == Theme::Glow;
}

void Launcher::FillCircle(int center_x, int center_y, int radius, SDL_Color color)
{
  SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
  for (int dy = -radius; dy <= radius; ++dy)
  {
    const int dx =
        static_cast<int>(std::sqrt(static_cast<double>(radius * radius - dy * dy)) + 0.5);
    SDL_RenderDrawLine(m_renderer, center_x - dx, center_y + dy, center_x + dx, center_y + dy);
  }
}

void Launcher::GlassPanel(int x, int y, int width, int height)
{
  FillRect(x, y, width, height, m_panel);
  Border(x, y, width, height, 1,
         SDL_Color{255, 255, 255, static_cast<Uint8>(HasAnimatedBackground() ? 28 : 16)});
}

SDL_Texture* Launcher::MakeGlyph(std::string_view label, bool pill)
{
  if (!m_font_small || !m_font_large)
    return nullptr;
  constexpr int supersample = 3;
  const int base = TTF_FontHeight(m_font_small) + 6;
  const int height = base * supersample;
  const int width = (pill ? base * 8 / 5 : base) * supersample;
  SDL_Texture* texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET, width, height);
  if (!texture)
    return nullptr;
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  SDL_SetRenderTarget(m_renderer, texture);
  SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
  SDL_RenderClear(m_renderer);
  const SDL_Color edge{14, 16, 22, 255};
  const SDL_Color rim{92, 99, 114, 255};
  const SDL_Color face{52, 57, 68, 255};
  if (pill)
  {
    const int radius = height / 2;
    FillCircle(radius, radius, radius, edge);
    FillCircle(width - radius, radius, radius, edge);
    FillRect(radius, 0, width - radius * 2, height, edge);
    FillCircle(radius, radius, radius - supersample, rim);
    FillCircle(width - radius, radius, radius - supersample, rim);
    FillRect(radius, supersample, width - radius * 2, height - supersample * 2, rim);
    FillCircle(radius, radius, radius - supersample * 2, face);
    FillCircle(width - radius, radius, radius - supersample * 2, face);
    FillRect(radius, supersample * 2, width - radius * 2, height - supersample * 4, face);
  }
  else
  {
    const int radius = height / 2;
    FillCircle(width / 2, height / 2, radius, edge);
    FillCircle(width / 2, height / 2, radius - supersample, rim);
    FillCircle(width / 2, height / 2, radius - supersample * 2, face);
  }
  const std::string owned(label);
  SDL_Surface* surface =
      TTF_RenderUTF8_Blended(m_font_large, owned.c_str(), SDL_Color{246, 248, 252, 255});
  if (surface)
  {
    SDL_Texture* text = SDL_CreateTextureFromSurface(m_renderer, surface);
    int text_width = surface->w;
    int text_height = surface->h;
    const int inner_height = height * 56 / 100;
    if (text_height > 0)
    {
      text_width = text_width * inner_height / text_height;
      text_height = inner_height;
    }
    SDL_Rect destination{(width - text_width) / 2, (height - text_height) / 2, text_width,
                         text_height};
    SDL_FreeSurface(surface);
    if (text)
    {
      SDL_SetTextureBlendMode(text, SDL_BLENDMODE_BLEND);
      SDL_RenderCopy(m_renderer, text, nullptr, &destination);
      SDL_DestroyTexture(text);
    }
  }
  SDL_SetRenderTarget(m_renderer, nullptr);
  return texture;
}

SDL_Texture* Launcher::MakeFlagTexture(DiscIO::Region region, int width, int height)
{
  SDL_Texture* texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888,
                                           SDL_TEXTUREACCESS_TARGET, width, height);
  if (!texture)
    return nullptr;
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  SDL_SetRenderTarget(m_renderer, texture);
  SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
  SDL_RenderClear(m_renderer);
  if (region == DiscIO::Region::NTSC_J)
  {
    FillRect(0, 0, width, height, SDL_Color{245, 245, 245, 255});
    FillCircle(width / 2, height / 2, height * 30 / 100, SDL_Color{188, 0, 45, 255});
  }
  else if (region == DiscIO::Region::NTSC_U)
  {
    for (int stripe = 0; stripe < 7; ++stripe)
      FillRect(0, stripe * height / 7, width, height / 7 + 1,
               stripe % 2 ? SDL_Color{235, 235, 235, 255} : SDL_Color{178, 34, 52, 255});
    FillRect(0, 0, width * 2 / 5, height * 4 / 7, SDL_Color{45, 50, 110, 255});
    for (int row = 0; row < 2; ++row)
      for (int column = 0; column < 3; ++column)
        FillRect(5 + column * (width * 2 / 5 - 8) / 3, 4 + row * 8, 2, 2,
                 SDL_Color{255, 255, 255, 255});
  }
  else
  {
    FillRect(0, 0, width, height, SDL_Color{0, 51, 153, 255});
    for (int star = 0; star < 12; ++star)
    {
      const double angle = star * 6.28318 / 12.0;
      const int x = width / 2 + static_cast<int>(std::cos(angle) * width * 0.30);
      const int y = height / 2 + static_cast<int>(std::sin(angle) * height * 0.32);
      FillRect(x - 1, y - 1, 2, 2, SDL_Color{255, 204, 0, 255});
    }
  }
  SDL_SetRenderTarget(m_renderer, nullptr);
  return texture;
}

void Launcher::InitializeUiTextures()
{
  m_glyphs[0] = MakeGlyph("A", false);
  m_glyphs[1] = MakeGlyph("B", false);
  m_glyphs[2] = MakeGlyph("X", false);
  m_glyphs[3] = MakeGlyph("Y", false);
  m_glyphs[4] = MakeGlyph("+", false);
  m_glyphs[5] = MakeGlyph("L", true);
  m_glyphs[6] = MakeGlyph("R", true);
  m_glyphs[7] = MakeGlyph("-", false);
  m_glyphs[8] = MakeGlyph("<", false);
  m_glyphs[9] = MakeGlyph(">", false);
  m_flags[1] = MakeFlagTexture(DiscIO::Region::NTSC_U, 36, 24);
  m_flags[2] = MakeFlagTexture(DiscIO::Region::PAL, 36, 24);
  m_flags[3] = MakeFlagTexture(DiscIO::Region::NTSC_J, 36, 24);
}

void Launcher::DestroyUiTextures()
{
  for (SDL_Texture*& texture : m_glyphs)
  {
    if (texture)
      SDL_DestroyTexture(texture);
    texture = nullptr;
  }
  for (SDL_Texture*& texture : m_flags)
  {
    if (texture)
      SDL_DestroyTexture(texture);
    texture = nullptr;
  }
}

void Launcher::EnsureGlowTexture()
{
  if (m_glow || !m_renderer)
    return;
  constexpr int size = 256;
  SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
  if (!surface)
    return;
  if (SDL_LockSurface(surface) == 0)
  {
    for (int y = 0; y < size; ++y)
    {
      auto* row =
          reinterpret_cast<Uint32*>(static_cast<Uint8*>(surface->pixels) + y * surface->pitch);
      for (int x = 0; x < size; ++x)
      {
        const float dx = (x - (size - 1) * 0.5f) / (size * 0.5f);
        const float dy = (y - (size - 1) * 0.5f) / (size * 0.5f);
        const float distance = std::sqrt(dx * dx + dy * dy);
        const float strength = distance >= 1.0f ? 0.0f : 1.0f - distance;
        row[x] = SDL_MapRGBA(surface->format, 255, 255, 255,
                             static_cast<Uint8>(255.0f * strength * strength));
      }
    }
    SDL_UnlockSurface(surface);
    m_glow = SDL_CreateTextureFromSurface(m_renderer, surface);
    if (m_glow)
      SDL_SetTextureBlendMode(m_glow, SDL_BLENDMODE_BLEND);
  }
  SDL_FreeSurface(surface);
}

void Launcher::DrawBubbles(float time)
{
  const SDL_Color top{70, 198, 229, 255};
  const SDL_Color middle{15, 147, 193, 255};
  const SDL_Color bottom{3, 82, 120, 255};
  const auto blend = [](Uint8 first, Uint8 second, float amount) {
    return static_cast<Uint8>(first + (second - first) * std::clamp(amount, 0.0f, 1.0f));
  };
  constexpr int bands = 56;
  for (int band = 0; band < bands; ++band)
  {
    const float y = (band + 0.5f) / bands;
    SDL_Color color{};
    if (y < 0.58f)
    {
      const float amount = y / 0.58f;
      color = {blend(top.r, middle.r, amount), blend(top.g, middle.g, amount),
               blend(top.b, middle.b, amount), 255};
    }
    else
    {
      const float amount = (y - 0.58f) / 0.42f;
      color = {blend(middle.r, bottom.r, amount), blend(middle.g, bottom.g, amount),
               blend(middle.b, bottom.b, amount), 255};
    }
    const int y0 = band * m_height / bands;
    const int y1 = (band + 1) * m_height / bands;
    FillRect(0, y0, m_width, y1 - y0, color);
  }

  EnsureGlowTexture();
  if (m_glow)
  {
    SDL_SetTextureColorMod(m_glow, 197, 244, 255);
    SDL_SetTextureAlphaMod(m_glow, 96);
    SDL_Rect surface{-m_width / 6, -m_height / 3, m_width * 4 / 3, m_height * 2 / 3};
    SDL_RenderCopy(m_renderer, m_glow, nullptr, &surface);
    for (int ray = 0; ray < 7; ++ray)
    {
      const float sway = std::sin(time * (0.10f + ray * 0.013f) + ray * 1.31f);
      const int width = m_width * (11 + (ray % 3) * 3) / 100;
      const int x =
          m_width * (8 + ray * 14) / 100 + static_cast<int>(sway * m_width * 0.025f) - width / 2;
      SDL_Rect shaft{x, -m_height / 3, width, m_height * 4 / 3};
      SDL_SetTextureAlphaMod(m_glow, static_cast<Uint8>(23 + (ray % 3) * 7));
      SDL_RenderCopyEx(m_renderer, m_glow, nullptr, &shaft, -9.0 + ray * 2.7 + sway * 2.0, nullptr,
                       SDL_FLIP_NONE);
    }
  }

  const auto draw_bubble = [&](int center_x, int center_y, int radius, Uint8 alpha) {
    if (radius < 3 || alpha == 0)
      return;
    if (m_glow)
    {
      SDL_SetTextureColorMod(m_glow, 180, 237, 255);
      SDL_SetTextureAlphaMod(m_glow, static_cast<Uint8>(alpha / 5));
      SDL_Rect glow{center_x - radius * 2, center_y - radius * 2, radius * 4, radius * 4};
      SDL_RenderCopy(m_renderer, m_glow, nullptr, &glow);
    }
    constexpr int segments = 24;
    std::array<SDL_Point, segments + 1> outer{};
    std::array<SDL_Point, segments + 1> inner{};
    for (int segment = 0; segment <= segments; ++segment)
    {
      const float angle = segment * 6.2831853f / segments;
      const float x = std::cos(angle);
      const float y = std::sin(angle);
      outer[segment] = {center_x + static_cast<int>(x * radius),
                        center_y + static_cast<int>(y * radius)};
      inner[segment] = {center_x + static_cast<int>(x * (radius - 1)),
                        center_y + static_cast<int>(y * (radius - 1))};
    }
    SDL_SetRenderDrawColor(m_renderer, 188, 240, 255, alpha);
    SDL_RenderDrawLines(m_renderer, outer.data(), outer.size());
    SDL_RenderDrawLines(m_renderer, inner.data(), inner.size());
    SDL_SetRenderDrawColor(m_renderer, 235, 252, 255,
                           static_cast<Uint8>(std::min(255, static_cast<int>(alpha) + 55)));
    std::array<SDL_Point, 6> highlight{};
    for (int segment = 0; segment < static_cast<int>(highlight.size()); ++segment)
    {
      const float angle = 3.55f + segment * 0.13f;
      highlight[segment] = {center_x + static_cast<int>(std::cos(angle) * radius),
                            center_y + static_cast<int>(std::sin(angle) * radius)};
    }
    SDL_RenderDrawLines(m_renderer, highlight.data(), highlight.size());
  };

  for (int index = 0; index < 18; ++index)
  {
    const float progress =
        std::fmod(index * 0.173f + time * (0.038f + (index % 5) * 0.007f), 1.18f);
    const float y = 1.08f - progress;
    const float x = 0.05f + std::fmod(index * 0.283f, 0.90f) +
                    0.032f * std::sin(time * (0.31f + (index % 4) * 0.04f) + index);
    const float fade = std::min(std::clamp((1.10f - y) * 5.0f, 0.0f, 1.0f),
                                std::clamp((y + 0.12f) * 6.0f, 0.0f, 1.0f));
    int radius = static_cast<int>(m_height * (0.009f + (index % 6) * 0.0042f));
    if (index % 11 == 0)
      radius = radius * 3 / 2;
    draw_bubble(static_cast<int>(x * m_width), static_cast<int>(y * m_height), radius,
                static_cast<Uint8>(fade * (85 + (index % 4) * 24)));
  }
  for (int index = 0; index < 24; ++index)
  {
    const float travel =
        std::fmod(index * 0.371f + time * 0.008f * (0.65f + (index % 5) * 0.11f), 1.12f) - 0.06f;
    const float y =
        std::fmod(index * 0.217f + 0.11f * std::sin(time * 0.29f + index * 1.73f), 1.0f);
    const float pulse = 0.45f + 0.55f * std::sin(time * (0.9f + (index % 4) * 0.17f) + index);
    const Uint8 alpha = static_cast<Uint8>(62 * (0.55f + 0.45f * pulse));
    const int size = index % 9 == 0 ? 3 : 2;
    FillRect(static_cast<int>(travel * m_width), static_cast<int>(y * m_height), size, size,
             SDL_Color{216, 246, 255, alpha});
  }
  if (m_glow)
  {
    SDL_SetTextureColorMod(m_glow, 255, 255, 255);
    SDL_SetTextureAlphaMod(m_glow, 255);
  }
}

void Launcher::DrawXmbRibbon(float time, float center, float amplitude, float frequency,
                             float slope, float phase, int half_width, SDL_Color color)
{
  constexpr int point_count = 121;
  std::array<SDL_Point, point_count> points{};
  const auto wave_y = [&](float x) {
    const float primary = std::sin(x * 6.2831853f * frequency + phase + time * 0.115f);
    const float detail =
        std::sin(x * 6.2831853f * (frequency * 2.07f) + phase * 0.61f - time * 0.072f);
    return center + slope * (x - 0.5f) + amplitude * (primary + detail * 0.24f);
  };
  for (int offset = -half_width; offset <= half_width; ++offset)
  {
    const float distance = half_width ? std::abs(static_cast<float>(offset) / half_width) : 0.0f;
    const Uint8 alpha =
        static_cast<Uint8>(color.a * std::pow(std::max(0.0f, 1.0f - distance), 1.45f));
    if (alpha < 2)
      continue;
    for (int point = 0; point < point_count; ++point)
    {
      const float x = static_cast<float>(point) / (point_count - 1);
      points[point] = {static_cast<int>(x * m_width),
                       static_cast<int>(wave_y(x) * m_height) + offset};
    }
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, alpha);
    SDL_RenderDrawLines(m_renderer, points.data(), points.size());
  }
}

void Launcher::DrawXmbFilament(float time, float center, float amplitude, float frequency,
                               float slope, float phase, SDL_Color color)
{
  constexpr int point_count = 161;
  std::array<SDL_Point, point_count> points{};
  for (int point = 0; point < point_count; ++point)
  {
    const float x = static_cast<float>(point) / (point_count - 1);
    const float primary = std::sin(x * 6.2831853f * frequency + phase + time * 0.115f);
    const float detail =
        std::sin(x * 6.2831853f * (frequency * 2.07f) + phase * 0.61f - time * 0.072f);
    const float y = center + slope * (x - 0.5f) + amplitude * (primary + detail * 0.24f);
    points[point] = {static_cast<int>(x * m_width), static_cast<int>(y * m_height)};
  }
  SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
  SDL_RenderDrawLines(m_renderer, points.data(), points.size());
}

void Launcher::DrawXmbSparkles(float time)
{
  for (int index = 0; index < 42; ++index)
  {
    const float x =
        std::fmod(index * 0.618034f + time * (0.0022f + (index % 5) * 0.00045f), 1.08f) - 0.04f;
    const float primary = std::sin(x * 6.2831853f * 0.91f + 0.4f + time * 0.115f);
    const float detail = std::sin(x * 6.2831853f * (0.91f * 2.07f) + 0.4f * 0.61f - time * 0.072f);
    const float y = 0.585f + 0.075f * (x - 0.5f) + 0.095f * (primary + detail * 0.24f) +
                    (std::fmod(index * 0.413f, 1.0f) - 0.5f) * 0.31f;
    const float pulse =
        0.5f + 0.5f * std::sin(time * (0.55f + (index % 7) * 0.08f) + index * 1.731f);
    const Uint8 alpha = static_cast<Uint8>(28.0f + pulse * (index % 9 == 0 ? 142.0f : 82.0f));
    const int px = static_cast<int>(x * m_width);
    const int py = static_cast<int>(y * m_height);
    const int size = index % 9 == 0 ? 3 : 2;
    FillRect(px, py, size, size, SDL_Color{220, 246, 255, alpha});
    if (index % 9 == 0 && pulse > 0.55f)
    {
      SDL_SetRenderDrawColor(m_renderer, 235, 251, 255, static_cast<Uint8>(alpha * 0.62f));
      SDL_RenderDrawLine(m_renderer, px - 5, py + 1, px + 7, py + 1);
      SDL_RenderDrawLine(m_renderer, px + 1, py - 5, px + 1, py + 7);
    }
  }
}

void Launcher::DrawXmb(float time)
{
  const SDL_Color top{3, 37, 102, 255};
  const SDL_Color middle{8, 93, 184, 255};
  const SDL_Color bottom{0, 20, 68, 255};
  const auto blend = [](Uint8 first, Uint8 second, float amount) {
    return static_cast<Uint8>(first + (second - first) * std::clamp(amount, 0.0f, 1.0f));
  };
  constexpr int bands = 72;
  for (int band = 0; band < bands; ++band)
  {
    const float y = (band + 0.5f) / bands;
    SDL_Color color{};
    if (y < 0.52f)
    {
      const float amount = y / 0.52f;
      color = {blend(top.r, middle.r, amount), blend(top.g, middle.g, amount),
               blend(top.b, middle.b, amount), 255};
    }
    else
    {
      const float amount = (y - 0.52f) / 0.48f;
      color = {blend(middle.r, bottom.r, amount), blend(middle.g, bottom.g, amount),
               blend(middle.b, bottom.b, amount), 255};
    }
    const int y0 = band * m_height / bands;
    const int y1 = (band + 1) * m_height / bands;
    FillRect(0, y0, m_width, y1 - y0, color);
  }

  EnsureGlowTexture();
  if (m_glow)
  {
    const auto glow = [&](float x, float y, float radius, Uint8 red, Uint8 green, Uint8 blue,
                          Uint8 alpha) {
      const int diameter = static_cast<int>(m_height * radius);
      SDL_Rect destination{static_cast<int>(m_width * x) - diameter / 2,
                           static_cast<int>(m_height * y) - diameter / 2, diameter, diameter};
      SDL_SetTextureColorMod(m_glow, red, green, blue);
      SDL_SetTextureAlphaMod(m_glow, alpha);
      SDL_RenderCopy(m_renderer, m_glow, nullptr, &destination);
    };
    glow(0.10f, 0.43f, 1.18f, 55, 157, 255, 54);
    glow(0.84f, 0.38f, 0.92f, 41, 112, 228, 42);
  }
  DrawXmbRibbon(time, 0.655f, 0.082f, 0.78f, -0.105f, 2.15f, std::max(12, m_height / 18),
                SDL_Color{63, 166, 255, 31});
  DrawXmbRibbon(time, 0.575f, 0.074f, 0.96f, 0.080f, 0.35f, std::max(10, m_height / 25),
                SDL_Color{189, 235, 255, 48});
  DrawXmbRibbon(time, 0.605f, 0.049f, 1.28f, -0.025f, 3.82f, std::max(5, m_height / 54),
                SDL_Color{230, 250, 255, 72});
  for (int trace = 0; trace < 9; ++trace)
  {
    const float offset = (trace - 4) * 0.009f;
    DrawXmbFilament(time, 0.588f + offset, 0.083f + trace * 0.0017f, 0.91f, 0.052f,
                    0.62f + trace * 0.19f,
                    SDL_Color{202, 241, 255, static_cast<Uint8>(18 + trace % 3 * 8)});
  }
  DrawXmbFilament(time, 0.578f, 0.073f, 0.96f, 0.080f, 0.35f, SDL_Color{243, 253, 255, 136});
  DrawXmbSparkles(time);
  if (m_glow)
  {
    SDL_SetTextureColorMod(m_glow, 255, 255, 255);
    SDL_SetTextureAlphaMod(m_glow, 255);
  }
}

void Launcher::ClearBackground()
{
  SDL_RenderSetClipRect(m_renderer, nullptr);
  SDL_SetRenderDrawColor(m_renderer, m_background.r, m_background.g, m_background.b, 255);
  SDL_RenderClear(m_renderer);
  const float time = m_animations ? SDL_GetTicks() / 1000.0f : 0.0f;
  if (m_theme == Theme::Xmb)
  {
    DrawXmb(time);
    return;
  }
  if (m_theme == Theme::Bubbles)
  {
    DrawBubbles(time);
    return;
  }
  if (m_theme != Theme::Glow)
    return;
  EnsureGlowTexture();
  if (!m_glow)
    return;
  struct Glow
  {
    float x, y, radius;
    Uint8 r, g, b, a;
  };
  const std::array<Glow, 4> glows = {
      {{0.10f + 0.13f * std::sin(time * 0.43f), 0.20f + 0.11f * std::cos(time * 0.37f), 0.90f, 45,
        140, 255, 128},
       {0.84f + 0.12f * std::cos(time * 0.34f), 0.34f + 0.10f * std::sin(time * 0.41f), 0.78f, 154,
        75, 255, 112},
       {0.54f + 0.10f * std::sin(time * 0.29f), 0.91f + 0.06f * std::cos(time * 0.33f), 0.94f, 0,
        210, 190, 94},
       {0.42f + 0.08f * std::cos(time * 0.25f), 0.48f + 0.09f * std::sin(time * 0.31f), 0.58f, 64,
        125, 255, 67}}};
  for (const Glow& glow : glows)
  {
    const int diameter = static_cast<int>(m_height * glow.radius);
    SDL_Rect destination{static_cast<int>(m_width * glow.x) - diameter / 2,
                         static_cast<int>(m_height * glow.y) - diameter / 2, diameter, diameter};
    SDL_SetTextureColorMod(m_glow, glow.r, glow.g, glow.b);
    SDL_SetTextureAlphaMod(m_glow, glow.a);
    SDL_RenderCopy(m_renderer, m_glow, nullptr, &destination);
  }
  for (int index = 0; index < 28; ++index)
  {
    const float travel =
        std::fmod(index * 0.371f + time * 0.011f * (0.65f + (index % 5) * 0.11f), 1.12f) - 0.06f;
    const float y =
        std::fmod(index * 0.217f + 0.11f * std::sin(time * 0.29f + index * 1.73f), 1.0f);
    const float pulse = 0.45f + 0.55f * std::sin(time * (0.9f + (index % 4) * 0.17f) + index);
    FillRect(static_cast<int>(travel * m_width), static_cast<int>(y * m_height),
             index % 9 == 0 ? 3 : 2, index % 9 == 0 ? 3 : 2,
             SDL_Color{182, 224, 255, static_cast<Uint8>(88 * (0.55f + 0.45f * pulse))});
  }
  SDL_SetTextureColorMod(m_glow, 255, 255, 255);
  SDL_SetTextureAlphaMod(m_glow, 255);
}

int Launcher::TextWidth(TTF_Font* font, std::string_view text)
{
  if (!font || text.empty())
    return 0;
  MetricKey key{font, std::string(text)};
  const auto found = m_metric_cache.find(key);
  if (found != m_metric_cache.end())
  {
    found->second.use = ++m_text_use;
    return found->second.width;
  }
  int width = 0;
  int height = 0;
  if (TTF_SizeUTF8(font, key.text.c_str(), &width, &height) != 0)
    return 0;
  if (m_metric_cache.size() >= METRIC_CACHE_LIMIT)
  {
    auto victim = m_metric_cache.begin();
    for (auto iterator = std::next(m_metric_cache.begin()); iterator != m_metric_cache.end();
         ++iterator)
    {
      if (iterator->second.use < victim->second.use)
        victim = iterator;
    }
    m_metric_cache.erase(victim);
  }
  m_metric_cache.emplace(std::move(key), MetricEntry{width, ++m_text_use});
  return width;
}

void Launcher::DrawText(TTF_Font* font, int x, int y, std::string_view text, SDL_Color color)
{
  if (!font || text.empty())
    return;
  const Uint32 packed = color.r | (static_cast<Uint32>(color.g) << 8) |
                        (static_cast<Uint32>(color.b) << 16) | (static_cast<Uint32>(color.a) << 24);
  TextKey key{font, packed, std::string(text)};
  auto found = m_text_cache.find(key);
  if (found != m_text_cache.end())
  {
    found->second.use = ++m_text_use;
    SDL_Rect destination{x, y, found->second.width, found->second.height};
    SDL_RenderCopy(m_renderer, found->second.texture, nullptr, &destination);
    return;
  }
  SDL_Surface* surface = TTF_RenderUTF8_Blended(font, key.text.c_str(), color);
  if (!surface)
    return;
  SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
  const int width = surface->w;
  const int height = surface->h;
  SDL_FreeSurface(surface);
  if (!texture)
    return;
  MetricKey metric_key{font, key.text};
  const auto metric = m_metric_cache.find(metric_key);
  if (metric != m_metric_cache.end())
  {
    metric->second.width = width;
    metric->second.use = ++m_text_use;
  }
  else
  {
    if (m_metric_cache.size() >= METRIC_CACHE_LIMIT)
    {
      auto victim = m_metric_cache.begin();
      for (auto iterator = std::next(m_metric_cache.begin()); iterator != m_metric_cache.end();
           ++iterator)
      {
        if (iterator->second.use < victim->second.use)
          victim = iterator;
      }
      m_metric_cache.erase(victim);
    }
    m_metric_cache.emplace(std::move(metric_key), MetricEntry{width, ++m_text_use});
  }
  const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
  if (bytes > TEXT_CACHE_BYTES)
  {
    SDL_Rect destination{x, y, width, height};
    SDL_RenderCopy(m_renderer, texture, nullptr, &destination);
    SDL_DestroyTexture(texture);
    return;
  }
  while (!m_text_cache.empty() &&
         (m_text_cache.size() >= TEXT_CACHE_LIMIT || m_text_cache_bytes > TEXT_CACHE_BYTES - bytes))
  {
    auto victim = m_text_cache.begin();
    for (auto iterator = std::next(m_text_cache.begin()); iterator != m_text_cache.end();
         ++iterator)
    {
      if (iterator->second.use < victim->second.use)
        victim = iterator;
    }
    SDL_DestroyTexture(victim->second.texture);
    m_text_cache_bytes -= victim->second.bytes;
    m_text_cache.erase(victim);
  }
  auto [iterator, inserted] = m_text_cache.emplace(
      std::move(key), TextTexture{texture, width, height, bytes, ++m_text_use});
  m_text_cache_bytes += bytes;
  SDL_Rect destination{x, y, width, height};
  SDL_RenderCopy(m_renderer, iterator->second.texture, nullptr, &destination);
}

void Launcher::DrawTextCentered(TTF_Font* font, int center_x, int y, std::string_view text,
                                SDL_Color color)
{
  DrawText(font, center_x - TextWidth(font, text) / 2, y, text, color);
}

void Launcher::DrawTextRight(TTF_Font* font, int right_x, int y, std::string_view text,
                             SDL_Color color)
{
  DrawText(font, right_x - TextWidth(font, text), y, text, color);
}

std::string Launcher::Ellipsize(TTF_Font* font, std::string_view text, int max_width)
{
  if (!font || text.empty() || max_width <= 0)
    return {};
  EllipsisKey key{font, max_width, std::string(text)};
  const auto found = m_ellipsis_cache.find(key);
  if (found != m_ellipsis_cache.end())
  {
    found->second.use = ++m_text_use;
    return found->second.text;
  }

  std::string result;
  if (TextWidth(font, text) <= max_width)
  {
    result = text;
  }
  else
  {
    std::vector<std::size_t> boundaries{0};
    for (std::size_t index = 0; index < text.size();)
    {
      const unsigned char lead = static_cast<unsigned char>(text[index]);
      std::size_t length = lead < 0x80           ? 1 :
                           (lead & 0xe0) == 0xc0 ? 2 :
                           (lead & 0xf0) == 0xe0 ? 3 :
                           (lead & 0xf8) == 0xf0 ? 4 :
                                                   1;
      if (index + length > text.size())
        length = 1;
      for (std::size_t continuation = 1; continuation < length; ++continuation)
      {
        if ((static_cast<unsigned char>(text[index + continuation]) & 0xc0) != 0x80)
        {
          length = 1;
          break;
        }
      }
      index += length;
      boundaries.push_back(index);
    }
    std::size_t low = 0;
    std::size_t high = boundaries.size() - 1;
    while (low < high)
    {
      const std::size_t middle = (low + high + 1) / 2;
      const std::string candidate = key.text.substr(0, boundaries[middle]) + "...";
      if (TextWidth(font, candidate) <= max_width)
        low = middle;
      else
        high = middle - 1;
    }
    result = key.text.substr(0, boundaries[low]) + "...";
  }

  if (m_ellipsis_cache.size() >= ELLIPSIS_CACHE_LIMIT)
  {
    auto victim = m_ellipsis_cache.begin();
    for (auto iterator = std::next(m_ellipsis_cache.begin()); iterator != m_ellipsis_cache.end();
         ++iterator)
    {
      if (iterator->second.use < victim->second.use)
        victim = iterator;
    }
    m_ellipsis_cache.erase(victim);
  }
  m_ellipsis_cache.emplace(std::move(key), EllipsisEntry{result, ++m_text_use});
  return result;
}

void Launcher::DrawWrapped(TTF_Font* font, int x, int y, int max_width, int line_height,
                           int max_lines, std::string_view text, SDL_Color color)
{
  if (!font || text.empty() || max_width <= 0 || max_lines <= 0)
    return;
  std::string line;
  int drawn = 0;
  const auto emit = [&](const std::string& value) {
    if (drawn < max_lines)
      DrawText(font, x, y + drawn++ * line_height, value, color);
  };
  std::size_t index = 0;
  while (index < text.size() && drawn < max_lines)
  {
    std::size_t separator = index;
    while (separator < text.size() && text[separator] != ' ' && text[separator] != '\n')
      ++separator;
    const std::string word{text.substr(index, separator - index)};
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && TextWidth(font, candidate) > max_width)
    {
      emit(line);
      line = word;
    }
    else
    {
      line = candidate;
    }
    if (separator < text.size() && text[separator] == '\n')
    {
      emit(line);
      line.clear();
    }
    index = separator + 1;
  }
  if (drawn < max_lines && !line.empty())
    emit(line);
}

void Launcher::DrawWrappedCentered(TTF_Font* font, int center_x, int y, int max_width,
                                   int line_height, int max_lines, std::string_view text,
                                   SDL_Color color)
{
  const std::vector<std::string> lines = WrapText(font, max_width, max_lines, text);
  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index)
  {
    DrawTextCentered(font, center_x, y + static_cast<int>(line_index) * line_height,
                     lines[line_index], color);
  }
}

std::vector<std::string> Launcher::WrapText(TTF_Font* font, int max_width, int max_lines,
                                            std::string_view text)
{
  std::vector<std::string> lines;
  if (!font || text.empty() || max_width <= 0 || max_lines <= 0)
    return lines;

  std::string line;
  bool truncated = false;
  const auto emit = [&] {
    if (line.empty())
      return;
    if (static_cast<int>(lines.size()) >= max_lines)
    {
      truncated = true;
      return;
    }
    lines.emplace_back(std::move(line));
    line.clear();
  };

  std::size_t index = 0;
  while (index < text.size())
  {
    while (index < text.size() && text[index] == ' ')
      ++index;
    if (index >= text.size())
      break;
    if (text[index] == '\n')
    {
      emit();
      ++index;
      continue;
    }

    std::size_t separator = index;
    while (separator < text.size() && text[separator] != ' ' && text[separator] != '\n')
      ++separator;
    std::string word{text.substr(index, separator - index)};
    if (TextWidth(font, word) > max_width)
      word = Ellipsize(font, word, max_width);
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && TextWidth(font, candidate) > max_width)
    {
      emit();
      if (static_cast<int>(lines.size()) >= max_lines)
      {
        truncated = true;
        break;
      }
      line = std::move(word);
    }
    else
    {
      line = candidate;
    }
    if (separator < text.size() && text[separator] == '\n')
      emit();
    index = separator + 1;
  }
  if (!line.empty())
    emit();
  if (truncated && !lines.empty())
    lines.back() = Ellipsize(font, lines.back() + " ...", max_width);
  return lines;
}

void Launcher::DrawScrollingTextLeft(TTF_Font* font, int x, int y, int max_width,
                                     std::string_view text, SDL_Color color)
{
  if (!font || text.empty() || max_width <= 0)
    return;
  const int width = TextWidth(font, text);
  if (width <= max_width)
  {
    DrawText(font, x, y, text, color);
    return;
  }
  m_frame_has_scrolling_text = true;
  SDL_Rect clip{x, y - 2, max_width, TTF_FontHeight(font) + 6};
  SDL_RenderSetClipRect(m_renderer, &clip);
  const int span = width - max_width;
  const float time = (SDL_GetTicks() % 6000) / 6000.0f;
  const float position = time < 0.5f ? time * 2.0f : (1.0f - time) * 2.0f;
  DrawText(font, x - static_cast<int>(position * span), y, text, color);
  SDL_RenderSetClipRect(m_renderer, nullptr);
}

void Launcher::DrawScrollingTextRight(TTF_Font* font, int right_x, int y, int max_width,
                                      std::string_view text, SDL_Color color)
{
  if (!font || text.empty() || max_width <= 0)
    return;
  const int width = TextWidth(font, text);
  if (width <= max_width)
  {
    DrawTextRight(font, right_x, y, text, color);
    return;
  }
  m_frame_has_scrolling_text = true;
  const int x = right_x - max_width;
  SDL_Rect clip{x, y - 2, max_width, TTF_FontHeight(font) + 6};
  SDL_RenderSetClipRect(m_renderer, &clip);
  const int span = width - max_width;
  const float time = (SDL_GetTicks() % 6000) / 6000.0f;
  const float position = time < 0.5f ? time * 2.0f : (1.0f - time) * 2.0f;
  DrawText(font, x - static_cast<int>(position * span), y, text, color);
  SDL_RenderSetClipRect(m_renderer, nullptr);
}

void Launcher::DrawTitleCell(int center_x, int width, int y, const Game& game, bool selected,
                             SDL_Color color)
{
  const int text_width = TextWidth(m_font_small, game.title);
  if (text_width <= width)
  {
    DrawTextCentered(m_font_small, center_x, y, game.title, color);
    return;
  }
  if (!selected)
  {
    DrawTextCentered(m_font_small, center_x, y, Ellipsize(m_font_small, game.title, width), color);
    return;
  }
  m_frame_has_scrolling_text = true;
  const int x = center_x - width / 2;
  SDL_Rect clip{x, y - 2, width, TTF_FontHeight(m_font_small) + 8};
  SDL_RenderSetClipRect(m_renderer, &clip);
  const int span = text_width - width;
  const float time = (SDL_GetTicks() % 5000) / 5000.0f;
  const float position = time < 0.5f ? time * 2.0f : (1.0f - time) * 2.0f;
  DrawText(m_font_small, x - static_cast<int>(position * span), y, game.title, color);
  SDL_RenderSetClipRect(m_renderer, nullptr);
}

void Launcher::DrawHeader(std::string_view title, std::string_view context)
{
  // Header titles are launcher-owned UI.  Context strings are deliberately left raw because
  // they frequently contain game names, paths, profile names, or remote share names.
  title = m_localization.Translate(title);
  const int top_height = m_width >= 1600 ? 112 : 80;
  const int band_height = top_height - 4;
  FillRect(0, 0, m_width, band_height, m_panel);
  if (!HasAnimatedBackground())
    FillRect(0, band_height, m_width, 2, m_selection);
  const int logo_size = band_height - 12;
  if (m_logo)
  {
    SDL_Rect destination{26, (band_height - logo_size) / 2, logo_size, logo_size};
    SDL_RenderCopy(m_renderer, m_logo, nullptr, &destination);
  }
  DrawTextCentered(m_font_large, m_width / 2, (band_height - TTF_FontHeight(m_font_large)) / 2,
                   title, m_value);
  if (!context.empty())
  {
    const int title_right = m_width / 2 + TextWidth(m_font_large, title) / 2;
    const int maximum_width = (m_width - 28) - title_right - 30;
    if (maximum_width > 40)
      DrawScrollingTextRight(m_font_small, m_width - 28,
                             (band_height - TTF_FontHeight(m_font_small)) / 2, maximum_width,
                             context, m_value);
  }
}

void Launcher::DrawButtonHint(int x, int y, std::string_view button, std::string_view label)
{
  // Footer labels are launcher-owned UI; the button token itself is a controller glyph.
  label = m_localization.Translate(label);
  SDL_Texture* const glyph = ButtonGlyph(button);
  int width = 0;
  int height = 0;
  if (glyph)
  {
    SDL_QueryTexture(glyph, nullptr, nullptr, &width, &height);
    width /= 3;
    height /= 3;
    SDL_Rect destination{x, y - height / 2, width, height};
    SDL_RenderCopy(m_renderer, glyph, nullptr, &destination);
  }
  else
  {
    width = TextWidth(m_font_small, button) + 14;
    height = TTF_FontHeight(m_font_small) + 6;
    Border(x, y - height / 2, width, height, 1, m_dim);
    DrawTextCentered(m_font_small, x + width / 2, y - TTF_FontHeight(m_font_small) / 2, button,
                     m_text);
  }
  if (!label.empty())
    DrawText(m_font_small, x + width + 8, y - TTF_FontHeight(m_font_small) / 2, label, m_dim);
}

SDL_Texture* Launcher::ButtonGlyph(std::string_view button) const
{
  if (button == "A")
    return m_glyphs[0];
  if (button == "B")
    return m_glyphs[1];
  if (button == "X")
    return m_glyphs[2];
  if (button == "Y")
    return m_glyphs[3];
  if (button == "+")
    return m_glyphs[4];
  if (button == "L")
    return m_glyphs[5];
  if (button == "R")
    return m_glyphs[6];
  if (button == "-")
    return m_glyphs[7];
  if (button == "Left")
    return m_glyphs[8];
  if (button == "Right")
    return m_glyphs[9];
  return nullptr;
}

void Launcher::DrawFooter(std::span<const std::pair<std::string_view, std::string_view>> hints,
                          int center_y)
{
  constexpr int glyph_gap = 16;
  constexpr int label_gap = 8;
  constexpr int pair_gap = 26;
  const int y = center_y >= 0 ? center_y : m_height - 26;
  int total = 0;
  for (const auto& [button, label] : hints)
  {
    const std::string_view localized_label = m_localization.Translate(label);
    SDL_Texture* const glyph = ButtonGlyph(button);
    int width = 0;
    if (glyph)
      SDL_QueryTexture(glyph, nullptr, nullptr, &width, nullptr);
    width = glyph ? width / 3 : TextWidth(m_font_small, button) + 14;
    total += width;
    if (!localized_label.empty())
      total += label_gap + TextWidth(m_font_small, localized_label);
    total += localized_label.empty() ? glyph_gap : pair_gap;
  }
  if (!hints.empty())
    total -= hints.back().second.empty() ? glyph_gap : pair_gap;
  int x = (m_width - total) / 2;
  m_footer_hit_count = 0;
  for (const auto& [button, label] : hints)
  {
    const std::string_view localized_label = m_localization.Translate(label);
    SDL_Texture* const glyph = ButtonGlyph(button);
    int width = 0;
    int height = 0;
    if (glyph)
      SDL_QueryTexture(glyph, nullptr, nullptr, &width, &height);
    if (glyph)
    {
      width /= 3;
      height /= 3;
    }
    else
    {
      width = TextWidth(m_font_small, button) + 14;
      height = TTF_FontHeight(m_font_small) + 6;
    }
    const int item_x = x;
    DrawButtonHint(x, y, button, label);
    x += width;
    if (!localized_label.empty())
      x += label_gap + TextWidth(m_font_small, localized_label);
    if (m_footer_hit_count < static_cast<int>(m_footer_hits.size()))
      m_footer_hits[m_footer_hit_count++] = {item_x - 6, y - height / 2 - 8, x - item_x + 12,
                                             height + 16};
    if (!localized_label.empty())
      x += pair_gap;
    else
      x += glyph_gap;
  }
}

int Launcher::FooterHitTest(int x, int y) const
{
  for (int index = 0; index < m_footer_hit_count; ++index)
  {
    const SDL_Rect& hit = m_footer_hits[index];
    if (x >= hit.x && x < hit.x + hit.w && y >= hit.y && y < hit.y + hit.h)
      return index;
  }
  return -1;
}

void Launcher::DrawSettingsFooter(std::string_view text)
{
  std::vector<std::pair<std::string_view, std::string_view>> hints;
  std::vector<std::string_view> tokens;
  const auto is_separator = [](char character) { return character == ' '; };
  std::size_t cursor = 0;
  while (cursor < text.size())
  {
    while (cursor < text.size() && is_separator(text[cursor]))
      ++cursor;
    if (cursor >= text.size())
      break;

    const std::size_t token_start = cursor;
    std::size_t token_end = text.size();
    std::size_t separator_end = text.size();
    bool found_separator = false;
    for (std::size_t index = cursor + 1; index < text.size(); ++index)
    {
      if (is_separator(text[index - 1]) && is_separator(text[index]))
      {
        token_end = index - 1;
        separator_end = index;
        found_separator = true;
        while (separator_end < text.size() && is_separator(text[separator_end]))
          ++separator_end;
        break;
      }
    }
    const auto trim_token = [&](std::string_view value) {
      std::size_t left = 0;
      while (left < value.size() && is_separator(value[left]))
        ++left;
      std::size_t right = value.size();
      while (right > left && is_separator(value[right - 1]))
        --right;
      return std::string_view(value.data() + left, right - left);
    };
    tokens.emplace_back(trim_token(text.substr(token_start, token_end - token_start + 1)));
    cursor = found_separator ? separator_end : text.size();
  }
  for (std::size_t index = 0; index + 1 < tokens.size(); index += 2)
  {
    if (tokens[index] == "Left / Right")
    {
      hints.emplace_back("Left", std::string_view{});
      hints.emplace_back("Right", tokens[index + 1]);
    }
    else
    {
      hints.emplace_back(tokens[index], tokens[index + 1]);
    }
  }
  if (hints.empty())
    DrawTextCentered(m_font_small, m_width / 2, m_height - 38, text, m_dim);
  else
    DrawFooter(hints);
}

void Launcher::BeginScreenFx()
{
  m_screen_fx_start = SDL_GetTicks();
  m_highlight_y = -1.0f;
}

void Launcher::DrawFadeIn()
{
  if (!m_animations)
    return;
  constexpr int duration = 160;
  const int elapsed = static_cast<int>(SDL_GetTicks() - m_screen_fx_start);
  if (elapsed < duration)
    FillRect(0, 0, m_width, m_height,
             SDL_Color{0, 0, 0, static_cast<Uint8>(200 * (duration - elapsed) / duration)});
}

void Launcher::ShowInfoCard(std::string_view section, std::string_view title, std::string_view kind,
                            std::string_view description, std::string_view current,
                            std::string_view scope, bool localize_title, bool localize_current)
{
  const std::string localized_section = std::string(
      m_localization.Translate(section.empty() ? std::string_view{"Settings"} : section));
  const std::string_view effective_title = title.empty() ? std::string_view{"Setting info"} : title;
  const std::string localized_title = localize_title ?
                                          std::string(m_localization.Translate(effective_title)) :
                                          std::string(effective_title);
  const std::string localized_kind =
      std::string(m_localization.Translate(kind.empty() ? "Dolphin setting" : kind));
  const std::string localized_scope =
      scope.empty() ? std::string{} : std::string(m_localization.Translate(scope));
  const std::string localized_description = std::string(m_localization.Translate(description));
  BeginScreenFx();
  while (BeginFrame())
  {
    bool close = false;
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::Tap)
        close = true;
      if (event.type == SDL_CONTROLLERBUTTONDOWN &&
          (event.cbutton.button == BUTTON_CONFIRM || event.cbutton.button == BUTTON_CANCEL ||
           event.cbutton.button == BUTTON_SETTINGS))
        close = true;
      if (event.type == SDL_KEYDOWN &&
          (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_ESCAPE ||
           event.key.keysym.sym == SDLK_x))
        close = true;
    }
    if (close)
      return;

    ClearBackground();
    const int panel_width = std::min(m_width - 120, 1000);
    const int panel_height = std::min(m_height - 96, 500);
    const int panel_x = (m_width - panel_width) / 2;
    const int panel_y = (m_height - panel_height) / 2;
    GlassPanel(panel_x, panel_y, panel_width, panel_height);
    Border(panel_x, panel_y, panel_width, panel_height, 3, m_selection);
    DrawText(m_font_small, panel_x + 40, panel_y + 24, localized_section, m_dim);
    DrawScrollingTextLeft(m_font_large, panel_x + 40, panel_y + 58, panel_width - 80,
                          localized_title, m_value);

    std::string metadata = localized_kind;
    if (!localized_scope.empty())
      metadata += "  |  " + localized_scope;
    DrawScrollingTextLeft(m_font_small, panel_x + 40, panel_y + 114, panel_width - 80, metadata,
                          m_selection);

    int body_y = panel_y + 164;
    if (!current.empty())
    {
      const std::string_view prefix = m_localization.Translate("Current: ");
      DrawText(m_font_small, panel_x + 40, panel_y + 146, prefix, m_dim);
      const std::string_view displayed_current =
          localize_current ? m_localization.Translate(current) : current;
      DrawScrollingTextLeft(m_font_small, panel_x + 40 + TextWidth(m_font_small, prefix),
                            panel_y + 146, panel_width - 80 - TextWidth(m_font_small, prefix),
                            displayed_current, m_text);
      body_y = panel_y + 198;
    }
    FillRect(panel_x + 40, body_y - 18, panel_width - 80, 2, SDL_Color{70, 78, 92, 210});
    const int maximum_lines = std::max(1, (panel_y + panel_height - 70 - body_y) / 32);
    DrawWrapped(m_font, panel_x + 40, body_y, panel_width - 80, 32, maximum_lines,
                localized_description, m_text);
    const std::string close_hint = "A / B / X  " + std::string(m_localization.Translate("Close")) +
                                   "       " +
                                   std::string(m_localization.Translate("Touch anywhere to close"));
    DrawWrappedCentered(m_font_small, m_width / 2, panel_y + panel_height - 48, panel_width - 80,
                        26, 2, close_hint, m_dim);
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

bool Launcher::BeginFrame()
{
  if (!m_running || !appletMainLoop())
    return false;
  PumpCoverDecodeResults();
  m_frame_has_scrolling_text = false;
  if (m_controller && !SDL_GameControllerGetAttached(m_controller))
  {
    SDL_GameControllerClose(m_controller);
    m_controller = nullptr;
    m_stick_x_latched = m_stick_y_latched = false;
    m_navigation_held = 0;
    m_navigation_since = m_navigation_last = 0;
  }
  QueueNavigationRepeat();
  return true;
}

bool Launcher::PollEvent(SDL_Event* event)
{
  if (!event)
    return false;
  while (true)
  {
    if (!m_waited_events.empty())
    {
      *event = m_waited_events.front();
      m_waited_events.pop_front();
    }
    else if (!SDL_PollEvent(event))
    {
      return false;
    }
    if (event->type == SDL_QUIT)
    {
      m_running = false;
      continue;
    }
    if (event->type == SDL_CONTROLLERDEVICEADDED)
    {
      if (!m_controller && SDL_IsGameController(event->cdevice.which))
        m_controller = SDL_GameControllerOpen(event->cdevice.which);
      continue;
    }
    if (event->type == SDL_CONTROLLERDEVICEREMOVED)
    {
      if (m_controller)
      {
        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(m_controller);
        if (joystick && SDL_JoystickInstanceID(joystick) == event->cdevice.which)
        {
          SDL_GameControllerClose(m_controller);
          m_controller = nullptr;
          m_stick_x_latched = m_stick_y_latched = false;
          m_navigation_held = 0;
          m_navigation_since = m_navigation_last = 0;
        }
      }
      continue;
    }
    if (event->type == SDL_CONTROLLERAXISMOTION)
    {
      constexpr int threshold = 18000;
      constexpr int dead_zone = 8000;
      int direction = -1;
      if (event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
      {
        if (!m_stick_x_latched && event->caxis.value < -threshold)
        {
          m_stick_x_latched = true;
          direction = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        }
        else if (!m_stick_x_latched && event->caxis.value > threshold)
        {
          m_stick_x_latched = true;
          direction = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        }
        else if (event->caxis.value > -dead_zone && event->caxis.value < dead_zone)
        {
          m_stick_x_latched = false;
        }
      }
      else if (event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
      {
        if (!m_stick_y_latched && event->caxis.value < -threshold)
        {
          m_stick_y_latched = true;
          direction = SDL_CONTROLLER_BUTTON_DPAD_UP;
        }
        else if (!m_stick_y_latched && event->caxis.value > threshold)
        {
          m_stick_y_latched = true;
          direction = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
        }
        else if (event->caxis.value > -dead_zone && event->caxis.value < dead_zone)
        {
          m_stick_y_latched = false;
        }
      }
      if (direction >= 0)
      {
        SDL_Event navigation{};
        navigation.type = SDL_CONTROLLERBUTTONDOWN;
        navigation.cbutton.button = static_cast<Uint8>(direction);
        SDL_PushEvent(&navigation);
      }
    }
    if (event->type == SDL_CONTROLLERBUTTONDOWN)
    {
      switch (event->cbutton.button)
      {
      case BUTTON_CONFIRM:
        PlayUiSound(UiSound::Confirm);
        break;
      case BUTTON_CANCEL:
        PlayUiSound(UiSound::Back);
        break;
      case SDL_CONTROLLER_BUTTON_DPAD_UP:
      case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
      case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
      case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
      case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
      case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        PlayUiSound(UiSound::Navigate);
        break;
      default:
        break;
      }
    }
    switch (event->type)
    {
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEWHEEL:
      // Keep short selection, cover, and touch transitions smooth after an input wake-up.
      m_interaction_animation_until = SDL_GetTicks() + 220;
      break;
    default:
      break;
    }
    return true;
  }
  return false;
}

bool Launcher::FrameNeedsAnimation()
{
  const Uint32 now = SDL_GetTicks();
  if (m_animations && m_highlight_y >= 0.0f && m_last_frame_highlight_y >= 0.0f &&
      std::abs(m_highlight_y - m_last_frame_highlight_y) > 0.2f)
  {
    // The highlight uses an exponential approach, so allow a few quiet frames for the tail.
    m_interaction_animation_until = now + 96;
  }
  m_last_frame_highlight_y = m_highlight_y;

  const bool fade_active = m_animations && m_screen_fx_start != 0 && now - m_screen_fx_start < 160;
  const bool interaction_active =
      m_interaction_animation_until != 0 && !SDL_TICKS_PASSED(now, m_interaction_animation_until);
  return (m_animations && HasAnimatedBackground()) || fade_active || m_frame_has_scrolling_text ||
         interaction_active || m_navigation_held != 0 || m_touch.active;
}

void Launcher::WaitForNextFrame(bool force_animation)
{
  constexpr Uint32 animated_interval = 16;
  constexpr Uint32 lifecycle_poll_interval = 250;
  const bool animate = force_animation || FrameNeedsAnimation();
  Uint32 now = SDL_GetTicks();

  // Static screens do not have a frame rate.  Poll applet lifecycle and worker state without
  // returning to the caller (and therefore without rendering) until an event or real timer fires.
  // USB and launcher workers also post SDL user events; the generation/state checks are a safe
  // fallback for a lost or unavailable event bridge.
  if (!animate)
  {
    m_frame_interval = 0;
    m_next_frame_deadline = 0;
    for (;;)
    {
      now = SDL_GetTicks();
      int timeout = static_cast<int>(lifecycle_poll_interval);
      const auto include_deadline = [&](Uint32 deadline) {
        if (deadline == 0)
          return false;
        if (SDL_TICKS_PASSED(now, deadline))
          return true;
        timeout = std::min(timeout, static_cast<int>(deadline - now));
        return false;
      };
      if (include_deadline(m_usb_refresh_at) || include_deadline(m_update_notice_until))
        return;

      SDL_Event event{};
      if (SDL_WaitEventTimeout(&event, timeout))
      {
        m_waited_events.push_back(event);
        return;
      }
      if (!m_running || !appletMainLoop())
      {
        m_running = false;
        return;
      }
      if (Storage::UsbStatusGeneration() != m_usb_generation)
        return;

      const Updater::Snapshot update = Updater::GetSnapshot();
      if (update.state != m_scheduler_update_state ||
          update.downloaded != m_scheduler_update_downloaded ||
          update.total != m_scheduler_update_total || update.release.tag != m_scheduler_update_tag)
      {
        m_scheduler_update_state = update.state;
        m_scheduler_update_downloaded = update.downloaded;
        m_scheduler_update_total = update.total;
        m_scheduler_update_tag = update.release.tag;
        return;
      }
    }
  }

  const Uint32 interval = animated_interval;
  if (m_frame_interval != interval || m_next_frame_deadline == 0)
  {
    m_frame_interval = interval;
    m_next_frame_deadline = now + interval;
  }
  if (!SDL_TICKS_PASSED(now, m_next_frame_deadline))
  {
    SDL_Event event{};
    const int timeout = static_cast<int>(m_next_frame_deadline - now);
    if (SDL_WaitEventTimeout(&event, timeout))
    {
      // Waiting must not consume the input (or a worker wake-up) before the screen loop sees it.
      m_waited_events.push_back(event);
      m_next_frame_deadline = 0;
      return;
    }
    now = SDL_GetTicks();
    if (!SDL_TICKS_PASSED(now, m_next_frame_deadline))
      return;
  }

  const Uint32 following_deadline = m_next_frame_deadline + interval;
  m_next_frame_deadline =
      SDL_TICKS_PASSED(now, following_deadline) ? now + interval : following_deadline;
}

TouchKind Launcher::FeedTouch(const SDL_Event& event, int* x, int* y)
{
  constexpr int tap_move = 26;
  constexpr int swipe_distance = 90;
  constexpr int scroll_step = 30;
  constexpr Uint32 tap_time = 400;
  if (event.type == SDL_FINGERDOWN)
  {
    if (m_touch.active && SDL_GetTicks() - m_touch.started_at < 2000)
      return TouchKind::None;
    m_touch.active = true;
    m_touch.vertical = false;
    m_touch.finger = event.tfinger.fingerId;
    m_touch.start_x = event.tfinger.x * m_width;
    m_touch.start_y = event.tfinger.y * m_height;
    m_touch.last_y = m_touch.start_y;
    m_touch.started_at = SDL_GetTicks();
  }
  else if (event.type == SDL_FINGERMOTION && m_touch.active &&
           event.tfinger.fingerId == m_touch.finger)
  {
    const float current_x = event.tfinger.x * m_width;
    const float current_y = event.tfinger.y * m_height;
    const float dx = current_x - m_touch.start_x;
    const float dy = current_y - m_touch.start_y;
    if (!m_touch.vertical && std::abs(dy) > tap_move && std::abs(dy) > std::abs(dx) * 1.15f)
      m_touch.vertical = true;
    if (m_touch.vertical)
    {
      const float step = current_y - m_touch.last_y;
      if (std::abs(step) >= scroll_step)
      {
        m_touch_scroll_steps = std::clamp(static_cast<int>(std::abs(step) / scroll_step), 1, 6);
        m_touch.last_y = current_y;
        if (x)
          *x = static_cast<int>(current_x);
        if (y)
          *y = static_cast<int>(current_y);
        return step < 0 ? TouchKind::ScrollUp : TouchKind::ScrollDown;
      }
    }
  }
  else if (event.type == SDL_FINGERUP && m_touch.active && event.tfinger.fingerId == m_touch.finger)
  {
    m_touch.active = false;
    const float current_x = event.tfinger.x * m_width;
    const float current_y = event.tfinger.y * m_height;
    const float dx = current_x - m_touch.start_x;
    const float dy = current_y - m_touch.start_y;
    const Uint32 elapsed = SDL_GetTicks() - m_touch.started_at;
    if (x)
      *x = static_cast<int>(current_x);
    if (y)
      *y = static_cast<int>(current_y);
    if (m_touch.vertical || (std::abs(dy) >= 55 && std::abs(dy) > std::abs(dx) * 1.15f))
    {
      const float remaining = current_y - m_touch.last_y;
      if (std::abs(remaining) < 18 && m_touch.vertical)
        return TouchKind::None;
      const float distance = m_touch.vertical ? remaining : dy;
      m_touch_scroll_steps = std::clamp(static_cast<int>(std::abs(distance) / scroll_step), 1, 6);
      return distance < 0 ? TouchKind::ScrollUp : TouchKind::ScrollDown;
    }
    if (std::abs(dx) >= swipe_distance && std::abs(dx) > std::abs(dy) * 1.5f)
      return dx < 0 ? TouchKind::SwipeLeft : TouchKind::SwipeRight;
    if (std::abs(dx) <= tap_move && std::abs(dy) <= tap_move && elapsed <= tap_time)
      return TouchKind::Tap;
  }
  return TouchKind::None;
}

bool Launcher::TouchScrollList(TouchKind kind, int* selection, int* top, int count, int visible)
{
  if (!selection || !top || count <= 0 ||
      (kind != TouchKind::ScrollUp && kind != TouchKind::ScrollDown))
    return false;
  const int previous = *selection;
  const int delta = (kind == TouchKind::ScrollUp ? 1 : -1) * m_touch_scroll_steps;
  *selection = std::clamp(*selection + delta, 0, count - 1);
  if (*selection < *top)
    *top = *selection;
  if (*selection >= *top + visible)
    *top = *selection - visible + 1;
  *top = std::max(0, *top);
  if (*selection != previous)
    PlayUiSound(UiSound::Navigate);
  return true;
}

void Launcher::QueueNavigationRepeat()
{
  if (!m_controller || !SDL_GameControllerGetAttached(m_controller))
    return;
  constexpr int threshold = 18000;
  int direction = 0;
  if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_DPAD_UP) ||
      SDL_GameControllerGetAxis(m_controller, SDL_CONTROLLER_AXIS_LEFTY) < -threshold)
    direction = SDL_CONTROLLER_BUTTON_DPAD_UP;
  else if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
           SDL_GameControllerGetAxis(m_controller, SDL_CONTROLLER_AXIS_LEFTY) > threshold)
    direction = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
  else if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ||
           SDL_GameControllerGetAxis(m_controller, SDL_CONTROLLER_AXIS_LEFTX) < -threshold)
    direction = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
  else if (SDL_GameControllerGetButton(m_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
           SDL_GameControllerGetAxis(m_controller, SDL_CONTROLLER_AXIS_LEFTX) > threshold)
    direction = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  const Uint32 now = SDL_GetTicks();
  if (direction != m_navigation_held)
  {
    m_navigation_held = direction;
    m_navigation_since = now;
    m_navigation_last = now;
    return;
  }
  if (!direction || now - m_navigation_since < 360 || now - m_navigation_last < 85)
    return;
  m_navigation_last = now;
  SDL_Event navigation{};
  navigation.type = SDL_CONTROLLERBUTTONDOWN;
  navigation.cbutton.button = static_cast<Uint8>(direction);
  SDL_PushEvent(&navigation);
}

int Launcher::EventNavigation(const SDL_Event& event) const
{
  if (event.type == SDL_CONTROLLERBUTTONDOWN)
  {
    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
      return -1;
    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
      return 1;
  }
  if (event.type == SDL_KEYDOWN)
  {
    if (event.key.keysym.sym == SDLK_UP)
      return -1;
    if (event.key.keysym.sym == SDLK_DOWN)
      return 1;
  }
  return 0;
}

bool Launcher::PromptText(std::string_view header, std::string_view initial, std::string* output,
                          bool password, bool allow_empty, std::string_view subtext,
                          std::string_view guide)
{
  if (!output)
    return false;
  SwkbdConfig keyboard{};
  if (R_FAILED(swkbdCreate(&keyboard, 0)))
    return false;
  if (password)
    swkbdConfigMakePresetPassword(&keyboard);
  else
    swkbdConfigMakePresetDefault(&keyboard);
  const std::string header_owned(m_localization.Translate(header));
  const std::string initial_owned(initial);
  const std::string subtext_owned(m_localization.Translate(subtext));
  const std::string guide_owned(m_localization.Translate(guide));
  if (!header.empty())
    swkbdConfigSetHeaderText(&keyboard, header_owned.c_str());
  if (!initial.empty())
    swkbdConfigSetInitialText(&keyboard, initial_owned.c_str());
  if (!subtext.empty())
    swkbdConfigSetSubText(&keyboard, subtext_owned.c_str());
  if (!guide.empty())
    swkbdConfigSetGuideText(&keyboard, guide_owned.c_str());
  std::array<char, 1024> buffer{};
  swkbdConfigSetStringLenMax(&keyboard, buffer.size() - 1);
  const Result result = swkbdShow(&keyboard, buffer.data(), buffer.size());
  swkbdClose(&keyboard);
  if (R_FAILED(result) || (!allow_empty && buffer[0] == '\0'))
    return false;
  *output = buffer.data();
  return true;
}

void Launcher::LoadSourcesAndShares()
{
  m_sources.clear();
  m_usb_source_bindings.clear();
  m_usb_locations = Storage::ListUsbLocations();
  std::unordered_set<std::string> source_identities;
  const int source_count = std::clamp(m_store.GetInt("Library/SourceCount", 0), 0, 16);
  for (int index = 0; index < source_count; ++index)
  {
    const std::string prefix = "Library/Source" + std::to_string(index);
    const std::string stored_path = NormalizePath(m_store.Get(prefix));
    const std::string usb_id = m_store.Get(prefix + "UsbId");
    std::string usb_relative = NormalizePath(m_store.Get(prefix + "UsbPath"));
    while (!usb_relative.empty() && usb_relative.front() == '/')
      usb_relative.erase(usb_relative.begin());
    std::string path = stored_path;
    if (!usb_id.empty())
    {
      const std::string root = Storage::ResolveUsbPath(usb_id);
      if (!root.empty())
        path = NormalizePath(JoinPath(root, usb_relative));
      else
        path = UnavailableUsbSourcePath(usb_id, usb_relative);
    }
    const std::string source_identity =
        usb_id.empty() ? Lower(path) : "usb:" + Lower(usb_id) + "/" + Lower(usb_relative);
    if (!path.empty() && source_identities.insert(source_identity).second)
    {
      m_sources.push_back(path);
      if (!usb_id.empty())
        m_usb_source_bindings.emplace(Lower(path), std::pair{usb_id, usb_relative});
    }
  }

  m_shares.clear();
  std::unordered_set<std::string> share_ids;
  const int share_count = std::clamp(m_store.GetInt("Storage/SmbCount", 0), 0, 8);
  for (int index = 0; index < share_count; ++index)
  {
    const std::string prefix = "Storage/Smb" + std::to_string(index);
    Storage::SmbShare share;
    share.id = m_store.Get(prefix + "Id");
    share.name = m_store.Get(prefix + "Name");
    share.server = m_store.Get(prefix + "Server");
    share.share = m_store.Get(prefix + "Share");
    share.path = m_store.Get(prefix + "Path");
    share.user = m_store.Get(prefix + "User");
    share.password = m_store.Get(prefix + "Password");
    share.domain = m_store.Get(prefix + "Domain");
    share.auto_mount = m_store.GetBool(prefix + "AutoMount", true);
    if (!Storage::SmbRootPath(share.id).empty() && !share.server.empty() && !share.share.empty() &&
        share_ids.insert(share.id).second)
      m_shares.push_back(std::move(share));
  }

  m_usb_generation = Storage::UsbStatusGeneration();
  m_usb_locations = Storage::ListUsbLocations();
  LoadLibraryIdentities();
  LoadLibraryOrganization();
  StartAutoMountShares();
}

void Launcher::StartAutoMountShares()
{
  StopAutoMountShares();
  std::vector<Storage::SmbShare> shares;
  for (const Storage::SmbShare& share : m_shares)
  {
    if (share.auto_mount)
      shares.push_back(share);
  }
  if (shares.empty())
    return;
  auto state = std::make_shared<SmbAutoMountState>();
  m_smb_auto_mount = state;
  m_smb_auto_mount_thread = std::thread([state, shares = std::move(shares)] {
    for (const Storage::SmbShare& share : shares)
    {
      if (state->cancel.load(std::memory_order_acquire))
        break;
      std::string error;
      if (Storage::MountSmb(share, &error, &state->cancel))
      {
        std::lock_guard lock(state->mutex);
        state->mounted_roots.push_back(Storage::SmbRootPath(share.id));
      }
      SDL_Event wake{};
      wake.type = SDL_USEREVENT;
      wake.user.code = 0x534d424d;  // SMBM: SMB mount state changed.
      SDL_PushEvent(&wake);
    }
    state->complete.store(true, std::memory_order_release);
    SDL_Event wake{};
    wake.type = SDL_USEREVENT;
    wake.user.code = 0x534d424d;
    SDL_PushEvent(&wake);
  });
}

void Launcher::StopAutoMountShares()
{
  if (m_smb_auto_mount)
    m_smb_auto_mount->cancel.store(true, std::memory_order_release);
  if (m_smb_auto_mount_thread.joinable())
    m_smb_auto_mount_thread.join();
  m_smb_auto_mount.reset();
}

void Launcher::PumpAutoMountShares()
{
  const std::shared_ptr<SmbAutoMountState> state = m_smb_auto_mount;
  if (!state)
    return;
  std::deque<std::string> roots;
  {
    std::lock_guard lock(state->mutex);
    roots.swap(state->mounted_roots);
  }
  for (const std::string& root : roots)
  {
    for (const std::string& source : m_sources)
    {
      if (PathAtOrBelow(source, root))
        m_pending_scan_sources.push_back(source);
    }
  }
  if (!roots.empty())
  {
    std::ranges::sort(m_pending_scan_sources);
    m_pending_scan_sources.erase(
        std::unique(m_pending_scan_sources.begin(), m_pending_scan_sources.end()),
        m_pending_scan_sources.end());
  }
  if (state->complete.load(std::memory_order_acquire))
  {
    if (m_smb_auto_mount_thread.joinable())
      m_smb_auto_mount_thread.join();
    m_smb_auto_mount.reset();
  }
}

void Launcher::StartUsbInitialization()
{
  if (m_usb_initialization || m_usb_initialization_thread.joinable())
    return;
  auto state = std::make_shared<UsbInitializationState>();
  m_usb_initialization = state;
  m_usb_initialization_thread = std::thread([state] {
    state->success = Storage::InitializeUsb(&state->error);
    state->complete.store(true, std::memory_order_release);
    SDL_Event wake{};
    wake.type = SDL_USEREVENT;
    wake.user.code = 0x55534249;  // USBI: asynchronous USB initialization completed.
    SDL_PushEvent(&wake);
  });
}

void Launcher::StopUsbInitialization()
{
  if (m_usb_initialization_thread.joinable())
    m_usb_initialization_thread.join();
  m_usb_initialization.reset();
}

void Launcher::PumpUsbInitialization()
{
  const std::shared_ptr<UsbInitializationState> state = m_usb_initialization;
  if (!state || !state->complete.load(std::memory_order_acquire))
    return;
  if (m_usb_initialization_thread.joinable())
    m_usb_initialization_thread.join();
  // USB is optional. Keep SD/SMB usable when usb:hs is unavailable instead of interrupting startup
  // with an error dialog; File Manager will simply have no USB roots.
  m_usb_initialization.reset();
}

void Launcher::SaveSources()
{
  m_usb_locations = Storage::ListUsbLocations();
  std::vector<std::string> normalized_sources;
  std::vector<std::pair<std::string, std::string>> normalized_bindings;
  std::unordered_set<std::string> identities;
  for (const std::string& source : m_sources)
  {
    std::string path = NormalizePath(source);
    std::pair<std::string, std::string> binding;
    const auto existing = m_usb_source_bindings.find(Lower(path));
    if (existing != m_usb_source_bindings.end())
      binding = existing->second;
    else
    {
      for (const Storage::Location& location : m_usb_locations)
      {
        if (!PathAtOrBelow(path, location.path))
          continue;
        const std::string normalized_root = NormalizePath(location.path);
        std::string relative = path.substr(normalized_root.size());
        while (!relative.empty() && relative.front() == '/')
          relative.erase(relative.begin());
        binding = {location.id, relative};
        break;
      }
    }
    const std::string identity = binding.first.empty() ?
                                     Lower(path) :
                                     "usb:" + Lower(binding.first) + "/" + Lower(binding.second);
    if (path.empty() || !identities.insert(identity).second || normalized_sources.size() >= 16)
      continue;
    // The runtime map is path-keyed for fast hotplug lookup. If topology churn temporarily gives
    // this stable source the same mutable alias as another record, retain it under its unique
    // unavailable placeholder instead of overwriting either binding.
    if (!binding.first.empty() && std::ranges::any_of(normalized_sources, [&](const auto& saved) {
          return Lower(saved) == Lower(path);
        }))
    {
      path = UnavailableUsbSourcePath(binding.first, binding.second);
    }
    normalized_sources.emplace_back(std::move(path));
    normalized_bindings.emplace_back(std::move(binding));
  }
  m_sources = std::move(normalized_sources);
  m_store.RemovePrefix("Library/Source");
  m_store.SetInt("Library/SourceCount", m_sources.size());
  std::unordered_map<std::string, std::pair<std::string, std::string>> bindings;
  for (std::size_t index = 0; index < m_sources.size(); ++index)
  {
    const std::string prefix = "Library/Source" + std::to_string(index);
    const std::string& source = m_sources[index];
    const auto& binding = normalized_bindings[index];
    m_store.Set(prefix, source);
    if (!binding.first.empty())
    {
      m_store.Set(prefix + "UsbId", binding.first);
      m_store.Set(prefix + "UsbPath", binding.second);
      bindings.emplace(Lower(source), binding);
    }
  }
  m_usb_source_bindings = std::move(bindings);
  MarkStoreDirty();
}

void Launcher::SaveShares()
{
  m_store.RemovePrefix("Storage/Smb");
  m_store.SetInt("Storage/SmbCount", m_shares.size());
  for (std::size_t index = 0; index < m_shares.size(); ++index)
  {
    const Storage::SmbShare& share = m_shares[index];
    const std::string prefix = "Storage/Smb" + std::to_string(index);
    m_store.Set(prefix + "Id", share.id);
    m_store.Set(prefix + "Name", share.name);
    m_store.Set(prefix + "Server", share.server);
    m_store.Set(prefix + "Share", share.share);
    m_store.Set(prefix + "Path", share.path);
    m_store.Set(prefix + "User", share.user);
    m_store.Set(prefix + "Password", share.password);
    m_store.Set(prefix + "Domain", share.domain);
    m_store.SetBool(prefix + "AutoMount", share.auto_mount);
  }
  MarkStoreDirty();
}

void Launcher::ReplaceSavedPathPrefix(const std::string& old_path, const std::string& new_path)
{
  const std::string old_normalized = NormalizePath(old_path);
  const std::string new_normalized = NormalizePath(new_path);
  if (old_normalized.empty() || new_normalized.empty())
    return;
  const std::string old_identity = Lower(old_normalized);
  bool sources_changed = false;
  bool games_changed = false;
  for (std::string& source : m_sources)
  {
    const std::string normalized = NormalizePath(source);
    const std::string identity = Lower(normalized);
    if (identity == old_identity)
    {
      source = new_normalized;
      sources_changed = true;
    }
    else if (identity.size() > old_identity.size() && identity.starts_with(old_identity) &&
             (old_identity.back() == '/' || identity[old_identity.size()] == '/'))
    {
      source = NormalizePath(new_normalized + normalized.substr(old_normalized.size()));
      sources_changed = true;
    }
  }
  if (!m_clipboard_path.empty() && PathAtOrBelow(m_clipboard_path, old_normalized))
  {
    const std::string clipboard = NormalizePath(m_clipboard_path);
    m_clipboard_path = NormalizePath(new_normalized + clipboard.substr(old_normalized.size()));
  }
  for (Game& game : m_games)
  {
    if (game.installed_nand || !PathAtOrBelow(game.path, old_normalized))
      continue;
    const std::string old_game_path = NormalizePath(game.path);
    game.path = NormalizePath(new_normalized + old_game_path.substr(old_normalized.size()));
    game.canonical_path = CanonicalLibraryPath(game.path);
    const auto identity =
        std::ranges::find(m_library_identities, game.key, &LibraryIdentityRecord::id);
    if (identity != m_library_identities.end())
    {
      RememberPreviousLibraryPath(&*identity, old_game_path);
      identity->canonical_path = game.canonical_path;
      identity->current_path = game.path;
    }
    games_changed = true;
  }
  if (sources_changed)
    SaveSources();
  if (games_changed)
  {
    SaveLibraryIdentities();
    m_library_refresh_requested = true;
  }
  if (sources_changed || games_changed)
  {
    FlushPendingSaves();
  }
}

void Launcher::RemoveSavedPathsBelow(const std::string& root)
{
  const std::size_t previous_size = m_sources.size();
  std::erase_if(m_sources, [&](const std::string& path) { return PathAtOrBelow(path, root); });
  if (m_sources.size() != previous_size)
  {
    SaveSources();
    FlushPendingSaves();
  }
  if (!m_clipboard_path.empty() && PathAtOrBelow(m_clipboard_path, root))
  {
    m_clipboard_path.clear();
    m_clipboard_move = false;
  }
}

void Launcher::EnsureSourceMountedAtStartup(const std::string& path)
{
  bool changed = false;
  for (Storage::SmbShare& share : m_shares)
  {
    if (PathAtOrBelow(path, Storage::SmbRootPath(share.id)) && !share.auto_mount)
    {
      share.auto_mount = true;
      changed = true;
    }
  }
  if (changed)
  {
    SaveShares();
    FlushPendingSaves();
  }
}

bool Launcher::RefreshConfiguredUsbSources()
{
  if (m_usb_source_bindings.empty() && !std::ranges::any_of(m_sources, IsUsbStoragePath))
    return false;
  m_usb_locations = Storage::ListUsbLocations();
  const auto previous_bindings = m_usb_source_bindings;
  std::vector<std::pair<std::string, std::string>> stable_bindings(m_sources.size());
  bool changed = false;

  // Phase one snapshots every source's stable identity before changing any spelling. Looking up
  // and updating the path-keyed map in one loop is unsafe when ums aliases swap: inserting A's
  // new ums0 key could overwrite B's old ums0 entry before B has been processed.
  for (std::size_t index = 0; index < m_sources.size(); ++index)
  {
    const std::string path = NormalizePath(m_sources[index]);
    const auto existing = previous_bindings.find(Lower(path));
    if (existing != previous_bindings.end())
    {
      stable_bindings[index] = existing->second;
      continue;
    }
    if (!IsUsbStoragePath(path))
      continue;

    // One-time migration for launcher.ini files written before stable USB identities existed.
    struct stat source_info{};
    if (::stat(path.c_str(), &source_info) == 0 && S_ISDIR(source_info.st_mode))
    {
      for (const Storage::Location& location : m_usb_locations)
      {
        if (!PathAtOrBelow(path, location.path))
          continue;
        const std::string root = NormalizePath(location.path);
        std::string relative = NormalizePath(path).substr(root.size());
        while (!relative.empty() && relative.front() == '/')
          relative.erase(relative.begin());
        stable_bindings[index] = {location.id, std::move(relative)};
        changed = true;
        break;
      }
      continue;
    }
    const std::size_t colon = path.find(':');
    std::string relative = colon == std::string::npos ? std::string{} : path.substr(colon + 1);
    while (!relative.empty() && relative.front() == '/')
      relative.erase(relative.begin());
    std::vector<std::string> matches;
    const Storage::Location* matched_location = nullptr;
    for (const Storage::Location& location : m_usb_locations)
    {
      const std::string candidate = NormalizePath(location.path + relative);
      struct stat candidate_info{};
      if (::stat(candidate.c_str(), &candidate_info) == 0 && S_ISDIR(candidate_info.st_mode))
      {
        matches.push_back(candidate);
        matched_location = &location;
      }
    }
    if (matches.size() == 1 && matched_location)
    {
      stable_bindings[index] = {matched_location->id, std::move(relative)};
      changed = true;
    }
  }

  // Phase two resolves every stable identity against the same USB snapshot, then atomically
  // replaces the source vector and lookup map. A duplicate mutable spelling is represented by a
  // stable placeholder, so both saved records survive until their volumes have distinct aliases.
  std::vector<std::string> refreshed_sources;
  refreshed_sources.reserve(m_sources.size());
  std::unordered_map<std::string, std::pair<std::string, std::string>> refreshed_bindings;
  std::unordered_set<std::string> source_identities;
  std::unordered_set<std::string> occupied_paths;
  for (std::size_t index = 0; index < m_sources.size(); ++index)
  {
    const std::string old_path = NormalizePath(m_sources[index]);
    const auto& binding = stable_bindings[index];
    std::string path = old_path;
    std::string identity = Lower(path);
    if (!binding.first.empty())
    {
      identity = "usb:" + Lower(binding.first) + "/" + Lower(binding.second);
      const std::string root = Storage::ResolveUsbPath(binding.first);
      path = root.empty() ? UnavailableUsbSourcePath(binding.first, binding.second) :
                            NormalizePath(JoinPath(root, binding.second));
      if (occupied_paths.contains(Lower(path)))
        path = UnavailableUsbSourcePath(binding.first, binding.second);
    }
    if (path.empty() || occupied_paths.contains(Lower(path)) ||
        !source_identities.insert(identity).second)
    {
      changed = true;
      continue;
    }
    changed |= Lower(path) != Lower(old_path);
    refreshed_sources.emplace_back(path);
    occupied_paths.insert(Lower(path));
    if (!binding.first.empty())
      refreshed_bindings.emplace(Lower(path), binding);
  }
  changed |= refreshed_sources.size() != m_sources.size();
  m_sources = std::move(refreshed_sources);
  m_usb_source_bindings = std::move(refreshed_bindings);
  if (changed)
  {
    SaveSources();
    FlushPendingSaves();
  }
  return changed;
}

void Launcher::LoadLibraryIdentities()
{
  m_library_identities.clear();
  m_library_identities_dirty = false;
  std::unordered_set<std::string> ids;
  const int count = std::clamp(m_store.GetInt("Library/IdentityCount", 0), 0, 16384);
  for (int index = 0; index < count; ++index)
  {
    const std::string prefix = "Library/Identity" + std::to_string(index);
    LibraryIdentityRecord record;
    record.id = m_store.Get(prefix + "Id");
    record.fingerprint = m_store.Get(prefix + "Fingerprint");
    record.base_identity = m_store.Get(prefix + "BaseIdentity");
    record.canonical_path = m_store.Get(prefix + "Path");
    record.current_path = m_store.Get(prefix + "CurrentPath");
    record.retired = m_store.GetBool(prefix + "Retired", false);
    const int previous_count = std::clamp(m_store.GetInt(prefix + "PreviousPathCount", 0), 0,
                                          static_cast<int>(MAX_PREVIOUS_LIBRARY_PATHS));
    for (int previous = 0; previous < previous_count; ++previous)
    {
      const std::string path =
          NormalizePath(m_store.Get(prefix + "PreviousPath" + std::to_string(previous)));
      if (!path.empty() && std::ranges::none_of(record.previous_paths, [&](const auto& existing) {
            return Lower(existing) == Lower(path);
          }))
      {
        record.previous_paths.emplace_back(path);
      }
    }
    const bool valid_id = !record.id.empty() && record.id.size() <= 96 &&
                          std::ranges::all_of(record.id, [](unsigned char character) {
                            return std::isalnum(character) || character == '-' || character == '_';
                          });
    if (valid_id && !record.fingerprint.empty() && ids.insert(record.id).second)
      m_library_identities.emplace_back(std::move(record));
  }
}

void Launcher::SaveLibraryIdentities()
{
  m_store.RemovePrefix("Library/Identity");
  m_store.SetInt("Library/IdentityCount", static_cast<int>(m_library_identities.size()));
  for (std::size_t index = 0; index < m_library_identities.size(); ++index)
  {
    const std::string prefix = "Library/Identity" + std::to_string(index);
    const LibraryIdentityRecord& record = m_library_identities[index];
    m_store.Set(prefix + "Id", record.id);
    m_store.Set(prefix + "Fingerprint", record.fingerprint);
    m_store.Set(prefix + "BaseIdentity", record.base_identity);
    m_store.Set(prefix + "Path", record.canonical_path);
    m_store.Set(prefix + "CurrentPath", record.current_path);
    m_store.SetBool(prefix + "Retired", record.retired);
    m_store.SetInt(prefix + "PreviousPathCount", static_cast<int>(record.previous_paths.size()));
    for (std::size_t previous = 0; previous < record.previous_paths.size(); ++previous)
      m_store.Set(prefix + "PreviousPath" + std::to_string(previous),
                  record.previous_paths[previous]);
  }
  m_library_identities_dirty = false;
  MarkStoreDirty();
}

std::string Launcher::CanonicalLibraryPath(std::string_view input) const
{
  const std::string path = NormalizePath(std::string(input));
  for (const Storage::Location& location : m_usb_locations)
  {
    if (!PathAtOrBelow(path, location.path))
      continue;
    const std::string root = NormalizePath(location.path);
    std::string relative = path.substr(std::min(path.size(), root.size()));
    while (!relative.empty() && relative.front() == '/')
      relative.erase(relative.begin());
    return "usb:" + location.id + "/" + Lower(relative);
  }
  for (const Storage::SmbShare& share : m_shares)
  {
    const std::string root = Storage::SmbRootPath(share.id);
    if (!PathAtOrBelow(path, root))
      continue;
    std::string relative = path.substr(std::min(path.size(), NormalizePath(root).size()));
    while (!relative.empty() && relative.front() == '/')
      relative.erase(relative.begin());
    return "smb:" + share.id + "/" + Lower(relative);
  }
  return Lower(path);
}

bool Launcher::LibraryIdentityPathExists(const LibraryIdentityRecord& record) const
{
  if (record.retired)
    return false;
  const auto regular_file = [](const std::string& path) {
    struct stat info{};
    return !path.empty() && ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
  };
  if (regular_file(record.current_path) &&
      CanonicalLibraryPath(record.current_path) == record.canonical_path)
  {
    return true;
  }

  const std::size_t slash = record.canonical_path.find('/');
  if (slash == std::string::npos)
    return false;
  std::string root;
  if (record.canonical_path.starts_with("usb:"))
    root = Storage::ResolveUsbPath(record.canonical_path.substr(4, slash - 4));
  else if (record.canonical_path.starts_with("smb:"))
    root = Storage::SmbRootPath(record.canonical_path.substr(4, slash - 4));
  if (root.empty())
    return false;

  std::string relative = record.canonical_path.substr(slash + 1);
  const std::size_t current_colon = record.current_path.find(':');
  if (current_colon != std::string::npos)
  {
    std::string current_relative = record.current_path.substr(current_colon + 1);
    while (!current_relative.empty() && current_relative.front() == '/')
      current_relative.erase(current_relative.begin());
    if (Lower(current_relative) == relative)
      relative = std::move(current_relative);
  }
  return regular_file(JoinPath(root, relative));
}

std::string Launcher::GameFingerprint(const UICommon::GameFile& metadata) const
{
  std::string hash;
  // GetSyncHash may read an entire multi-gigabyte disc image.  Stable disc identity already has a
  // game/title ID, platform, disc and revision, and deliberately survives patched/repacked images,
  // so hashing those files only made every first scan dramatically slower.  Anonymous executables
  // still need a content discriminator because they have no reliable title identity.
  if (metadata.GetPlatform() == DiscIO::Platform::ELFOrDOL ||
      (metadata.GetGameID().empty() && metadata.GetTitleID() == 0))
  {
    const auto digest = metadata.GetSyncHash();
    static constexpr char HEX[] = "0123456789abcdef";
    hash.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest)
    {
      hash += HEX[byte >> 4];
      hash += HEX[byte & 0xf];
    }
  }
  // Keep the legacy tuple prefix stable so existing identity records migrate without a reset.
  return metadata.GetGameID() + ":" + std::to_string(metadata.GetRevision()) + ":" +
         std::to_string(metadata.GetDiscNumber()) + ":" +
         std::to_string(static_cast<int>(metadata.GetPlatform())) + ":" +
         std::to_string(metadata.GetFileSize()) + ":" + hash;
}

std::string Launcher::GameBaseIdentity(const UICommon::GameFile& metadata) const
{
  // The title ID adds precision for WADs, while platform/disc/revision distinguish multi-disc and
  // revision-specific configurations. No filename, size, timestamp, or content hash belongs here:
  // those are expected to change for patched games such as BetterWW.
  if (metadata.GetGameID().empty() && metadata.GetTitleID() == 0)
    return "v2-anonymous:" + Hex64(HashPath(GameFingerprint(metadata)));
  return "v2:" + metadata.GetGameID() + ":" + Hex64(metadata.GetTitleID()) + ":" +
         std::to_string(static_cast<int>(metadata.GetPlatform())) + ":" +
         std::to_string(metadata.GetDiscNumber()) + ":" + std::to_string(metadata.GetRevision());
}

void Launcher::AssignStableIdentity(Game* game)
{
  if (!game || game->installed_nand)
    return;
  if (game->canonical_path.empty())
    game->canonical_path = CanonicalLibraryPath(game->path);
  if (game->fingerprint.empty())
    game->fingerprint = GameFingerprint(*game->metadata);
  if (game->base_identity.empty())
    game->base_identity = GameBaseIdentity(*game->metadata);

  const std::string legacy_base = LegacyBaseIdentityFromFingerprint(game->fingerprint);
  const auto base_compatible = [&](const LibraryIdentityRecord& record) {
    if (!record.base_identity.empty())
      return record.base_identity == game->base_identity;
    if (game->base_identity.starts_with("v2-anonymous:"))
      return record.fingerprint == game->fingerprint;
    // Records from 1.0.3 and earlier did not persist BaseIdentity. Their fingerprint contains the
    // same platform/disc/revision tuple, so migrate it once and never accept path alone.
    const std::string record_legacy = LegacyBaseIdentityFromFingerprint(record.fingerprint);
    return !legacy_base.empty() && record_legacy == legacy_base;
  };

  LibraryIdentityRecord* match = nullptr;
  bool identity_changed = false;
  for (LibraryIdentityRecord& record : m_library_identities)
  {
    if (record.retired || m_claimed_library_ids.contains(record.id) ||
        record.canonical_path != game->canonical_path)
      continue;
    if (base_compatible(record))
    {
      match = &record;
      break;
    }
    // A different title now occupies this path. It must not receive path-derived settings,
    // controls, artwork, or a legacy forwarder identity. Keep the canonical scope for recovery,
    // but retire resolution so an already-installed stable forwarder cannot boot the replacement.
    game->allow_legacy_path_migration = false;
    RememberPreviousLibraryPath(&record, record.current_path);
    record.current_path.clear();
    record.retired = true;
    identity_changed = true;
    m_reserved_library_ids.erase(record.id);
  }
  if (!match)
  {
    const std::string game_scope = LibraryIdentityScope(game->canonical_path);
    for (LibraryIdentityRecord& record : m_library_identities)
    {
      const bool fingerprint_compatible =
          record.fingerprint == game->fingerprint ||
          (!game->fingerprint.empty() && game->fingerprint.back() == ':' &&
           record.fingerprint.starts_with(game->fingerprint));
      if (m_claimed_library_ids.contains(record.id) || game_scope.empty() ||
          LibraryIdentityScope(record.canonical_path) != game_scope || !fingerprint_compatible ||
          !base_compatible(record))
        continue;
      // All live records are reserved without touching the filesystem at scan startup. Only a
      // rare fingerprint-based rename candidate needs a stat; exact canonical matches above are
      // still immediate. This removes one potentially-networked stat per library entry from every
      // launch while keeping identical copies from stealing one another's IDs.
      if (m_reserved_library_ids.contains(record.id))
      {
        if (LibraryIdentityPathExists(record))
          continue;
        m_reserved_library_ids.erase(record.id);
      }
      match = &record;
      break;
    }
  }
  if (!match)
  {
    std::string id = StableIdStem(game->game_id, game->fingerprint);
    const std::string stem = id;
    unsigned collision = 1;
    const auto id_exists = [&](std::string_view candidate) {
      return std::ranges::any_of(m_library_identities, [&](const LibraryIdentityRecord& record) {
        return record.id == candidate;
      });
    };
    while (id_exists(id))
      id = stem + "-" + std::to_string(++collision);
    LibraryIdentityRecord record;
    record.id = std::move(id);
    record.fingerprint = game->fingerprint;
    record.base_identity = game->base_identity;
    record.canonical_path = game->canonical_path;
    record.current_path = NormalizePath(game->path);
    m_library_identities.emplace_back(std::move(record));
    match = &m_library_identities.back();
    identity_changed = true;
  }
  else
  {
    // Patched/repacked content with a compatible base tuple legitimately keeps its identity.
    const std::string current_path = NormalizePath(game->path);
    if (!match->current_path.empty() &&
        Lower(NormalizePath(match->current_path)) != Lower(current_path))
    {
      RememberPreviousLibraryPath(match, match->current_path);
    }
    identity_changed = match->fingerprint != game->fingerprint ||
                       match->base_identity != game->base_identity ||
                       match->canonical_path != game->canonical_path ||
                       match->current_path != current_path || match->retired;
    match->fingerprint = game->fingerprint;
    match->base_identity = game->base_identity;
    match->canonical_path = game->canonical_path;
    match->current_path = current_path;
    match->retired = false;
  }
  // Populate the field when migrating a record written by a launcher version that only stored the
  // canonical key.
  if (match->current_path.empty())
  {
    match->current_path = NormalizePath(game->path);
    identity_changed = true;
  }
  m_library_identities_dirty |= identity_changed;
  game->key = match->id;
  m_reserved_library_ids.erase(game->key);
  m_claimed_library_ids.insert(game->key);
  if (game->canonical_path.starts_with("usb:"))
    game->storage_id = game->canonical_path.substr(0, game->canonical_path.find('/'));
  else if (game->canonical_path.starts_with("smb:"))
    game->storage_id = game->canonical_path.substr(0, game->canonical_path.find('/'));
  else
    game->storage_id = DeviceName(game->path);
}

void Launcher::MigrateLegacyGameState(Game* game)
{
  if (!game || !game->allow_legacy_path_migration || game->legacy_key.empty() ||
      game->legacy_key == game->key)
    return;
  const auto migrate_store_value = [&](std::string_view group) {
    const std::string old_key = std::string(group) + "/" + game->legacy_key;
    const std::string new_key = std::string(group) + "/" + game->key;
    const std::string old_value = m_store.Get(old_key);
    if (m_store.Get(new_key).empty() && !old_value.empty())
      m_store.Set(new_key, old_value);
    if (!old_value.empty())
      m_store.Remove(old_key);
  };
  migrate_store_value("Alias");
  migrate_store_value("Recent");
  migrate_store_value("Favorite");

  const std::string old_cover = std::string(COVER_DIRECTORY) + "/" + game->legacy_key + ".png";
  const std::string new_cover = CoverPath(*game);
  if (!RegularFileExists(new_cover) && RegularFileExists(old_cover))
    File::Rename(old_cover, new_cover);

  const std::string entries = File::GetUserPath(D_GAMESETTINGS_IDX) + "Entries/";
  const std::string old_ini = entries + Hex64(HashPath(Lower(NormalizePath(game->path)))) + ".ini";
  const std::string new_ini = entries + game->key + ".ini";
  if (!RegularFileExists(new_ini) && RegularFileExists(old_ini))
  {
    File::CreateFullPath(new_ini);
    File::Rename(old_ini, new_ini);
  }
  MarkStoreDirty();
}

void Launcher::LoadLibraryOrganization()
{
  // Collection and search are transient views. Always open a fresh launcher on the complete
  // library, while favorites and collection membership themselves remain persistent.
  const bool had_saved_view =
      !m_store.Get("Library/ActiveCollection").empty() || !m_store.Get("Library/Search").empty();
  m_active_collection.clear();
  m_search_query.clear();
  m_store.Remove("Library/ActiveCollection");
  m_store.Remove("Library/Search");
  if (had_saved_view)
    MarkStoreDirty();
  m_favorites.clear();
  const int favorite_count = std::clamp(m_store.GetInt("Library/FavoriteCount", 0), 0, 16384);
  for (int index = 0; index < favorite_count; ++index)
  {
    const std::string id = m_store.Get("Library/Favorite" + std::to_string(index));
    if (!id.empty())
      m_favorites.insert(id);
  }
  m_collections.clear();
  const int collection_count = std::clamp(m_store.GetInt("Library/CollectionCount", 0), 0, 128);
  for (int index = 0; index < collection_count; ++index)
  {
    const std::string prefix = "Library/Collection" + std::to_string(index);
    Collection collection;
    collection.name = m_store.Get(prefix + "Name");
    std::string members = m_store.Get(prefix + "Members");
    for (std::size_t start = 0; start <= members.size();)
    {
      const std::size_t separator = members.find(',', start);
      const std::string member = members.substr(
          start, separator == std::string::npos ? std::string::npos : separator - start);
      if (!member.empty())
        collection.members.insert(member);
      if (separator == std::string::npos)
        break;
      start = separator + 1;
    }
    if (!collection.name.empty())
      m_collections.emplace_back(std::move(collection));
  }
}

void Launcher::SaveCollections()
{
  m_store.RemovePrefix("Library/Favorite");
  m_store.SetInt("Library/FavoriteCount", static_cast<int>(m_favorites.size()));
  std::size_t favorite_index = 0;
  for (const std::string& id : m_favorites)
    m_store.Set("Library/Favorite" + std::to_string(favorite_index++), id);
  m_store.RemovePrefix("Library/Collection");
  m_store.SetInt("Library/CollectionCount", static_cast<int>(m_collections.size()));
  for (std::size_t index = 0; index < m_collections.size(); ++index)
  {
    const std::string prefix = "Library/Collection" + std::to_string(index);
    m_store.Set(prefix + "Name", m_collections[index].name);
    std::string members;
    for (const std::string& id : m_collections[index].members)
    {
      if (!members.empty())
        members += ',';
      members += id;
    }
    m_store.Set(prefix + "Members", std::move(members));
  }
  MarkStoreDirty();
}

void Launcher::RebuildVisibleGames()
{
  m_visible_games.clear();
  const std::string query = Lower(Trim(m_search_query));
  const Collection* active = nullptr;
  if (!m_active_collection.empty() && m_active_collection != "favorites")
  {
    const auto found = std::ranges::find(m_collections, m_active_collection, &Collection::name);
    if (found != m_collections.end())
      active = &*found;
  }
  for (std::size_t index = 0; index < m_games.size(); ++index)
  {
    const Game& game = m_games[index];
    if (m_active_collection == "favorites" && !m_favorites.contains(game.key))
      continue;
    if (active && !active->members.contains(game.key))
      continue;
    if (!query.empty())
    {
      const std::string searchable =
          Lower(game.title + " " + game.game_id + " " + game.platform + " " + game.path);
      if (searchable.find(query) == std::string::npos)
        continue;
    }
    m_visible_games.push_back(index);
  }
}

Game* Launcher::VisibleGame(int index)
{
  return index >= 0 && index < static_cast<int>(m_visible_games.size()) ?
             &m_games[m_visible_games[index]] :
             nullptr;
}

void Launcher::StartGameScan(std::vector<std::string> sources, bool replace)
{
  StopGameScan();
  if (replace)
    CancelQueuedCoverDecodes();
  RefreshConfiguredUsbSources();
  sources = replace ? m_sources : std::move(sources);
  m_usb_locations = Storage::ListUsbLocations();
  m_reserved_library_ids.clear();
  for (const LibraryIdentityRecord& record : m_library_identities)
  {
    if (!record.retired)
      m_reserved_library_ids.insert(record.id);
  }

  std::unordered_set<std::uint64_t> unaffected_wad_titles;
  if (replace)
  {
    for (Game& game : m_games)
    {
      if (game.cover)
        SDL_DestroyTexture(game.cover);
    }
    m_games.clear();
    m_visible_games.clear();
    m_cover_use = 0;
    m_claimed_library_ids.clear();
  }
  else
  {
    m_claimed_library_ids.clear();
    // Records belonging to the roots being refreshed must remain available so incoming entries
    // update their existing IDs. Every unaffected live game is claimed up front; otherwise a new
    // byte-identical file from a partial USB/SMB scan could steal another game's fingerprint ID.
    std::erase_if(m_games, [&](Game& game) {
      if (game.installed_nand)
        return false;
      const bool in_target = std::ranges::any_of(
          sources, [&](const std::string& source) { return PathAtOrBelow(game.path, source); });
      if (in_target && game.cover)
        SDL_DestroyTexture(game.cover);
      return in_target;
    });
    for (const Game& game : m_games)
    {
      if (game.installed_nand)
        continue;
      m_claimed_library_ids.insert(game.key);
      if (game.metadata && game.metadata->GetPlatform() == DiscIO::Platform::WiiWAD &&
          game.title_id != 0)
        unaffected_wad_titles.insert(game.title_id);
    }
    RebuildVisibleGames();
  }

  auto state = std::make_shared<LibraryScanState>();
  state->full = replace;
  if (!replace)
  {
    for (const std::string& source : sources)
    {
      const auto binding = m_usb_source_bindings.find(Lower(NormalizePath(source)));
      if (binding != m_usb_source_bindings.end())
        state->target_usb_ids.insert(binding->second.first);
    }
    for (const std::string& id : state->target_usb_ids)
    {
      if (std::ranges::any_of(m_usb_locations,
                              [&](const Storage::Location& location) { return location.id == id; }))
        m_unavailable_usb_ids.erase(id);
    }
  }
  std::vector<std::pair<std::string, std::string>> usb_roots;
  for (const Storage::Location& location : m_usb_locations)
    usb_roots.emplace_back(location.id, NormalizePath(location.path));
  std::vector<std::pair<std::string, std::string>> smb_roots;
  for (const Storage::SmbShare& share : m_shares)
    smb_roots.emplace_back(share.id, NormalizePath(Storage::SmbRootPath(share.id)));
  std::unordered_multimap<std::string, std::pair<std::string, std::string>> known_fingerprints;
  known_fingerprints.reserve(m_library_identities.size());
  for (const LibraryIdentityRecord& record : m_library_identities)
  {
    if (!record.canonical_path.empty() && !record.fingerprint.empty())
      known_fingerprints.emplace(record.canonical_path,
                                 std::pair{record.base_identity, record.fingerprint});
  }
  // Load entries from the saved collection first. This is especially important when Dolphin was
  // closed while a collection was active: its first page should appear immediately instead of
  // waiting for unrelated games earlier in a large source tree.
  std::unordered_set<std::string> priority_ids;
  if (m_active_collection == "favorites")
  {
    priority_ids = m_favorites;
  }
  else if (!m_active_collection.empty())
  {
    const auto collection =
        std::ranges::find(m_collections, m_active_collection, &Collection::name);
    if (collection != m_collections.end())
      priority_ids = collection->members;
  }
  std::vector<std::string> priority_paths;
  priority_paths.reserve(priority_ids.size());
  for (const LibraryIdentityRecord& record : m_library_identities)
  {
    if (record.retired || record.current_path.empty() || !priority_ids.contains(record.id))
      continue;
    if (std::ranges::any_of(sources, [&](const std::string& source) {
          return PathAtOrBelow(record.current_path, source);
        }))
    {
      priority_paths.push_back(record.current_path);
    }
  }
  const std::size_t first_page_size = static_cast<std::size_t>(std::max(1, GridPageSize()));
  m_library_scan = state;
  m_library_refresh_requested = false;
  m_library_scan_thread = std::thread(
      [this, state, sources = std::move(sources), usb_roots = std::move(usb_roots),
       smb_roots = std::move(smb_roots), known_fingerprints = std::move(known_fingerprints),
       unaffected_wad_titles = std::move(unaffected_wad_titles),
       priority_paths = std::move(priority_paths), first_page_size] {
        const auto canonical_path = [&](std::string_view input) {
          const std::string path = NormalizePath(std::string(input));
          for (const auto& [id, root] : usb_roots)
          {
            if (!PathAtOrBelow(path, root))
              continue;
            std::string relative = path.substr(std::min(path.size(), root.size()));
            while (!relative.empty() && relative.front() == '/')
              relative.erase(relative.begin());
            return "usb:" + id + "/" + Lower(relative);
          }
          for (const auto& [id, root] : smb_roots)
          {
            if (!PathAtOrBelow(path, root))
              continue;
            std::string relative = path.substr(std::min(path.size(), root.size()));
            while (!relative.empty() && relative.front() == '/')
              relative.erase(relative.begin());
            return "smb:" + id + "/" + Lower(relative);
          }
          return Lower(path);
        };
        if (state->full)
        {
          m_game_cache.Clear(UICommon::GameFileCache::DeleteOnDisk::No);
          m_game_cache.Load();
        }

        std::unordered_set<std::uint64_t> source_wad_titles = std::move(unaffected_wad_titles);
        std::vector<std::string> all_paths;
        std::unordered_set<std::string> seen_paths;
        const auto process_path = [&](const std::string& path) {
          if (state->cancel.load(std::memory_order_acquire) || DiscIO::ShouldHideFromGameList(path))
            return;
          bool cache_changed = false;
          std::shared_ptr<const UICommon::GameFile> metadata =
              m_game_cache.AddOrGet(path, &cache_changed, false);
          state->cache_changed |= cache_changed;
          if (!metadata || !metadata->IsValid())
            return;

          Game game;
          game.metadata = metadata;
          game.path = metadata->GetFilePath();
          game.game_id = metadata->GetGameID();
          game.game_tdb_id = metadata->GetGameTDBID();
          game.title_id = metadata->GetTitleID();
          game.revision = metadata->GetRevision();
          game.title = metadata->GetName(UICommon::GameFile::Variant::LongAndPossiblyCustom);
          if (game.title.empty())
            game.title = metadata->GetFileName();
          game.region = metadata->GetRegion();
          switch (metadata->GetPlatform())
          {
          case DiscIO::Platform::GameCubeDisc:
            game.platform = "GameCube";
            break;
          case DiscIO::Platform::WiiDisc:
            game.platform = "Wii";
            break;
          case DiscIO::Platform::WiiWAD:
            game.platform = "WiiWare / VC";
            if (game.title_id != 0)
              source_wad_titles.insert(game.title_id);
            break;
          case DiscIO::Platform::Triforce:
            game.platform = "Triforce";
            break;
          case DiscIO::Platform::ELFOrDOL:
            game.platform = "Executable";
            break;
          default:
            game.platform = "Unknown";
            break;
          }
          game.legacy_key = (game.game_id.empty() ? "game" : game.game_id) + "-" + [&] {
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "%04x-%016llx", game.revision,
                          static_cast<unsigned long long>(HashPath(Lower(game.path))));
            return std::string(suffix);
          }();
          game.canonical_path = canonical_path(game.path);
          game.base_identity = GameBaseIdentity(*metadata);
          game.metadata_refreshed = cache_changed;
          if (!cache_changed)
          {
            const auto [first, last] = known_fingerprints.equal_range(game.canonical_path);
            const auto known = std::find_if(first, last, [&](const auto& entry) {
              if (!entry.second.first.empty())
                return entry.second.first == game.base_identity;
              return LegacyBaseIdentityFromFingerprint(entry.second.second) ==
                     LegacyBaseIdentityFromFingerprint(GameFingerprint(*metadata));
            });
            if (known != last)
              game.fingerprint = known->second.second;
          }
          if (game.fingerprint.empty())
            game.fingerprint = GameFingerprint(*metadata);
          struct stat info{};
          if (::stat(game.path.c_str(), &info) == 0)
            game.modified = info.st_mtime;
          {
            std::lock_guard lock(state->mutex);
            state->ready.emplace_back(std::move(game));
          }
          const std::size_t processed =
              state->processed.fetch_add(1, std::memory_order_acq_rel) + 1;
          // Wake immediately for the first game and first complete page, then coalesce
          // notifications. Sorting/rebuilding the visible list once per file was the dominant cost
          // in large caches.
          if (processed <= first_page_size || processed % 32 == 0)
          {
            SDL_Event wake{};
            wake.type = SDL_USEREVENT;
            wake.user.code = 0x444c5343;  // DLSC: Dolphin library scan changed.
            SDL_PushEvent(&wake);
          }
        };

        for (const std::string& path : priority_paths)
        {
          if (state->cancel.load(std::memory_order_acquire))
            break;
          const std::string normalized = Lower(NormalizePath(path));
          if (normalized.empty() || !seen_paths.insert(normalized).second ||
              DiscIO::ShouldHideFromGameList(path))
          {
            continue;
          }
          all_paths.emplace_back(path);
          state->discovered.store(all_paths.size(), std::memory_order_release);
          process_path(path);
        }

        // Enumerate and publish one source at a time. A slow SMB source must not hold back games
        // already found on SD or USB, while all_paths is still retained for the final cache prune.
        for (const std::string& source : sources)
        {
          if (state->cancel.load(std::memory_order_acquire))
            break;
          WalkGamePaths(source, state->cancel, [&](std::string path) {
            if (DiscIO::ShouldHideFromGameList(path))
              return;
            if (!seen_paths.insert(Lower(NormalizePath(path))).second)
              return;
            all_paths.emplace_back(path);
            state->discovered.store(all_paths.size(), std::memory_order_release);
            process_path(path);
          });
        }

        if (state->full && !state->cancel.load(std::memory_order_acquire))
        {
          // AddOrGet already refreshed every discovered file. The final pass only prunes stale
          // cache paths; re-statting every game (and Riivolution dependency) here doubled scan I/O.
          state->cache_changed |= m_game_cache.Update(all_paths, {}, {}, state->cancel, false);
        }
        if (state->cache_changed && !state->cancel.load(std::memory_order_acquire))
          m_game_cache.Save();

        if (!state->cancel.load(std::memory_order_acquire))
        {
          for (const Tools::InstalledTitle& title : Tools::ListInstalledWiiTitles())
          {
            if (state->cancel.load(std::memory_order_acquire))
              break;
            if (!title.bootable || title.system_title || source_wad_titles.contains(title.title_id))
              continue;
            Game game;
            game.title_id = title.title_id;
            game.title = title.name;
            game.game_id = title.game_id;
            game.game_tdb_id = title.game_tdb_id;
            game.revision = title.revision;
            game.modified = title.modified;
            game.region = title.region;
            game.platform = "WiiWare / VC";
            game.installed_nand = true;
            char key[32];
            std::snprintf(key, sizeof(key), "nand-%016llx",
                          static_cast<unsigned long long>(title.title_id));
            game.key = key;
            std::lock_guard lock(state->mutex);
            state->ready.emplace_back(std::move(game));
          }
        }
        state->complete.store(true, std::memory_order_release);
        SDL_Event wake{};
        wake.type = SDL_USEREVENT;
        wake.user.code = 0x444c5343;
        SDL_PushEvent(&wake);
      });
}

void Launcher::StopGameScan()
{
  if (m_library_scan)
    m_library_scan->cancel.store(true, std::memory_order_release);
  if (m_library_scan_thread.joinable())
    m_library_scan_thread.join();
  m_library_scan.reset();
  // IDs are assigned as progressive results reach the UI.  A scan can be cancelled because the
  // user launches a game, edits storage, or closes Dolphin before its normal completion path.
  // Persist those already-published records so settings and generated shortcuts never reference
  // an identity which only existed in memory.
  if (m_library_identities_dirty)
  {
    SaveLibraryIdentities();
    FlushPendingSaves();
  }
}

void Launcher::PumpGameScan()
{
  const std::shared_ptr<LibraryScanState> state = m_library_scan;
  if (!state)
    return;
  std::deque<Game> ready;
  {
    std::lock_guard lock(state->mutex);
    // Keep publication bounded so metadata insertion and list maintenance cannot monopolize an
    // SDL frame. The worker wakes each first-page result, so the launcher becomes interactive
    // immediately and fills that page progressively.
    constexpr std::size_t batch_limit = 2;
    const std::size_t count = std::min(state->ready.size(), batch_limit);
    for (std::size_t index = 0; index < count; ++index)
    {
      ready.emplace_back(std::move(state->ready.front()));
      state->ready.pop_front();
    }
  }
  std::size_t published = 0;
  for (Game& game : ready)
  {
    if (game.canonical_path.empty())
      game.canonical_path = CanonicalLibraryPath(game.path);
    if (game.canonical_path.starts_with("usb:"))
    {
      const std::size_t slash = game.canonical_path.find('/');
      const std::string usb_id =
          game.canonical_path.substr(4, slash == std::string::npos ? std::string::npos : slash - 4);
      if (m_unavailable_usb_ids.contains(usb_id))
        continue;
    }
    if (!game.installed_nand)
    {
      if (game.metadata && game.metadata->GetPlatform() == DiscIO::Platform::WiiWAD &&
          game.title_id != 0)
      {
        std::erase_if(m_games, [&](Game& existing) {
          if (!existing.installed_nand || existing.title_id != game.title_id)
            return false;
          if (existing.cover)
            SDL_DestroyTexture(existing.cover);
          return true;
        });
      }
      AssignStableIdentity(&game);
      MigrateLegacyGameState(&game);
      game.config_override_path = EntryGameIniPath(game);
    }
    const std::string alias = m_store.Get("Alias/" + game.key);
    if (!alias.empty())
    {
      game.title = alias;
      game.has_custom_title = true;
    }
    game.played = m_store.GetInt("Recent/" + game.key, 0);
    game.has_game_config = RegularFileExists(GameIniPath(game));

    const auto existing = std::ranges::find(m_games, game.key, &Game::key);
    if (existing == m_games.end())
    {
      m_games.emplace_back(std::move(game));
      ++published;
    }
    else
    {
      const bool metadata_changed =
          game.metadata_refreshed || (!game.fingerprint.empty() && !existing->fingerprint.empty() &&
                                      game.fingerprint != existing->fingerprint);
      if (metadata_changed)
      {
        if (existing->cover)
          SDL_DestroyTexture(existing->cover);
      }
      else
      {
        game.cover = existing->cover;
        game.cover_use = existing->cover_use;
        game.cover_request = existing->cover_request;
        game.cover_loaded_at = existing->cover_loaded_at;
        game.cover_attempted = existing->cover_attempted;
        game.cover_queued = existing->cover_queued;
      }
      *existing = std::move(game);
      ++published;
    }
  }
  if (published != 0)
  {
    state->unsorted_published += published;
    const std::size_t first_page = static_cast<std::size_t>(std::max(1, GridPageSize()));
    if (m_games.size() <= first_page || state->unsorted_published >= 16)
    {
      SortGames();
      state->unsorted_published = 0;
    }
    else
    {
      RebuildVisibleGames();
    }
  }

  bool queue_empty = false;
  {
    std::lock_guard lock(state->mutex);
    queue_empty = state->ready.empty();
  }
  if (!queue_empty)
  {
    SDL_Event wake{};
    wake.type = SDL_USEREVENT;
    wake.user.code = 0x444c5343;
    SDL_PushEvent(&wake);
  }
  if (!state->complete.load(std::memory_order_acquire) || !queue_empty)
    return;
  if (m_library_scan_thread.joinable())
    m_library_scan_thread.join();
  if (!state->cancel.load(std::memory_order_acquire))
  {
    SaveLibraryIdentities();
    SortGames();
  }
  m_library_scan.reset();
  m_library_refresh_requested = false;
}

void Launcher::ScanGames()
{
  StartGameScan(m_sources, true);
}

[[maybe_unused]] void Launcher::ScanGamesLegacy()
{
  RefreshConfiguredUsbSources();
  m_usb_locations = Storage::ListUsbLocations();
  m_claimed_library_ids.clear();
  for (Game& game : m_games)
  {
    if (game.cover)
      SDL_DestroyTexture(game.cover);
  }
  m_games.clear();
  m_cover_use = 0;

  ClearBackground();
  DrawHeader("Dolphin");
  DrawTextCentered(m_font_large, m_width / 2, m_height / 2 - 60, "Scanning game library...",
                   m_text);
  DrawTextCentered(m_font_small, m_width / 2, m_height / 2 + 8,
                   "SD, USB and connected SMB sources are indexed with Dolphin metadata.", m_dim);
  SDL_RenderPresent(m_renderer);

  std::vector<std::string_view> source_views;
  source_views.reserve(m_sources.size());
  for (const std::string& source : m_sources)
    source_views.emplace_back(source);
  const std::vector<std::string> paths = UICommon::FindAllGamePaths(source_views, true);
  m_game_cache.Load();
  if (m_game_cache.Update(paths))
    m_game_cache.Save();
  std::unordered_set<std::string> path_set(paths.begin(), paths.end());
  std::unordered_set<std::uint64_t> source_wad_titles;
  m_game_cache.ForEach([&](const std::shared_ptr<const UICommon::GameFile>& metadata) {
    if (!metadata || !metadata->IsValid() || !path_set.contains(metadata->GetFilePath()))
      return;
    Game game;
    game.metadata = metadata;
    game.path = metadata->GetFilePath();
    game.game_id = metadata->GetGameID();
    game.game_tdb_id = metadata->GetGameTDBID();
    game.title_id = metadata->GetTitleID();
    game.revision = metadata->GetRevision();
    game.title = metadata->GetName(UICommon::GameFile::Variant::LongAndPossiblyCustom);
    if (game.title.empty())
      game.title = metadata->GetFileName();
    game.region = metadata->GetRegion();
    switch (metadata->GetPlatform())
    {
    case DiscIO::Platform::GameCubeDisc:
      game.platform = "GameCube";
      break;
    case DiscIO::Platform::WiiDisc:
      game.platform = "Wii";
      break;
    case DiscIO::Platform::WiiWAD:
      game.platform = "WiiWare / VC";
      if (game.title_id != 0)
        source_wad_titles.insert(game.title_id);
      break;
    case DiscIO::Platform::Triforce:
      game.platform = "Triforce";
      break;
    case DiscIO::Platform::ELFOrDOL:
      game.platform = "Executable";
      break;
    default:
      game.platform = "Unknown";
      break;
    }
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "-%04x-%016llx", game.revision,
                  static_cast<unsigned long long>(HashPath(Lower(game.path))));
    game.legacy_key = (game.game_id.empty() ? "game" : game.game_id) + suffix;
    AssignStableIdentity(&game);
    MigrateLegacyGameState(&game);
    const std::string alias = m_store.Get("Alias/" + game.key);
    if (!alias.empty())
    {
      game.title = alias;
      game.has_custom_title = true;
    }
    game.played = m_store.GetInt("Recent/" + game.key, 0);
    struct stat info{};
    if (::stat(game.path.c_str(), &info) == 0)
      game.modified = info.st_mtime;
    m_games.emplace_back(std::move(game));
  });

  for (const Tools::InstalledTitle& title : Tools::ListInstalledWiiTitles())
  {
    if (!title.bootable || title.system_title || source_wad_titles.contains(title.title_id))
      continue;

    Game game;
    game.title_id = title.title_id;
    game.title = title.name;
    game.game_id = title.game_id;
    game.game_tdb_id = title.game_tdb_id;
    game.revision = title.revision;
    game.modified = title.modified;
    game.region = title.region;
    game.platform = "WiiWare / VC";
    game.installed_nand = true;
    char key[32];
    std::snprintf(key, sizeof(key), "nand-%016llx",
                  static_cast<unsigned long long>(title.title_id));
    game.key = key;
    const std::string alias = m_store.Get("Alias/" + game.key);
    if (!alias.empty())
    {
      game.title = alias;
      game.has_custom_title = true;
    }
    game.played = m_store.GetInt("Recent/" + game.key, 0);
    m_games.emplace_back(std::move(game));
  }

  std::unordered_map<std::string, std::size_t> game_id_counts;
  for (const Game& game : m_games)
  {
    if (!game.installed_nand && !game.game_id.empty())
      ++game_id_counts[game.game_id];
  }
  for (Game& game : m_games)
  {
    // Renamed mods and multiple images with the same internal disc ID need a path-specific layer.
    // Images without an ID also need this path because the regular Game ID INI has no filename.
    if (!game.installed_nand &&
        (game.has_custom_title || game.game_id.empty() || game_id_counts[game.game_id] > 1))
    {
      game.config_override_path = EntryGameIniPath(game);
    }
    game.has_game_config = RegularFileExists(GameIniPath(game));
  }
  SaveLibraryIdentities();
  SortGames();
  m_library_refresh_requested = false;
}

void Launcher::SortGames()
{
  std::ranges::sort(m_games, [&](const Game& left, const Game& right) {
    if (m_sort_mode == SortMode::RecentlyPlayed && left.played != right.played)
      return left.played > right.played;
    if (m_sort_mode == SortMode::RecentlyAdded && left.modified != right.modified)
      return left.modified > right.modified;
    return Lower(left.title) < Lower(right.title);
  });
  RebuildVisibleGames();
}

void Launcher::RenderMessage(std::string_view title, std::span<const std::string> lines,
                             bool localize_lines)
{
  const std::string localized_title{m_localization.Translate(title)};
  std::string message;
  for (const std::string& line : lines)
  {
    if (!message.empty())
      message += "\n\n";
    const std::string_view displayed_line =
        localize_lines ? m_localization.Translate(line) : std::string_view(line);
    message.append(displayed_line);
  }
  if (message.empty())
    message = std::string(m_localization.Translate("Unknown Dolphin error"));

  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    bool close = false;
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (event.type == SDL_CONTROLLERBUTTONDOWN &&
          (event.cbutton.button == BUTTON_CONFIRM || event.cbutton.button == BUTTON_CANCEL))
        close = true;
      if (event.type == SDL_KEYDOWN &&
          (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_ESCAPE))
        close = true;
      if (touch == TouchKind::Tap)
        close = true;
    }
    if (close)
      return;
    ClearBackground();
    const int panel_width = std::min(m_width - 96, 1080);
    const int maximum_panel_height = m_height - 80;
    const int body_width = panel_width - 96;
    const int line_height = std::max(32, TTF_FontHeight(m_font) + 8);
    const int maximum_lines = std::max(1, (maximum_panel_height - 170) / line_height);
    const std::vector<std::string> wrapped = WrapText(m_font, body_width, maximum_lines, message);
    const int body_height = std::max(line_height, static_cast<int>(wrapped.size()) * line_height);
    const int panel_height = std::clamp(170 + body_height, 250, maximum_panel_height);
    const int panel_x = (m_width - panel_width) / 2;
    const int panel_y = (m_height - panel_height) / 2;
    GlassPanel(panel_x, panel_y, panel_width, panel_height);
    Border(panel_x, panel_y, panel_width, panel_height, 3, m_selection);
    DrawTextCentered(m_font_large, m_width / 2, panel_y + 28,
                     Ellipsize(m_font_large, localized_title, body_width), m_selection);

    const int body_y = panel_y + 92;
    const int footer_y = panel_y + panel_height - 46;
    const SDL_Rect body_clip{panel_x + 40, body_y - 4, panel_width - 80,
                             std::max(1, footer_y - body_y - 8)};
    SDL_RenderSetClipRect(m_renderer, &body_clip);
    for (std::size_t index = 0; index < wrapped.size(); ++index)
    {
      DrawTextCentered(m_font, m_width / 2, body_y + static_cast<int>(index) * line_height,
                       wrapped[index], m_text);
    }
    SDL_RenderSetClipRect(m_renderer, nullptr);
    DrawTextCentered(m_font_small, m_width / 2, footer_y,
                     m_localization.Translate("Press A to continue"), m_dim);
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

void Launcher::Toast(std::string message, int milliseconds)
{
  // Toasts are launcher-owned status messages. Translate here, at the semantic boundary, so
  // arbitrary strings passed to the low-level text renderer are never treated as translation keys.
  message = std::string(m_localization.Translate(message));
  const Uint32 deadline = SDL_GetTicks() + milliseconds;
  do
  {
    ClearBackground();
    const int width = std::min(m_width - 80, std::max(820, TextWidth(m_font, message) + 72));
    const int height = 120;
    const int x = (m_width - width) / 2;
    const int y = (m_height - height) / 2;
    GlassPanel(x, y, width, height);
    Border(x, y, width, height, 2, m_highlight);
    DrawTextCentered(m_font, m_width / 2, y + 46, message, m_text);
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame(true);
  } while (m_running && appletMainLoop() && !SDL_TICKS_PASSED(SDL_GetTicks(), deadline));
}

bool Launcher::Confirm(std::string_view title, std::span<const std::string> lines,
                       bool localize_lines)
{
  const std::string_view localized_title = m_localization.Translate(title);
  const std::string yes_label = std::string(m_localization.Translate("Yes")) + "  (A)";
  const std::string no_label = std::string(m_localization.Translate("No")) + "  (B)";
  const int panel_width = std::min(m_width - 96, 1080);
  const int body_width = panel_width - 96;
  const int line_height = std::max(30, TTF_FontHeight(m_font) + 5);
  std::vector<std::string> wrapped_lines;
  for (const std::string& line : lines)
  {
    const std::string_view displayed_line =
        localize_lines ? m_localization.Translate(line) : std::string_view(line);
    if (displayed_line.empty())
    {
      wrapped_lines.emplace_back();
      continue;
    }
    std::vector<std::string> wrapped = WrapText(m_font, body_width, 4, displayed_line);
    wrapped_lines.insert(wrapped_lines.end(), std::make_move_iterator(wrapped.begin()),
                         std::make_move_iterator(wrapped.end()));
  }
  const int panel_height =
      std::clamp(218 + static_cast<int>(wrapped_lines.size()) * line_height, 290, m_height - 64);
  const int panel_x = (m_width - panel_width) / 2;
  const int panel_y = (m_height - panel_height) / 2;
  const int button_width = 210;
  const int button_height = 56;
  const int button_y = panel_y + panel_height - button_height - 22;
  const int yes_x = m_width / 2 - button_width - 18;
  const int no_x = m_width / 2 + 18;
  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == BUTTON_CONFIRM)
          return true;
        else if (event.cbutton.button == BUTTON_CANCEL)
          return false;
      }
      if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_RETURN)
          return true;
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          return false;
      }
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= button_y && touch_y < button_y + button_height)
        {
          if (touch_x >= yes_x && touch_x < yes_x + button_width)
            return true;
          if (touch_x >= no_x && touch_x < no_x + button_width)
            return false;
        }
      }
    }
    ClearBackground();
    GlassPanel(panel_x, panel_y, panel_width, panel_height);
    Border(panel_x, panel_y, panel_width, panel_height, 3, SDL_Color{210, 70, 70, 255});
    DrawTextCentered(m_font_large, m_width / 2, panel_y + 34,
                     Ellipsize(m_font_large, localized_title, body_width),
                     SDL_Color{235, 120, 120, 255});
    int y = panel_y + 108;
    const int body_bottom = button_y - 18;
    SDL_Rect body_clip{panel_x + 36, y - 4, panel_width - 72, std::max(1, body_bottom - y)};
    SDL_RenderSetClipRect(m_renderer, &body_clip);
    for (const std::string& line : wrapped_lines)
    {
      DrawTextCentered(m_font, m_width / 2, y, line, m_text);
      y += line_height;
    }
    SDL_RenderSetClipRect(m_renderer, nullptr);
    FillRect(yes_x, button_y, button_width, button_height, SDL_Color{150, 50, 50, 255});
    Border(yes_x, button_y, button_width, button_height, 2, SDL_Color{215, 95, 95, 255});
    DrawTextCentered(m_font, yes_x + button_width / 2,
                     button_y + (button_height - TTF_FontHeight(m_font)) / 2, yes_label, m_text);
    FillRect(no_x, button_y, button_width, button_height, SDL_Color{48, 54, 64, 255});
    Border(no_x, button_y, button_width, button_height, 2, m_dim);
    DrawTextCentered(m_font, no_x + button_width / 2,
                     button_y + (button_height - TTF_FontHeight(m_font)) / 2, no_label, m_text);
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  return false;
}

int Launcher::Dropdown(std::string_view title, const std::vector<std::string>& choices, int current,
                       bool localize_title, bool localize_choices)
{
  if (choices.empty())
    return -1;
  const int count = static_cast<int>(choices.size());
  int selection = std::clamp(current, 0, count - 1);
  int top = 0;
  constexpr int row_height = 52;
  int visible = std::clamp((m_height - 200) / row_height, 1, count);
  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, count, visible))
        continue;
      if (touch == TouchKind::Tap)
      {
        const int panel_width = m_width > 760 ? 760 : m_width - 160;
        const int panel_x = (m_width - panel_width) / 2;
        const int list_y = (m_height - (90 + visible * row_height)) / 2 + 70;
        for (int row = 0; row < visible && top + row < count; ++row)
        {
          const int row_y = list_y + row * row_height;
          if (touch_x >= panel_x && touch_x < panel_x + panel_width && touch_y >= row_y &&
              touch_y < row_y + row_height)
            return top + row;
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == BUTTON_CONFIRM)
          return selection;
        if (event.cbutton.button == BUTTON_CANCEL)
          return current;
      }
      else if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_RETURN)
          return selection;
        if (event.key.keysym.sym == SDLK_ESCAPE)
          return current;
      }
      if (selection < top)
        top = selection;
      if (selection >= top + visible)
        top = selection - visible + 1;
    }

    ClearBackground();
    const int panel_width = m_width > 760 ? 760 : m_width - 160;
    const int panel_height = 90 + visible * row_height;
    const int panel_x = (m_width - panel_width) / 2;
    const int panel_y = (m_height - panel_height) / 2;
    GlassPanel(panel_x, panel_y, panel_width, panel_height);
    Border(panel_x, panel_y, panel_width, panel_height, 3, m_selection);
    const std::string_view displayed_title =
        localize_title ? m_localization.Translate(title) : title;
    DrawTextCentered(m_font_large, m_width / 2, panel_y + 18, displayed_title, m_value);
    const int list_y = panel_y + 70;
    for (int row = 0; row < visible && top + row < count; ++row)
    {
      const int index = top + row;
      const int y = list_y + row * row_height;
      const bool selected = index == selection;
      if (selected)
      {
        FillRect(panel_x + 8, y, panel_width - 16, row_height - 4, m_focus);
        FillRect(panel_x + 8, y, 5, row_height - 4, m_selection);
      }
      const std::string_view displayed_choice =
          localize_choices ? m_localization.Translate(choices[index]) : choices[index];
      DrawText(m_font, panel_x + 34, y + (row_height - TTF_FontHeight(m_font)) / 2,
               displayed_choice, selected ? m_value : m_text);
    }
    if (count > visible)
    {
      const int track_height = visible * row_height;
      const int track_x = panel_x + panel_width - 12;
      FillRect(track_x, list_y, 4, track_height, SDL_Color{40, 44, 54, 255});
      const int thumb_height = track_height * visible / count;
      const int denominator = std::max(1, count - visible);
      FillRect(track_x, list_y + (track_height - thumb_height) * top / denominator, 4, thumb_height,
               m_selection);
    }
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  return current;
}

int Launcher::SelectChoice(std::string_view title, std::span<const std::string_view> choices,
                           int current, int delta)
{
  if (choices.empty())
    return -1;
  const int count = static_cast<int>(choices.size());
  current = std::clamp(current, 0, count - 1);
  if (delta != 0 || count <= 2)
    return (current + (delta < 0 ? -1 : 1) + count) % count;

  std::vector<std::string> labels;
  labels.reserve(choices.size());
  for (const std::string_view choice : choices)
    labels.emplace_back(choice);
  return Dropdown(title, labels, current);
}

int Launcher::RunRows(std::string_view title, std::string_view context,
                      const std::function<std::vector<Row>()>& rows_provider,
                      const std::function<bool(int, int)>& action, bool touch_activates_full_row,
                      std::function<bool(int)> reset, std::function<bool(int)> resettable)
{
  const std::string position_key = std::string(title) + '\n' + std::string(context);
  const auto saved_position = m_row_positions.find(position_key);
  int selection = saved_position == m_row_positions.end() ? 0 : saved_position->second.first;
  int top = saved_position == m_row_positions.end() ? 0 : saved_position->second.second;
  const auto finish = [&](int result) {
    m_row_positions[position_key] = {selection, top};
    return result;
  };
  constexpr int row_height = 46;
  constexpr int list_top = 118;
  const int visible = std::max(1, (m_height - list_top - 72) / row_height);
  BeginScreenFx();
  while (BeginFrame())
  {
    std::vector<Row> rows = rows_provider();
    if (rows.empty())
      return finish(-1);
    selection = std::clamp(selection, 0, static_cast<int>(rows.size()) - 1);
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, static_cast<int>(rows.size()), visible))
        continue;
      if ((touch == TouchKind::SwipeLeft || touch == TouchKind::SwipeRight) &&
          rows[selection].enabled && rows[selection].adjustable)
      {
        action(selection, touch == TouchKind::SwipeLeft ? -1 : 1);
        continue;
      }
      if (touch == TouchKind::Tap)
      {
        if (touch_y < (m_width >= 1600 ? 112 : 80) || touch_y >= m_height - 40)
          return finish(-1);
        const int index = top + (touch_y - list_top) / row_height;
        const int column_width = std::min(980, m_width - 180);
        const int column_x = (m_width - column_width) / 2;
        if (touch_x >= column_x && touch_x < column_x + column_width && touch_y >= list_top &&
            index >= 0 && index < static_cast<int>(rows.size()))
        {
          selection = index;
          if ((touch_activates_full_row || touch_x >= column_x + column_width / 2) &&
              rows[index].enabled && action(index, 0))
            return finish(index);
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction != 0)
      {
        int next = selection;
        do
        {
          next = (next + direction + static_cast<int>(rows.size())) % rows.size();
        } while (!rows[next].enabled && next != selection);
        if (next != selection)
          selection = next;
      }
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT && rows[selection].enabled &&
            rows[selection].adjustable)
          action(selection, -1);
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT &&
                 rows[selection].enabled && rows[selection].adjustable)
          action(selection, 1);
        else if (event.cbutton.button == BUTTON_SETTINGS)
        {
          const SettingHelpInfo info = SettingHelpFor(title, rows[selection]);
          const std::string_view current = rows[selection].value == ">" ?
                                               std::string_view{} :
                                               std::string_view(rows[selection].value);
          ShowInfoCard(title, rows[selection].label, info.kind, info.description, current,
                       SettingScope(title, context), rows[selection].localize_label,
                       rows[selection].localize_value);
          BeginScreenFx();
        }
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_X && reset &&
                 rows[selection].enabled &&
                 (rows[selection].adjustable || (resettable && resettable(selection))))
        {
          if (reset(selection))
          {
            Toast("Setting reset to default", 550);
            BeginScreenFx();
          }
        }
        else if (event.cbutton.button == BUTTON_CONFIRM && rows[selection].enabled)
        {
          if (action(selection, 0))
            return finish(selection);
        }
        else if (event.cbutton.button == BUTTON_CANCEL)
          return finish(-1);
      }
      else if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_LEFT && rows[selection].enabled &&
            rows[selection].adjustable)
          action(selection, -1);
        else if (event.key.keysym.sym == SDLK_RIGHT && rows[selection].enabled &&
                 rows[selection].adjustable)
          action(selection, 1);
        else if (event.key.keysym.sym == SDLK_x)
        {
          const SettingHelpInfo info = SettingHelpFor(title, rows[selection]);
          const std::string_view current = rows[selection].value == ">" ?
                                               std::string_view{} :
                                               std::string_view(rows[selection].value);
          ShowInfoCard(title, rows[selection].label, info.kind, info.description, current,
                       SettingScope(title, context), rows[selection].localize_label,
                       rows[selection].localize_value);
          BeginScreenFx();
        }
        else if ((event.key.keysym.sym == SDLK_y || event.key.keysym.sym == SDLK_DELETE) && reset &&
                 rows[selection].enabled &&
                 (rows[selection].adjustable || (resettable && resettable(selection))))
        {
          if (reset(selection))
          {
            Toast("Setting reset to default", 550);
            BeginScreenFx();
          }
        }
        else if (event.key.keysym.sym == SDLK_RETURN && rows[selection].enabled)
        {
          if (action(selection, 0))
            return finish(selection);
        }
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          return finish(-1);
      }
    }
    if (selection < top)
      top = selection;
    if (selection >= top + visible)
      top = selection - visible + 1;

    ClearBackground();
    DrawHeader(title, context);
    const int column_width = std::min(980, m_width - 180);
    const int column_x = (m_width - column_width) / 2;
    const int label_x = column_x + 40;
    const int value_x = column_x + column_width - 40;
    GlassPanel(column_x - 12, list_top - 10, column_width + 24, visible * row_height + 18);
    const float target_y = static_cast<float>(list_top + (selection - top) * row_height + 1);
    m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                        target_y :
                        m_highlight_y + (target_y - m_highlight_y) * 0.30f;
    FillRect(column_x, static_cast<int>(m_highlight_y), column_width, row_height - 2, m_focus);
    FillRect(column_x, static_cast<int>(m_highlight_y), 5, row_height - 2, m_selection);
    for (int row = 0; row < visible && top + row < static_cast<int>(rows.size()); ++row)
    {
      const int index = top + row;
      const int slot_y = list_top + row * row_height;
      const int y = slot_y + (row_height - TTF_FontHeight(m_font)) / 2;
      const bool current = index == selection;
      const SDL_Color label_color = !rows[index].enabled    ? m_dim :
                                    rows[index].destructive ? SDL_Color{255, 120, 120, 255} :
                                    current                 ? m_value :
                                                              m_text;
      const std::string_view localized_label = rows[index].localize_label ?
                                                   m_localization.Translate(rows[index].label) :
                                                   std::string_view(rows[index].label);
      DrawText(m_font, label_x, y, Ellipsize(m_font, localized_label, column_width * 2 / 3),
               label_color);
      const std::string_view displayed_value = rows[index].localize_value ?
                                                   m_localization.Translate(rows[index].value) :
                                                   std::string_view(rows[index].value);
      DrawTextRight(
          m_font_small, value_x, y + (TTF_FontHeight(m_font) - TTF_FontHeight(m_font_small)) / 2,
          Ellipsize(m_font_small, displayed_value, column_width / 3), current ? m_value : m_dim);
    }
    if (static_cast<int>(rows.size()) > visible)
    {
      const int track_height = visible * row_height;
      const int track_x = column_x + column_width + 16;
      const int track_y = list_top - 2;
      FillRect(track_x, track_y, 4, track_height, SDL_Color{40, 44, 54, 255});
      const int thumb_height = std::max(16, track_height * visible / static_cast<int>(rows.size()));
      const int denominator = std::max(1, static_cast<int>(rows.size()) - visible);
      FillRect(track_x, track_y + (track_height - thumb_height) * top / denominator, 4,
               thumb_height, m_selection);
    }
    const bool has_adjustable_row =
        std::ranges::any_of(rows, [](const Row& row) { return row.enabled && row.adjustable; });
    const bool can_reset = reset && rows[selection].enabled &&
                           (rows[selection].adjustable || (resettable && resettable(selection)));
    if (has_adjustable_row && can_reset)
      DrawSettingsFooter(
          "Left / Right  Change       A  Choose       X  Info       Y  Reset       B  Back");
    else if (has_adjustable_row)
      DrawSettingsFooter("Left / Right  Change       A  Choose       X  Info       B  Back");
    else
      DrawSettingsFooter("A  Choose       X  Info       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  return finish(-1);
}

std::string Launcher::CoverPath(const Game& game) const
{
  return std::string(COVER_DIRECTORY) + "/" + game.key + ".png";
}

CoverDecodeResult Launcher::DecodeCover(const CoverDecodeJob& job)
{
  CoverDecodeResult result;
  result.key = job.key;
  result.request = job.request;
  result.epoch = job.epoch;
  SDL_Surface* surface = nullptr;
  bool custom_exists = RegularFileExists(job.custom_path);
  // Finish an interrupted atomic import before loading the cover. The normal path performs only
  // the existing cover stat; the backup check is needed solely when the active file is absent.
  if (!custom_exists && RegularFileExists(job.custom_path + ".old"))
  {
    (void)RecoverAtomicFile(job.custom_path);
    custom_exists = RegularFileExists(job.custom_path);
  }
  if (custom_exists)
    surface = IMG_Load(job.custom_path.c_str());
  if (!surface && job.metadata)
  {
    const UICommon::GameCover& cover = job.metadata->GetCoverImage();
    if (!cover.empty() && cover.buffer.size() <= static_cast<std::size_t>(INT_MAX))
    {
      if (SDL_RWops* stream =
              SDL_RWFromConstMem(cover.buffer.data(), static_cast<int>(cover.buffer.size())))
        surface = IMG_Load_RW(stream, 1);
    }
  }
  if (!surface || surface->w < 1 || surface->h < 1 || surface->w > 8192 || surface->h > 8192 ||
      static_cast<std::uint64_t>(surface->w) * static_cast<std::uint64_t>(surface->h) >
          16ULL * 1024 * 1024)
  {
    if (surface)
      SDL_FreeSurface(surface);
    return result;
  }
  constexpr int maximum_width = 360;
  constexpr int maximum_height = 540;
  int width = surface->w;
  int height = surface->h;
  if (width > maximum_width)
  {
    height = static_cast<int>(static_cast<long long>(height) * maximum_width / width);
    width = maximum_width;
  }
  if (height > maximum_height)
  {
    width = static_cast<int>(static_cast<long long>(width) * maximum_height / height);
    height = maximum_height;
  }
  if (width != surface->w || height != surface->h)
  {
    SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, std::max(1, width), std::max(1, height),
                                                         32, SDL_PIXELFORMAT_RGBA32);
    if (!scaled)
    {
      SDL_FreeSurface(surface);
      return result;
    }
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    SDL_GetSurfaceBlendMode(surface, &blend);
    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
    const bool copied = SDL_BlitScaled(surface, nullptr, scaled, nullptr) == 0;
    SDL_SetSurfaceBlendMode(surface, blend);
    SDL_FreeSurface(surface);
    if (!copied)
    {
      SDL_FreeSurface(scaled);
      return result;
    }
    surface = scaled;
  }
  SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
  SDL_FreeSurface(surface);
  if (!rgba)
    return result;
  const bool must_lock = SDL_MUSTLOCK(rgba);
  if (must_lock && SDL_LockSurface(rgba) != 0)
  {
    SDL_FreeSurface(rgba);
    return result;
  }
  result.width = rgba->w;
  result.height = rgba->h;
  result.pixels.resize(static_cast<std::size_t>(result.width) * result.height * 4);
  for (int row = 0; row < result.height; ++row)
  {
    std::memcpy(result.pixels.data() + static_cast<std::size_t>(row) * result.width * 4,
                static_cast<const Uint8*>(rgba->pixels) + static_cast<std::size_t>(row) * rgba->pitch,
                static_cast<std::size_t>(result.width) * 4);
  }
  if (must_lock)
    SDL_UnlockSurface(rgba);
  SDL_FreeSurface(rgba);
  return result;
}

void Launcher::CoverDecodeThread()
{
  for (;;)
  {
    CoverDecodeJob job;
    {
      std::unique_lock lock(m_cover_decode_mutex);
      m_cover_decode_condition.wait(lock, [&] {
        return m_cover_decode_stop ||
               (!m_cover_decode_jobs.empty() && m_cover_decode_ready.size() < COVER_READY_LIMIT);
      });
      if (m_cover_decode_stop)
        return;
      job = std::move(m_cover_decode_jobs.front());
      m_cover_decode_jobs.pop_front();
    }

    CoverDecodeResult result = DecodeCover(job);
    bool publish = false;
    {
      std::lock_guard lock(m_cover_decode_mutex);
      if (!m_cover_decode_stop && job.epoch == m_cover_decode_epoch)
      {
        m_cover_decode_ready.emplace_back(std::move(result));
        publish = true;
      }
    }
    if (publish)
    {
      SDL_Event wake{};
      wake.type = SDL_USEREVENT;
      wake.user.code = 0x434f5652;  // COVR: a decoded cover is ready for SDL upload.
      SDL_PushEvent(&wake);
    }
  }
}

void Launcher::StartCoverDecodeWorker()
{
  std::lock_guard lock(m_cover_decode_mutex);
  if (m_cover_decode_started)
    return;
  m_cover_decode_stop = false;
  m_cover_decode_started = true;
  m_cover_decode_thread = std::thread(&Launcher::CoverDecodeThread, this);
}

void Launcher::StopCoverDecodeWorker()
{
  {
    std::lock_guard lock(m_cover_decode_mutex);
    if (!m_cover_decode_started)
      return;
    m_cover_decode_stop = true;
    m_cover_decode_jobs.clear();
    m_cover_decode_ready.clear();
  }
  m_cover_decode_condition.notify_all();
  if (m_cover_decode_thread.joinable())
    m_cover_decode_thread.join();
  std::lock_guard lock(m_cover_decode_mutex);
  m_cover_decode_started = false;
}

void Launcher::CancelQueuedCoverDecodes()
{
  {
    std::lock_guard lock(m_cover_decode_mutex);
    ++m_cover_decode_epoch;
    m_cover_decode_jobs.clear();
    m_cover_decode_ready.clear();
  }
  for (Game& game : m_games)
  {
    game.cover_queued = false;
    game.cover_request = 0;
  }
  m_cover_decode_condition.notify_all();
}

void Launcher::QueueCoverDecode(Game* game, bool priority)
{
  if (!game || game->cover || game->cover_attempted)
    return;
  if (game->cover_queued)
  {
    if (priority)
    {
      std::lock_guard lock(m_cover_decode_mutex);
      const auto found = std::ranges::find(m_cover_decode_jobs, game->cover_request,
                                           &CoverDecodeJob::request);
      if (found != m_cover_decode_jobs.end() && found != m_cover_decode_jobs.begin())
      {
        CoverDecodeJob job = std::move(*found);
        m_cover_decode_jobs.erase(found);
        m_cover_decode_jobs.emplace_front(std::move(job));
        m_cover_decode_condition.notify_one();
      }
    }
    return;
  }
  if (m_cover_decode_budget <= 0)
    return;
  --m_cover_decode_budget;

  CoverDecodeJob job;
  job.key = game->key;
  job.custom_path = CoverPath(*game);
  job.metadata = game->metadata;
  job.request = ++m_cover_request_serial;
  game->cover_request = job.request;
  game->cover_queued = true;

  CoverDecodeJob dropped;
  bool did_drop = false;
  {
    std::lock_guard lock(m_cover_decode_mutex);
    job.epoch = m_cover_decode_epoch;
    if (m_cover_decode_jobs.size() >= COVER_JOB_LIMIT)
    {
      dropped = std::move(m_cover_decode_jobs.back());
      m_cover_decode_jobs.pop_back();
      did_drop = true;
    }
    if (priority)
      m_cover_decode_jobs.emplace_front(std::move(job));
    else
      m_cover_decode_jobs.emplace_back(std::move(job));
  }
  if (did_drop)
  {
    const auto old = std::ranges::find(m_games, dropped.key, &Game::key);
    if (old != m_games.end() && old->cover_request == dropped.request)
    {
      old->cover_queued = false;
      old->cover_request = 0;
    }
  }
  m_cover_decode_condition.notify_one();
}

SDL_Texture* Launcher::UploadCoverTexture(const CoverDecodeResult& result)
{
  if (!m_renderer || result.width < 1 || result.height < 1 || result.pixels.empty())
    return nullptr;
  SDL_Texture* texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC, result.width, result.height);
  if (texture && SDL_UpdateTexture(texture, nullptr, result.pixels.data(), result.width * 4) != 0)
  {
    SDL_DestroyTexture(texture);
    texture = nullptr;
  }
  if (!texture)
  {
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
        const_cast<Uint8*>(result.pixels.data()), result.width, result.height, 32,
        result.width * 4, SDL_PIXELFORMAT_RGBA32);
    if (surface)
    {
      texture = SDL_CreateTextureFromSurface(m_renderer, surface);
      SDL_FreeSurface(surface);
    }
  }
  if (texture)
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  return texture;
}

void Launcher::PumpCoverDecodeResults()
{
  int uploads = 0;
  int processed = 0;
  while (processed < 12)
  {
    CoverDecodeResult result;
    {
      std::lock_guard lock(m_cover_decode_mutex);
      if (m_cover_decode_ready.empty())
        break;
      if (!m_cover_decode_ready.front().pixels.empty() && uploads >= COVER_UPLOAD_BUDGET)
        break;
      result = std::move(m_cover_decode_ready.front());
      m_cover_decode_ready.pop_front();
    }
    m_cover_decode_condition.notify_one();
    ++processed;
    const auto game = std::ranges::find(m_games, result.key, &Game::key);
    if (game == m_games.end() || game->cover_request != result.request)
      continue;
    game->cover_queued = false;
    game->cover_attempted = true;
    if (result.pixels.empty())
      continue;
    SDL_Texture* texture = UploadCoverTexture(result);
    ++uploads;
    if (!texture)
      continue;
    if (std::ranges::count_if(m_games, [](const Game& item) { return item.cover != nullptr; }) >=
        COVER_CACHE_LIMIT)
    {
      EvictCover();
    }
    game->cover = texture;
    game->cover_use = ++m_cover_use;
    game->cover_loaded_at = SDL_GetTicks();
  }
}

SDL_Texture* Launcher::LoadScaledTexture(const std::string& path, int width, int height)
{
  SDL_Surface* source = IMG_Load(path.c_str());
  if (!source)
    return nullptr;
  SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, std::max(1, width), std::max(1, height),
                                                       32, SDL_PIXELFORMAT_RGBA32);
  if (!scaled)
  {
    SDL_FreeSurface(source);
    return nullptr;
  }
  SDL_BlendMode blend = SDL_BLENDMODE_NONE;
  SDL_GetSurfaceBlendMode(source, &blend);
  SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
  const bool copied = SDL_BlitScaled(source, nullptr, scaled, nullptr) == 0;
  SDL_SetSurfaceBlendMode(source, blend);
  SDL_FreeSurface(source);
  if (!copied)
  {
    SDL_FreeSurface(scaled);
    return nullptr;
  }
  SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, scaled);
  SDL_FreeSurface(scaled);
  if (texture)
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  return texture;
}

void Launcher::EvictCover()
{
  Game* victim = nullptr;
  for (Game& game : m_games)
  {
    if (game.cover && (!victim || game.cover_use < victim->cover_use))
      victim = &game;
  }
  if (!victim)
    return;
  SDL_DestroyTexture(victim->cover);
  victim->cover = nullptr;
  victim->cover_use = 0;
  victim->cover_attempted = false;
}

void Launcher::EnsureCover(Game* game, bool priority)
{
  if (!game)
    return;
  if (game->cover)
  {
    game->cover_use = ++m_cover_use;
    return;
  }
  QueueCoverDecode(game, priority);
}

void Launcher::ReloadCover(Game* game)
{
  if (!game)
    return;
  if (game->cover)
    SDL_DestroyTexture(game->cover);
  game->cover = nullptr;
  game->cover_use = 0;
  game->cover_request = 0;
  game->cover_attempted = false;
  game->cover_queued = false;
  m_cover_decode_budget = 1;
  EnsureCover(game, true);
}

int Launcher::GridColumns() const
{
  return m_grid_columns;
}

int Launcher::GridRows() const
{
  return m_grid_rows;
}

int Launcher::GridPageSize() const
{
  return GridColumns() * GridRows();
}

int Launcher::GridNavigate(int selection, int dx, int dy) const
{
  if (m_visible_games.empty())
    return 0;
  const int columns = GridColumns();
  const int rows = GridRows();
  const int per_page = columns * rows;
  const int page = selection / per_page;
  const int position = selection % per_page;
  const int row = position / columns;
  const int column = position % columns;
  if (dx > 0)
  {
    if (column + 1 < columns && selection + 1 < static_cast<int>(m_visible_games.size()))
      return selection + 1;
    if ((page + 1) * per_page < static_cast<int>(m_visible_games.size()))
      return std::min((page + 1) * per_page + row * columns,
                      static_cast<int>(m_visible_games.size()) - 1);
  }
  else if (dx < 0)
  {
    if (column > 0)
      return selection - 1;
    if (page > 0)
      return std::min((page - 1) * per_page + row * columns + columns - 1,
                      static_cast<int>(m_visible_games.size()) - 1);
  }
  else if (dy > 0 && row + 1 < rows &&
           selection + columns < static_cast<int>(m_visible_games.size()))
  {
    return selection + columns;
  }
  else if (dy < 0 && row > 0)
  {
    return selection - columns;
  }
  return selection;
}

int Launcher::GridPage(int selection, int direction) const
{
  if (m_visible_games.empty())
    return 0;
  const int per_page = GridPageSize();
  const int maximum_page = (static_cast<int>(m_visible_games.size()) - 1) / per_page;
  const int page = std::clamp(selection / per_page + direction, 0, maximum_page);
  return std::min(page * per_page + selection % per_page,
                  static_cast<int>(m_visible_games.size()) - 1);
}

int Launcher::GridHitTest(int x, int y, int page_start) const
{
  const bool large = m_width >= 1600;
  const int top = large ? 112 : 80;
  const int footer = large ? 54 : 38;
  const int gap_x = large ? 24 : 18;
  const int gap_y = large ? 18 : 14;
  const int title_height = m_show_titles ? (large ? 30 : 24) : 0;
  const int available_height = m_height - top - footer;
  const int caption = title_height ? title_height + 8 : 0;
  const int cover_height = std::max(
      72, (available_height - (GridRows() - 1) * gap_y - GridRows() * caption) / GridRows());
  const int automatic_width = cover_height * 2 / 3;
  const int margin = large ? 60 : 40;
  const int maximum_width = (m_width - margin * 2 - (GridColumns() - 1) * gap_x) / GridColumns();
  int cover_width = std::max(48, std::min(automatic_width, maximum_width));
  const int actual_height = std::min(cover_height, cover_width * 3 / 2);
  cover_width = actual_height * 2 / 3;
  const int grid_width = GridColumns() * cover_width + (GridColumns() - 1) * gap_x;
  const int x0 = (m_width - grid_width) / 2;
  const int grid_height = GridRows() * (actual_height + caption) + (GridRows() - 1) * gap_y;
  const int y0 = top + std::max(0, (available_height - grid_height) / 2);
  for (int row = 0; row < GridRows(); ++row)
  {
    for (int column = 0; column < GridColumns(); ++column)
    {
      const int index = page_start + row * GridColumns() + column;
      if (index >= static_cast<int>(m_visible_games.size()))
        continue;
      const int cell_x = x0 + column * (cover_width + gap_x);
      const int cell_y = y0 + row * (actual_height + caption + gap_y);
      if (x >= cell_x - 4 && x < cell_x + cover_width + 4 && y >= cell_y - 4 &&
          y < cell_y + actual_height + caption)
        return index;
    }
  }
  return -1;
}

void Launcher::RenderGrid(int selection)
{
  ClearBackground();
  m_cover_decode_budget = COVER_REQUEST_BUDGET;
  if (Game* selected = VisibleGame(selection))
    EnsureCover(selected, true);
  const bool large = m_width >= 1600;
  const int top = large ? 112 : 80;
  const int footer = large ? 54 : 38;
  const int gap_x = large ? 24 : 18;
  const int gap_y = large ? 18 : 14;
  const int title_height = m_show_titles ? (large ? 30 : 24) : 0;
  const int caption = title_height ? title_height + 8 : 0;
  const int available_height = m_height - top - footer;
  const int maximum_cover_height = std::max(
      72, (available_height - (GridRows() - 1) * gap_y - GridRows() * caption) / GridRows());
  const int automatic_width = maximum_cover_height * 2 / 3;
  const int margin = large ? 60 : 40;
  const int maximum_cover_width =
      (m_width - margin * 2 - (GridColumns() - 1) * gap_x) / GridColumns();
  int cover_width = std::max(48, std::min(automatic_width, maximum_cover_width));
  const int cover_height = std::min(maximum_cover_height, cover_width * 3 / 2);
  cover_width = cover_height * 2 / 3;
  const int grid_width = GridColumns() * cover_width + (GridColumns() - 1) * gap_x;
  const int x0 = (m_width - grid_width) / 2;
  const int grid_height = GridRows() * (cover_height + caption) + (GridRows() - 1) * gap_y;
  const int y0 = top + std::max(0, (available_height - grid_height) / 2);
  const int per_page = GridPageSize();
  const int page_start = m_visible_games.empty() ? 0 : selection / per_page * per_page;
  const int page_count = m_visible_games.empty() ?
                             1 :
                             (static_cast<int>(m_visible_games.size()) + per_page - 1) / per_page;
  const int page = m_visible_games.empty() ? 1 : selection / per_page + 1;

  const int band_height = y0 - 4;
  FillRect(0, 0, m_width, band_height, m_panel);
  if (!HasAnimatedBackground())
    FillRect(0, band_height, m_width, 2, m_selection);
  const int logo_size = band_height - 12;
  if (m_logo)
  {
    SDL_Rect logo{26, (band_height - logo_size) / 2, logo_size, logo_size};
    SDL_RenderCopy(m_renderer, m_logo, nullptr, &logo);
  }
  static constexpr std::array<std::string_view, 3> SORT_NAMES = {"A-Z", "Recently played",
                                                                 "Recently added"};
  std::string status =
      std::to_string(m_visible_games.empty() ? 0 : selection + 1) + " / " +
      std::to_string(m_visible_games.size()) + "   ·   " +
      std::string(m_localization.Translate("Page")) + " " + std::to_string(page) + " / " +
      std::to_string(page_count) + "   ·   " + std::string(m_localization.Translate("Sort:")) +
      " " + std::string(m_localization.Translate(SORT_NAMES[static_cast<int>(m_sort_mode)]));
  if (!m_active_collection.empty())
    status += "   ·   " + (m_active_collection == "favorites" ?
                               std::string(m_localization.Translate("Favorites")) :
                               m_active_collection);
  if (!m_search_query.empty())
    status += "   ·   " + std::string(m_localization.Translate("Search:")) + " " + m_search_query;
  DrawTextCentered(m_font, m_width / 2, (band_height - TTF_FontHeight(m_font)) / 2, status,
                   m_value);
  const int status_right = m_width / 2 + TextWidth(m_font, status) / 2;
  const int maximum_width = (m_width - 34) - (status_right + 24);
  DrawScrollingTextRight(
      m_font_small, m_width - 34, (band_height - TTF_FontHeight(m_font_small)) / 2, maximum_width,
      m_visible_games.empty() ? std::string(m_localization.Translate("No game selected")) :
                                GameLocationLabel(*VisibleGame(selection)),
      m_dim);

  for (int row = 0; row < GridRows(); ++row)
  {
    for (int column = 0; column < GridColumns(); ++column)
    {
      const int index = page_start + row * GridColumns() + column;
      if (index >= static_cast<int>(m_visible_games.size()))
        continue;
      Game& game = *VisibleGame(index);
      EnsureCover(&game);
      const int x = x0 + column * (cover_width + gap_x);
      const int y = y0 + row * (cover_height + caption + gap_y);
      const bool current = index == selection;
      FillRect(x + 4, y + 6, cover_width, cover_height, SDL_Color{0, 0, 0, 55});
      FillRect(x + 2, y + 3, cover_width, cover_height, SDL_Color{0, 0, 0, 70});
      if (game.cover)
      {
        Uint8 alpha = 255;
        if (m_animations && SDL_GetTicks() - game.cover_loaded_at < 180)
          alpha = static_cast<Uint8>((SDL_GetTicks() - game.cover_loaded_at) * 255 / 180);
        SDL_SetTextureAlphaMod(game.cover, alpha);
        SDL_SetTextureColorMod(game.cover, current ? 255 : 150, current ? 255 : 150,
                               current ? 255 : 150);
        SDL_Rect destination{x, y, cover_width, cover_height};
        SDL_RenderCopy(m_renderer, game.cover, nullptr, &destination);
      }
      else
      {
        FillRect(x, y, cover_width, cover_height, m_card);
        const std::string_view no_cover = m_localization.Translate("NO COVER");
        const int text_width = cover_width - 16;
        const int line_height = TTF_FontHeight(m_font_small) + 4;
        const int center_y = y + cover_height / 2;
        if (TextWidth(m_font_small, no_cover) <= text_width)
          DrawTextCentered(m_font_small, x + cover_width / 2,
                           center_y - TTF_FontHeight(m_font_small) / 2, no_cover, m_dim);
        else
          DrawWrappedCentered(m_font_small, x + cover_width / 2, center_y - line_height, text_width,
                              line_height, 2, no_cover, m_dim);
      }
      Border(x, y, cover_width, cover_height, 1, SDL_Color{12, 13, 18, 255});
      FillRect(x, y, cover_width, 1, SDL_Color{255, 255, 255, 26});
      if (current)
      {
        constexpr int glow = 6;
        for (int glow_index = glow; glow_index >= 1; --glow_index)
        {
          const Uint8 alpha = static_cast<Uint8>(150 * (glow - glow_index + 1) / glow);
          Border(x - 2 - glow_index, y - 2 - glow_index, cover_width + 4 + glow_index * 2,
                 cover_height + 4 + glow_index * 2, 1, SDL_Color{255, 170, 0, alpha});
        }
        Border(x - 2, y - 2, cover_width + 4, cover_height + 4, 2, m_selection);
      }
      int flag_index = 0;
      if (game.region == DiscIO::Region::NTSC_U)
        flag_index = 1;
      else if (game.region == DiscIO::Region::PAL)
        flag_index = 2;
      else if (game.region == DiscIO::Region::NTSC_J || game.region == DiscIO::Region::NTSC_K)
        flag_index = 3;
      if (m_show_region_flags && flag_index && m_flags[flag_index])
      {
        int flag_width = std::clamp(cover_width * 26 / 100, 16, 30);
        const int flag_height = flag_width * 2 / 3;
        SDL_Rect flag{x + 6, y + 6, flag_width, flag_height};
        SDL_RenderCopy(m_renderer, m_flags[flag_index], nullptr, &flag);
        Border(x + 6, y + 6, flag_width, flag_height, 1, SDL_Color{10, 12, 18, 255});
      }
      if (m_show_custom_settings_badges && game.has_game_config)
      {
        const int size = std::max(12, cover_width / 11);
        FillRect(x + cover_width - size - 8, y + 8, size, size, m_selection);
        Border(x + cover_width - size - 8, y + 8, size, size, 2, SDL_Color{10, 12, 18, 255});
      }
      if (m_favorites.contains(game.key))
        DrawText(m_font_small, x + cover_width - 27, y + 5, "★", m_value);
      if (m_show_titles)
        DrawTitleCell(x + cover_width / 2, cover_width, y + cover_height + 6, game, current,
                      current ? m_value : m_dim);
    }
  }
  // Decode the following page after all visible covers are queued. Page turns therefore avoid
  // the first-visit stall without allowing a large library to flood memory or the SDL upload path.
  const int prefetch_start = page_start + per_page;
  const int prefetch_end =
      std::min(static_cast<int>(m_visible_games.size()), prefetch_start + per_page);
  for (int index = prefetch_start; index < prefetch_end; ++index)
    EnsureCover(VisibleGame(index));
  if (m_visible_games.empty())
  {
    if (m_library_scan)
    {
      const std::size_t processed = m_library_scan->processed.load(std::memory_order_acquire);
      const std::size_t discovered = m_library_scan->discovered.load(std::memory_order_acquire);
      const std::string progress =
          discovered ? std::string(m_localization.Translate("Scanning game library...")) + "  " +
                           std::to_string(processed) + " / " + std::to_string(discovered) :
                       std::string(m_localization.Translate("Scanning game folders..."));
      DrawTextCentered(m_font, m_width / 2, m_height / 2, progress, m_dim);
    }
    else
    {
      DrawTextCentered(
          m_font, m_width / 2, m_height / 2,
          m_localization.Translate("No games match this view -- press - to change filters"), m_dim);
    }
  }
  DrawUpdateNotification();
  const std::array<std::pair<std::string_view, std::string_view>, 8> footer_hints = {
      std::pair{"A", "Launch"},    std::pair{"Y", "Sort"},   std::pair{"X", "Settings"},
      std::pair{"+", "Game Menu"}, std::pair{"-", "Filter"}, std::pair{"L", ""},
      std::pair{"R", "Page"},      std::pair{"B", "Quit"}};
  DrawFooter(footer_hints);
  SDL_RenderPresent(m_renderer);
}

std::string Launcher::SharedGameIniPath(const Game& game) const
{
  if (game.game_id.empty())
    return {};
  return File::GetUserPath(D_GAMESETTINGS_IDX) + game.game_id + ".ini";
}

std::string Launcher::EntryGameIniPath(const Game& game) const
{
  if (game.installed_nand || game.key.empty())
    return {};
  return File::GetUserPath(D_GAMESETTINGS_IDX) + "Entries/" + game.key + ".ini";
}

std::string Launcher::GameIniPath(const Game& game) const
{
  return game.config_override_path.empty() ? SharedGameIniPath(game) : game.config_override_path;
}

std::optional<std::string> Launcher::GetGameSetting(const Game& game, std::string_view section,
                                                    std::string_view key) const
{
  const auto read = [&](const std::string& path) -> std::optional<std::string> {
    if (path.empty())
      return std::nullopt;
    auto iterator = m_game_ini_cache.find(path);
    if (iterator == m_game_ini_cache.end())
    {
      auto ini = std::make_unique<Common::IniFile>();
      ini->Load(path);
      iterator = m_game_ini_cache.emplace(path, std::move(ini)).first;
    }
    const Common::IniFile::Section* ini_section = iterator->second->GetSection(section);
    if (!ini_section)
      return std::nullopt;
    std::string value;
    return ini_section->Get(key, &value) ? std::optional<std::string>{value} : std::nullopt;
  };

  if (const std::optional<std::string> value = read(GameIniPath(game)))
    return value;
  if (!game.config_override_path.empty())
    return read(SharedGameIniPath(game));
  return std::nullopt;
}

bool Launcher::SetGameSetting(const Game& game, std::string_view section, std::string_view key,
                              const std::optional<std::string>& value)
{
  return SetGameSettings(game, {{section, key, value}});
}

bool Launcher::SetGameSettings(const Game& game,
                               std::initializer_list<std::tuple<std::string_view, std::string_view,
                                                                std::optional<std::string>>>
                                   edits)
{
  const std::string path = GameIniPath(game);
  if (path.empty())
    return false;
  if (!File::CreateFullPath(path))
    return false;
  Common::IniFile ini;
  ini.Load(path);
  for (const auto& [section, key, value] : edits)
  {
    if (value)
      ini.GetOrCreateSection(section)->Set(std::string(key), *value);
    else
      ini.DeleteKey(section, key);
  }
  const bool saved = ini.Save(path);
  if (saved)
    m_game_ini_cache.erase(path);
  return saved;
}

void Launcher::InvalidateGameSettingCache(const Game& game) const
{
  const std::string path = GameIniPath(game);
  if (!path.empty())
    m_game_ini_cache.erase(path);
  const std::string shared_path = SharedGameIniPath(game);
  if (!shared_path.empty() && shared_path != path)
    m_game_ini_cache.erase(shared_path);
}

std::string Launcher::GlobalValueLabel(std::string_view value) const
{
  return std::string(m_localization.Translate("Global")) + ": " +
         std::string(m_localization.Translate(value));
}

std::string Launcher::UseGlobalValueLabel(std::string_view value) const
{
  return std::string(m_localization.Translate("Use global")) + " (" +
         std::string(m_localization.Translate(value)) + ")";
}

std::string Launcher::PerGameBoolLabel(const Game& game, std::string_view section,
                                       std::string_view key, bool global, bool inverted) const
{
  const std::optional<std::string> local = GetGameSetting(game, section, key);
  if (!local)
  {
    const bool value = inverted ? !global : global;
    return GlobalValueLabel(value ? "On" : "Off");
  }
  const std::string normalized = Lower(*local);
  bool value = normalized == "true" || normalized == "1" || normalized == "yes";
  if (inverted)
    value = !value;
  return value ? "On" : "Off";
}

void Launcher::EditPerGameBool(Game& game, std::string_view title, std::string_view section,
                               std::string_view key, bool global, int delta,
                               std::string_view on_label, std::string_view off_label, bool inverted)
{
  const std::optional<std::string> local = GetGameSetting(game, section, key);
  int selected = 0;
  if (local)
  {
    const std::string normalized = Lower(*local);
    bool value = normalized == "true" || normalized == "1" || normalized == "yes";
    if (inverted)
      value = !value;
    selected = value ? 1 : 2;
  }

  if (delta == 0)
  {
    const bool global_value = inverted ? !global : global;
    selected = Dropdown(std::string(title),
                        {UseGlobalValueLabel(global_value ? on_label : off_label),
                         std::string(on_label), std::string(off_label)},
                        selected);
  }
  else
  {
    selected = (selected + (delta < 0 ? -1 : 1) + 3) % 3;
  }

  if (selected == 0)
    SetGameSetting(game, section, key, std::nullopt);
  else
  {
    bool value = selected == 1;
    if (inverted)
      value = !value;
    SetGameSetting(game, section, key, value ? "True" : "False");
  }
}

void Launcher::EmulationSettings(bool per_game, Game* game)
{
  static constexpr std::array<std::string_view, 3> CPU_LABELS = {
      "JIT ARM64 (recommended)", "Cached interpreter", "Interpreter"};
  static constexpr std::array<int, 3> CPU_VALUES = {
      static_cast<int>(PowerPC::CPUCore::JITARM64),
      static_cast<int>(PowerPC::CPUCore::CachedInterpreter),
      static_cast<int>(PowerPC::CPUCore::Interpreter)};
  static constexpr std::array<std::string_view, 4> SPEED_LABELS = {"100%", "75%", "50%",
                                                                   "Unlimited"};
  static constexpr std::array<float, 4> SPEED_VALUES = {1.0f, 0.75f, 0.5f, 0.0f};

  const auto global_cpu_index = [&] {
    const int value = static_cast<int>(Config::Get(Config::MAIN_CPU_CORE));
    const auto iterator = std::ranges::find(CPU_VALUES, value);
    return iterator == CPU_VALUES.end() ? 0 : static_cast<int>(iterator - CPU_VALUES.begin());
  };
  const auto game_value = [&](std::string_view key) -> std::optional<std::string> {
    return per_game && game ? GetGameSetting(*game, "Core", key) : std::nullopt;
  };
  const auto bool_label = [&](std::string_view key, bool global) {
    if (!per_game)
      return std::string(global ? "On" : "Off");
    return PerGameBoolLabel(*game, "Core", key, global);
  };

  RunRows(
      per_game ? "Game emulation settings" : "Emulation", game ? game->title : std::string{},
      [&] {
        int cpu_index = global_cpu_index();
        std::string cpu_label(CPU_LABELS[cpu_index]);
        if (const auto value = game_value("CPUCore"))
        {
          const int parsed = std::atoi(value->c_str());
          const auto iterator = std::ranges::find(CPU_VALUES, parsed);
          if (iterator != CPU_VALUES.end())
            cpu_label = std::string(CPU_LABELS[iterator - CPU_VALUES.begin()]);
        }
        else if (per_game)
        {
          cpu_label = GlobalValueLabel(cpu_label);
        }
        const float global_speed = Config::Get(Config::MAIN_EMULATION_SPEED);
        int speed_index = 0;
        for (int index = 0; index < static_cast<int>(SPEED_VALUES.size()); ++index)
          if (std::abs(global_speed - SPEED_VALUES[index]) < 0.01f)
            speed_index = index;
        std::string speed_label(SPEED_LABELS[speed_index]);
        if (const auto value = game_value("EmulationSpeed"))
        {
          const float parsed = std::strtof(value->c_str(), nullptr);
          for (int index = 0; index < static_cast<int>(SPEED_VALUES.size()); ++index)
            if (std::abs(parsed - SPEED_VALUES[index]) < 0.01f)
              speed_label = SPEED_LABELS[index];
        }
        else if (per_game)
        {
          speed_label = GlobalValueLabel(speed_label);
        }
        return std::vector<Row>{
            {"CPU engine", cpu_label, !per_game || (game && !game->game_id.empty())},
            {"Dual Core", bool_label("CPUThread", Config::Get(Config::MAIN_CPU_THREAD))},
            {"Enable cheats", bool_label("EnableCheats", Config::Get(Config::MAIN_ENABLE_CHEATS))},
            {"Fast disc speed",
             bool_label("FastDiscSpeed", Config::Get(Config::MAIN_FAST_DISC_SPEED))},
            {"MMU emulation", bool_label("MMU", Config::Get(Config::MAIN_MMU))},
            {"Emulation speed", speed_label},
            {"Advanced CPU & timing", ">", true, false, false},
        };
      },
      [&](int index, int delta) {
        if (index == 0)
        {
          const int global = global_cpu_index();
          int current = global;
          const std::optional<std::string> local = game_value("CPUCore");
          if (local)
          {
            const auto iterator = std::ranges::find(CPU_VALUES, std::atoi(local->c_str()));
            if (iterator != CPU_VALUES.end())
              current = iterator - CPU_VALUES.begin();
          }
          if (per_game)
          {
            std::vector<std::string> choices{UseGlobalValueLabel(CPU_LABELS[global])};
            for (const std::string_view label : CPU_LABELS)
              choices.emplace_back(label);
            int selected = local ? current + 1 : 0;
            if (delta == 0)
              selected = Dropdown("CPU engine", choices, selected);
            else
              selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(choices.size())) %
                         static_cast<int>(choices.size());
            if (selected == 0)
              SetGameSetting(*game, "Core", "CPUCore", std::nullopt);
            else
              SetGameSetting(*game, "Core", "CPUCore", std::to_string(CPU_VALUES[selected - 1]));
          }
          else
          {
            current = SelectChoice("CPU engine", CPU_LABELS, current, delta);
            Config::SetBase(Config::MAIN_CPU_CORE,
                            static_cast<PowerPC::CPUCore>(CPU_VALUES[current]));
            MarkConfigDirty();
          }
        }
        else if (index >= 1 && index <= 4)
        {
          const std::array<std::string_view, 4> keys = {"CPUThread", "EnableCheats",
                                                        "FastDiscSpeed", "MMU"};
          const std::array<bool, 4> globals = {
              Config::Get(Config::MAIN_CPU_THREAD), Config::Get(Config::MAIN_ENABLE_CHEATS),
              Config::Get(Config::MAIN_FAST_DISC_SPEED), Config::Get(Config::MAIN_MMU)};
          if (per_game)
          {
            static constexpr std::array<std::string_view, 4> titles = {
                "Dual Core", "Enable cheats", "Fast disc speed", "MMU emulation"};
            EditPerGameBool(*game, titles[index - 1], "Core", keys[index - 1], globals[index - 1],
                            delta);
          }
          else
          {
            const bool next = !globals[index - 1];
            if (index == 1)
              Config::SetBase(Config::MAIN_CPU_THREAD, next);
            else if (index == 2)
              Config::SetBase(Config::MAIN_ENABLE_CHEATS, next);
            else if (index == 3)
              Config::SetBase(Config::MAIN_FAST_DISC_SPEED, next);
            else
              Config::SetBase(Config::MAIN_MMU, next);
            MarkConfigDirty();
          }
        }
        else if (index == 5)
        {
          const float global_value = Config::Get(Config::MAIN_EMULATION_SPEED);
          int global = 0;
          for (int item = 0; item < static_cast<int>(SPEED_VALUES.size()); ++item)
            if (std::abs(global_value - SPEED_VALUES[item]) < 0.01f)
              global = item;
          int current = global;
          const std::optional<std::string> local = game_value("EmulationSpeed");
          if (local)
          {
            const float current_value = std::strtof(local->c_str(), nullptr);
            for (int item = 0; item < static_cast<int>(SPEED_VALUES.size()); ++item)
              if (std::abs(current_value - SPEED_VALUES[item]) < 0.01f)
                current = item;
          }
          if (per_game)
          {
            std::vector<std::string> choices{UseGlobalValueLabel(SPEED_LABELS[global])};
            for (const std::string_view label : SPEED_LABELS)
              choices.emplace_back(label);
            int selected = local ? current + 1 : 0;
            if (delta == 0)
              selected = Dropdown("Emulation speed", choices, selected);
            else
              selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(choices.size())) %
                         static_cast<int>(choices.size());
            if (selected == 0)
              SetGameSetting(*game, "Core", "EmulationSpeed", std::nullopt);
            else
              SetGameSetting(*game, "Core", "EmulationSpeed",
                             std::to_string(SPEED_VALUES[selected - 1]));
          }
          else
          {
            current = SelectChoice("Emulation speed", SPEED_LABELS, current, delta);
            Config::SetBase(Config::MAIN_EMULATION_SPEED, SPEED_VALUES[current]);
            MarkConfigDirty();
          }
        }
        else if (index == 6)
        {
          AdvancedEmulationSettings(per_game, game);
        }
        return false;
      },
      false,
      [&](int index) {
        static constexpr std::array<std::string_view, 6> keys = {
            "CPUCore", "CPUThread", "EnableCheats", "FastDiscSpeed", "MMU", "EmulationSpeed"};
        if (index < 0 || index >= static_cast<int>(keys.size()))
          return false;
        if (per_game)
          SetGameSetting(*game, "Core", keys[index], std::nullopt);
        else if (index == 0)
          ResetConfigSetting(Config::MAIN_CPU_CORE);
        else if (index == 1)
          ResetConfigSetting(Config::MAIN_CPU_THREAD);
        else if (index == 2)
          ResetConfigSetting(Config::MAIN_ENABLE_CHEATS);
        else if (index == 3)
          ResetConfigSetting(Config::MAIN_FAST_DISC_SPEED);
        else if (index == 4)
          ResetConfigSetting(Config::MAIN_MMU);
        else
          ResetConfigSetting(Config::MAIN_EMULATION_SPEED);
        return true;
      });
  if (game)
    game->has_game_config = RegularFileExists(GameIniPath(*game));
}

void Launcher::AdvancedEmulationSettings(bool per_game, Game* game)
{
  static constexpr std::array<int, 11> CLOCK_PERCENT_PRESETS = {25,  50,  75,  100, 125, 150,
                                                                200, 250, 300, 400, 500};
  const auto get = [&](std::string_view key) {
    return per_game && game ? GetGameSetting(*game, "Core", key) : std::nullopt;
  };
  const auto bool_label = [&](std::string_view key, bool global) {
    return per_game ? PerGameBoolLabel(*game, "Core", key, global) :
                      std::string(global ? "On" : "Off");
  };
  const auto percent_label = [&](std::string_view key, float global) {
    const std::optional<std::string> local = get(key);
    const float effective_value = local ? std::strtof(local->c_str(), nullptr) : global;
    const int percent = std::clamp(static_cast<int>(std::lround(effective_value * 100.0f)), 1, 500);
    const std::string label = std::to_string(percent) + "%";
    return per_game && !local ? GlobalValueLabel(label) : label;
  };
  const auto edit_percent = [&](std::string_view title, std::string_view key,
                                const Config::Info<float>& info, int delta) {
    const std::optional<std::string> local = get(key);
    const float global = Config::Get(info);
    int percent = std::clamp(static_cast<int>(std::lround(
                                 (local ? std::strtof(local->c_str(), nullptr) : global) * 100.0f)),
                             1, 500);

    std::vector<int> values(CLOCK_PERCENT_PRESETS.begin(), CLOCK_PERCENT_PRESETS.end());
    if (!std::ranges::contains(values, percent))
    {
      values.push_back(percent);
      std::ranges::sort(values);
    }
    const int global_percent = std::clamp(static_cast<int>(std::lround(global * 100.0f)), 1, 500);
    if (!std::ranges::contains(values, global_percent))
    {
      values.push_back(global_percent);
      std::ranges::sort(values);
    }

    const auto format = [&](int value) {
      return value == 100 ? std::string{m_localization.Translate("Default (100%)")} :
                            std::to_string(value) + "%";
    };
    std::vector<std::string> choices;
    choices.reserve(values.size() + (per_game ? 1 : 0));
    if (per_game)
      choices.push_back(UseGlobalValueLabel(format(global_percent)));
    for (const int value : values)
      choices.push_back(format(value));

    int selected = static_cast<int>(std::ranges::find(values, percent) - values.begin());
    if (per_game)
      selected = local ? selected + 1 : 0;
    if (delta == 0)
      selected = Dropdown(title, choices, selected, true, false);
    else
      selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(choices.size())) %
                 static_cast<int>(choices.size());
    if (selected < 0)
      return;
    if (per_game && selected == 0)
    {
      SetGameSetting(*game, "Core", key, std::nullopt);
      return;
    }
    percent = values[per_game ? selected - 1 : selected];
    const float value = static_cast<float>(percent) / 100.0f;
    if (per_game)
      SetGameSetting(*game, "Core", key, std::to_string(value));
    else
    {
      Config::SetBase(info, value);
      MarkConfigDirty();
    }
  };

  RunRows(
      per_game ? "Game advanced CPU & timing" : "Advanced CPU & timing",
      game ? game->title : std::string{},
      [&] {
        return std::vector<Row>{
            {"Accurate CPU write-back cache",
             bool_label("AccurateCPUCache", Config::Get(Config::MAIN_ACCURATE_CPU_CACHE))},
            {"Correct time drift",
             bool_label("CorrectTimeDrift", Config::Get(Config::MAIN_CORRECT_TIME_DRIFT))},
            {"Precision frame timing",
             bool_label("PrecisionFrameTiming", Config::Get(Config::MAIN_PRECISION_FRAME_TIMING))},
            {"Rush frame presentation",
             bool_label("RushFramePresentation",
                        Config::Get(Config::MAIN_RUSH_FRAME_PRESENTATION))},
            {"Smooth early presentation",
             bool_label("SmoothEarlyPresentation",
                        Config::Get(Config::MAIN_SMOOTH_EARLY_PRESENTATION))},
            {"Emulated CPU clock override",
             bool_label("OverclockEnable", Config::Get(Config::MAIN_OVERCLOCK_ENABLE))},
            {"CPU clock percentage",
             percent_label("Overclock", Config::Get(Config::MAIN_OVERCLOCK))},
            {"VBI frequency override",
             bool_label("VIOverclockEnable", Config::Get(Config::MAIN_VI_OVERCLOCK_ENABLE))},
            {"VBI frequency percentage",
             percent_label("VIOverclock", Config::Get(Config::MAIN_VI_OVERCLOCK))},
        };
      },
      [&](int index, int delta) {
        static constexpr std::array<std::string_view, 7> BOOL_KEYS = {
            "AccurateCPUCache",      "CorrectTimeDrift",        "PrecisionFrameTiming",
            "RushFramePresentation", "SmoothEarlyPresentation", "OverclockEnable",
            "VIOverclockEnable"};
        const std::array<bool, 7> globals = {Config::Get(Config::MAIN_ACCURATE_CPU_CACHE),
                                             Config::Get(Config::MAIN_CORRECT_TIME_DRIFT),
                                             Config::Get(Config::MAIN_PRECISION_FRAME_TIMING),
                                             Config::Get(Config::MAIN_RUSH_FRAME_PRESENTATION),
                                             Config::Get(Config::MAIN_SMOOTH_EARLY_PRESENTATION),
                                             Config::Get(Config::MAIN_OVERCLOCK_ENABLE),
                                             Config::Get(Config::MAIN_VI_OVERCLOCK_ENABLE)};
        const int bool_index = index <= 4 ? index : index == 5 ? 5 : index == 7 ? 6 : -1;
        if (bool_index >= 0)
        {
          if ((bool_index == 0 || bool_index >= 5) && delta == 0)
          {
            const bool effective = [&] {
              const auto local = get(BOOL_KEYS[bool_index]);
              if (!local)
                return globals[bool_index];
              const std::string normalized = Lower(*local);
              return normalized == "true" || normalized == "1" || normalized == "yes";
            }();
            if (!effective &&
                !Confirm("Enable expert timing option?",
                         std::array<std::string, 2>{
                             bool_index == 0 ? "Accurate cache emulation has a large CPU cost." :
                                               "Clock overrides can change speed and break games.",
                             "Use the default unless a game specifically needs it."},
                         true))
              return false;
          }
          if (per_game)
          {
            EditPerGameBool(*game, std::string(BOOL_KEYS[bool_index]), "Core",
                            BOOL_KEYS[bool_index], globals[bool_index], delta);
          }
          else
          {
            const bool next = !globals[bool_index];
            if (bool_index == 0)
              Config::SetBase(Config::MAIN_ACCURATE_CPU_CACHE, next);
            else if (bool_index == 1)
              Config::SetBase(Config::MAIN_CORRECT_TIME_DRIFT, next);
            else if (bool_index == 2)
              Config::SetBase(Config::MAIN_PRECISION_FRAME_TIMING, next);
            else if (bool_index == 3)
              Config::SetBase(Config::MAIN_RUSH_FRAME_PRESENTATION, next);
            else if (bool_index == 4)
              Config::SetBase(Config::MAIN_SMOOTH_EARLY_PRESENTATION, next);
            else if (bool_index == 5)
              Config::SetBase(Config::MAIN_OVERCLOCK_ENABLE, next);
            else
              Config::SetBase(Config::MAIN_VI_OVERCLOCK_ENABLE, next);
            MarkConfigDirty();
          }
        }
        else if (index == 6)
          edit_percent("CPU clock percentage", "Overclock", Config::MAIN_OVERCLOCK, delta);
        else if (index == 8)
          edit_percent("VBI frequency percentage", "VIOverclock", Config::MAIN_VI_OVERCLOCK, delta);
        return false;
      },
      false,
      [&](int index) {
        static constexpr std::array<std::string_view, 9> keys = {"AccurateCPUCache",
                                                                 "CorrectTimeDrift",
                                                                 "PrecisionFrameTiming",
                                                                 "RushFramePresentation",
                                                                 "SmoothEarlyPresentation",
                                                                 "OverclockEnable",
                                                                 "Overclock",
                                                                 "VIOverclockEnable",
                                                                 "VIOverclock"};
        if (index < 0 || index >= static_cast<int>(keys.size()))
          return false;
        if (per_game)
          SetGameSetting(*game, "Core", keys[index], std::nullopt);
        else if (index == 0)
          ResetConfigSetting(Config::MAIN_ACCURATE_CPU_CACHE);
        else if (index == 1)
          ResetConfigSetting(Config::MAIN_CORRECT_TIME_DRIFT);
        else if (index == 2)
          ResetConfigSetting(Config::MAIN_PRECISION_FRAME_TIMING);
        else if (index == 3)
          ResetConfigSetting(Config::MAIN_RUSH_FRAME_PRESENTATION);
        else if (index == 4)
          ResetConfigSetting(Config::MAIN_SMOOTH_EARLY_PRESENTATION);
        else if (index == 5)
          ResetConfigSetting(Config::MAIN_OVERCLOCK_ENABLE);
        else if (index == 6)
          ResetConfigSetting(Config::MAIN_OVERCLOCK);
        else if (index == 7)
          ResetConfigSetting(Config::MAIN_VI_OVERCLOCK_ENABLE);
        else
          ResetConfigSetting(Config::MAIN_VI_OVERCLOCK);
        return true;
      });
  if (game)
    game->has_game_config = RegularFileExists(GameIniPath(*game));
}

void Launcher::GraphicsSettings(bool per_game, Game* game)
{
  static constexpr std::array<std::string_view, 3> BACKEND_LABELS = {
      "Vulkan (NVK)", "OpenGL (NVC0)", "OpenGL (Zink/NVK)"};
  static constexpr std::array<std::string_view, 5> RESOLUTION_LABELS = {
      "Auto (integral)", "1x native", "2x", "3x", "4x"};
  static constexpr std::array<int, 5> RESOLUTION_VALUES = {0, 1, 2, 3, 4};
  static constexpr std::array<std::string_view, 4> ASPECT_LABELS = {"Auto", "Force 16:9",
                                                                    "Force 4:3", "Stretch"};
  static constexpr std::array<std::string_view, 4> SHADER_LABELS = {
      "Synchronous", "Synchronous ubershaders", "Asynchronous ubershaders", "Asynchronous skip"};
  const auto get = [&](std::string_view section, std::string_view key) {
    return per_game && game ? GetGameSetting(*game, section, key) : std::nullopt;
  };
  const auto bool_value = [](const std::optional<std::string>& value, bool fallback) {
    if (!value)
      return fallback;
    const std::string normalized = Lower(*value);
    return normalized == "true" || normalized == "1" || normalized == "yes";
  };
  const auto backend_index = [](std::string_view value, bool use_zink) {
    return value == "OGL" ? (use_zink ? 2 : 1) : 0;
  };
  const auto global_backend_index = [&] {
    return backend_index(Config::Get(Config::MAIN_GFX_BACKEND),
                         Config::Get(Config::GFX_SWITCH_USE_ZINK));
  };
  const auto effective_backend_index = [&] {
    const std::optional<std::string> local_backend = get("Core", "GFXBackend");
    const std::optional<std::string> local_zink = get("Video_Settings", "SwitchUseZink");
    const std::string backend =
        local_backend.value_or(Config::Get(Config::MAIN_GFX_BACKEND));
    const bool use_zink =
        bool_value(local_zink, Config::Get(Config::GFX_SWITCH_USE_ZINK));
    return backend_index(backend, use_zink);
  };
  const auto show_glthread = [&] { return effective_backend_index() != 0; };
  const auto backend_display = [&] {
    const bool has_local = get("Core", "GFXBackend").has_value() ||
                           get("Video_Settings", "SwitchUseZink").has_value();
    const int selected = effective_backend_index();
    return per_game && !has_local ? GlobalValueLabel(BACKEND_LABELS[selected]) :
                                    std::string(BACKEND_LABELS[selected]);
  };
  const auto display_choice = [&](std::string_view section, std::string_view key, int global,
                                  std::span<const std::string_view> labels,
                                  std::span<const int> values) {
    int selected = 0;
    const auto global_iterator = std::ranges::find(values, global);
    if (global_iterator != values.end())
      selected = global_iterator - values.begin();
    if (const auto value = get(section, key))
    {
      const auto iterator = std::ranges::find(values, std::atoi(value->c_str()));
      if (iterator != values.end())
        selected = iterator - values.begin();
      return std::string(labels[selected]);
    }
    return per_game ? GlobalValueLabel(labels[selected]) : std::string(labels[selected]);
  };
  const auto bool_display = [&](std::string_view section, std::string_view key, bool global) {
    return per_game ? PerGameBoolLabel(*game, section, key, global) :
                      std::string(global ? "On" : "Off");
  };
  const auto change_choice =
      [&](std::string_view section, std::string_view key, int global, std::span<const int> values,
          std::span<const std::string_view> labels, std::string_view title, int delta) {
        int global_selected = 0;
        if (const auto iterator = std::ranges::find(values, global); iterator != values.end())
          global_selected = iterator - values.begin();
        const std::optional<std::string> local = get(section, key);
        int selected = global_selected;
        if (local)
        {
          const auto iterator = std::ranges::find(values, std::atoi(local->c_str()));
          if (iterator != values.end())
            selected = iterator - values.begin();
        }
        if (per_game)
        {
          std::vector<std::string> choices{UseGlobalValueLabel(labels[global_selected])};
          for (const std::string_view label : labels)
            choices.emplace_back(label);
          int choice = local ? selected + 1 : 0;
          if (delta == 0)
            choice = Dropdown(title, choices, choice);
          else
            choice = (choice + (delta < 0 ? -1 : 1) + static_cast<int>(choices.size())) %
                     static_cast<int>(choices.size());
          if (choice == 0)
          {
            SetGameSetting(*game, section, key, std::nullopt);
            return values[global_selected];
          }
          selected = choice - 1;
          SetGameSetting(*game, section, key, std::to_string(values[selected]));
        }
        else
        {
          selected = SelectChoice(title, labels, selected, delta);
        }
        return values[selected];
      };
  const std::array<int, 4> aspect_values = {0, 1, 2, 3};
  const std::array<int, 4> shader_values = {0, 1, 2, 3};
  RunRows(
      per_game ? "Game graphics settings" : "Graphics", game ? game->title : std::string{},
      [&] {
        std::vector<Row> rows;
        rows.emplace_back("Video backend", backend_display(), true, false, false);
        if (show_glthread())
        {
          rows.emplace_back("GLThread",
                            bool_display("Video_Settings", "SwitchGLThread",
                                         Config::Get(Config::GFX_SWITCH_GLTHREAD)));
        }
        rows.emplace_back("Internal resolution",
                          display_choice("Video_Settings", "InternalResolution",
                                         Config::Get(Config::GFX_EFB_SCALE), RESOLUTION_LABELS,
                                         RESOLUTION_VALUES));
        rows.emplace_back("Aspect ratio",
                          display_choice("Video_Settings", "AspectRatio",
                                         static_cast<int>(Config::Get(Config::GFX_ASPECT_RATIO)),
                                         ASPECT_LABELS, aspect_values));
        rows.emplace_back("VSync", bool_display("Video_Hardware", "VSync",
                                                Config::Get(Config::GFX_VSYNC)));
        rows.emplace_back(
            "Shader compilation",
            display_choice("Video_Settings", "ShaderCompilationMode",
                           static_cast<int>(Config::Get(Config::GFX_SHADER_COMPILATION_MODE)),
                           SHADER_LABELS, shader_values));
        rows.emplace_back("Wait for shaders before starting",
                          bool_display("Video_Settings", "WaitForShadersBeforeStarting",
                                       Config::Get(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING)));
        rows.emplace_back("Crop to aspect ratio",
                          bool_display("Video_Settings", "Crop",
                                       Config::Get(Config::GFX_CROP_TO_ASPECT_RATIO)));
        rows.emplace_back("Show FPS", bool_display("Video_Settings", "ShowFPS",
                                                   Config::Get(Config::GFX_SHOW_FPS)));
        rows.emplace_back("Enhancements", ">", true, false, false);
        rows.emplace_back("Hacks", ">", true, false, false);
        return rows;
      },
      [&](int index, int delta) {
        if (index == 0)
        {
          const std::optional<std::string> local_backend = get("Core", "GFXBackend");
          const std::optional<std::string> local_zink =
              get("Video_Settings", "SwitchUseZink");
          const bool has_local = local_backend.has_value() || local_zink.has_value();
          const int current_global_backend = global_backend_index();
          int selected = effective_backend_index();
          if (per_game)
          {
            std::vector<std::string> choices{
                UseGlobalValueLabel(BACKEND_LABELS[current_global_backend])};
            for (const std::string_view label : BACKEND_LABELS)
              choices.emplace_back(label);
            int choice = has_local ? selected + 1 : 0;
            if (delta == 0)
              choice = Dropdown("Video backend", choices, choice);
            else
              choice = (choice + (delta < 0 ? -1 : 1) + static_cast<int>(choices.size())) %
                       static_cast<int>(choices.size());
            if (choice == 0)
            {
              SetGameSettings(*game,
                              {{"Core", "GFXBackend", std::nullopt},
                               {"Video_Settings", "SwitchUseZink", std::nullopt}});
            }
            else
            {
              selected = choice - 1;
              SetGameSettings(
                  *game,
                  {{"Core", "GFXBackend", std::string(selected == 0 ? "Vulkan" : "OGL")},
                   {"Video_Settings", "SwitchUseZink",
                    std::string(selected == 2 ? "True" : "False")}});
            }
          }
          else
          {
            selected = SelectChoice("Video backend", BACKEND_LABELS, selected, delta);
            Config::SetBase(Config::MAIN_GFX_BACKEND,
                            std::string(selected == 0 ? "Vulkan" : "OGL"));
            Config::SetBase(Config::GFX_SWITCH_USE_ZINK, selected == 2);
            MarkConfigDirty();
          }
          return false;
        }
        const bool glthread_visible = show_glthread();
        if (glthread_visible && index == 1)
        {
          const bool global = Config::Get(Config::GFX_SWITCH_GLTHREAD);
          if (per_game)
          {
            EditPerGameBool(*game, "GLThread", "Video_Settings", "SwitchGLThread", global,
                            delta);
          }
          else
          {
            Config::SetBase(Config::GFX_SWITCH_GLTHREAD, !global);
            MarkConfigDirty();
          }
          return false;
        }
        const int graphics_index = index - (glthread_visible ? 1 : 0);
        if (graphics_index == 1)
        {
          const int next = change_choice("Video_Settings", "InternalResolution",
                                         Config::Get(Config::GFX_EFB_SCALE), RESOLUTION_VALUES,
                                         RESOLUTION_LABELS, "Internal resolution", delta);
          if (!per_game)
          {
            Config::SetBase(Config::GFX_EFB_SCALE, next);
            MarkConfigDirty();
          }
        }
        else if (graphics_index == 2)
        {
          const int next = change_choice("Video_Settings", "AspectRatio",
                                         static_cast<int>(Config::Get(Config::GFX_ASPECT_RATIO)),
                                         aspect_values, ASPECT_LABELS, "Aspect ratio", delta);
          if (!per_game)
          {
            Config::SetBase(Config::GFX_ASPECT_RATIO, static_cast<AspectMode>(next));
            MarkConfigDirty();
          }
        }
        else if (graphics_index == 3)
        {
          const bool global = Config::Get(Config::GFX_VSYNC);
          if (per_game)
          {
            EditPerGameBool(*game, "VSync", "Video_Hardware", "VSync", global, delta);
          }
          else
          {
            Config::SetBase(Config::GFX_VSYNC, !global);
            MarkConfigDirty();
          }
        }
        else if (graphics_index == 4)
        {
          const int next =
              change_choice("Video_Settings", "ShaderCompilationMode",
                            static_cast<int>(Config::Get(Config::GFX_SHADER_COMPILATION_MODE)),
                            shader_values, SHADER_LABELS, "Shader compilation", delta);
          if (!per_game)
          {
            Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                            static_cast<ShaderCompilationMode>(next));
            MarkConfigDirty();
          }
        }
        else if (graphics_index >= 5 && graphics_index <= 7)
        {
          const std::array<std::string_view, 3> keys = {"WaitForShadersBeforeStarting", "Crop",
                                                        "ShowFPS"};
          const std::array<bool, 3> values = {
              Config::Get(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING),
              Config::Get(Config::GFX_CROP_TO_ASPECT_RATIO), Config::Get(Config::GFX_SHOW_FPS)};
          if (per_game)
          {
            static constexpr std::array<std::string_view, 3> titles = {
                "Wait for shaders before starting", "Crop to aspect ratio", "Show FPS"};
            EditPerGameBool(*game, titles[graphics_index - 5], "Video_Settings",
                            keys[graphics_index - 5], values[graphics_index - 5], delta);
          }
          else
          {
            const bool next = !values[graphics_index - 5];
            if (graphics_index == 5)
              Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, next);
            else if (graphics_index == 6)
              Config::SetBase(Config::GFX_CROP_TO_ASPECT_RATIO, next);
            else
              Config::SetBase(Config::GFX_SHOW_FPS, next);
            MarkConfigDirty();
          }
        }
        else if (graphics_index == 8)
        {
          GraphicsEnhancementsSettings(per_game, game);
        }
        else if (graphics_index == 9)
        {
          GraphicsHacksSettings(per_game, game);
        }
        return false;
      },
      false,
      [&](int index) {
        if (index == 0)
        {
          if (per_game)
          {
            SetGameSettings(*game,
                            {{"Core", "GFXBackend", std::nullopt},
                             {"Video_Settings", "SwitchUseZink", std::nullopt}});
          }
          else
          {
            ResetConfigSetting(Config::MAIN_GFX_BACKEND);
            ResetConfigSetting(Config::GFX_SWITCH_USE_ZINK);
          }
          return true;
        }
        const bool glthread_visible = show_glthread();
        if (glthread_visible && index == 1)
        {
          if (per_game)
            SetGameSetting(*game, "Video_Settings", "SwitchGLThread", std::nullopt);
          else
            ResetConfigSetting(Config::GFX_SWITCH_GLTHREAD);
          return true;
        }
        const int graphics_index = index - (glthread_visible ? 1 : 0);
        static constexpr std::array<std::string_view, 7> sections = {
            "Video_Settings", "Video_Settings", "Video_Hardware", "Video_Settings",
            "Video_Settings", "Video_Settings", "Video_Settings"};
        static constexpr std::array<std::string_view, 7> keys = {
            "InternalResolution", "AspectRatio", "VSync", "ShaderCompilationMode",
            "WaitForShadersBeforeStarting", "Crop", "ShowFPS"};
        if (graphics_index < 1 || graphics_index > 7)
          return false;
        const int item = graphics_index - 1;
        if (per_game)
          SetGameSetting(*game, sections[item], keys[item], std::nullopt);
        else if (graphics_index == 1)
          ResetConfigSetting(Config::GFX_EFB_SCALE);
        else if (graphics_index == 2)
          ResetConfigSetting(Config::GFX_ASPECT_RATIO);
        else if (graphics_index == 3)
          ResetConfigSetting(Config::GFX_VSYNC);
        else if (graphics_index == 4)
          ResetConfigSetting(Config::GFX_SHADER_COMPILATION_MODE);
        else if (graphics_index == 5)
          ResetConfigSetting(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING);
        else if (graphics_index == 6)
          ResetConfigSetting(Config::GFX_CROP_TO_ASPECT_RATIO);
        else
          ResetConfigSetting(Config::GFX_SHOW_FPS);
        return true;
      });
  if (game)
    game->has_game_config = RegularFileExists(GameIniPath(*game));
}

void Launcher::FrameGenerationSettings(bool per_game, Game* game)
{
  const auto parse_bool = [](const std::optional<std::string>& value, bool fallback) {
    if (!value)
      return fallback;
    const std::string normalized = Lower(*value);
    if (normalized == "true" || normalized == "1" || normalized == "yes")
      return true;
    if (normalized == "false" || normalized == "0" || normalized == "no")
      return false;
    return fallback;
  };
  const auto normalize_flow = [](float value) { return value > 0.375f ? 0.5f : 0.25f; };
  const auto flow_label = [](float value) {
    return value > 0.375f ? std::string{"Half"} : std::string{"Quarter (recommended)"};
  };
  const auto local = [&](std::string_view key) {
    return per_game && game ? GetGameSetting(*game, "Video_Settings", key) : std::nullopt;
  };
  const auto effective_enabled = [&] {
    return parse_bool(local("LSFGEnabled"), Config::Get(Config::GFX_LSFG_ENABLED));
  };
  const auto effective_flow = [&] {
    const std::optional<std::string> value = local("LSFGFlowScale");
    return normalize_flow(value ? std::strtof(value->c_str(), nullptr) :
                                  Config::Get(Config::GFX_LSFG_FLOW_SCALE));
  };
  const auto effective_performance = [&] {
    return parse_bool(local("LSFGPerformanceMode"), Config::Get(Config::GFX_LSFG_PERFORMANCE_MODE));
  };

  RunRows(
      per_game ? "Game frame generation" : "Frame Generation", game ? game->title : std::string{},
      [&] {
        const bool installed = Vulkan::LSFG::IsDllInstalled();
        const bool enabled = effective_enabled();
        const float flow = effective_flow();
        const bool performance = effective_performance();
        return std::vector<Row>{
            {"LSFG 2x (Vulkan only)",
             per_game ? PerGameBoolLabel(*game, "Video_Settings", "LSFGEnabled",
                                         Config::Get(Config::GFX_LSFG_ENABLED)) :
                        std::string(enabled ? "On" : "Off"),
             installed || enabled},
            {"Flow resolution",
             per_game && !local("LSFGFlowScale") ? GlobalValueLabel(flow_label(flow)) :
                                                   flow_label(flow),
             installed && enabled},
            {"Performance mode",
             per_game ? PerGameBoolLabel(*game, "Video_Settings", "LSFGPerformanceMode",
                                         Config::Get(Config::GFX_LSFG_PERFORMANCE_MODE)) :
                        std::string(performance ? "On" : "Off"),
             installed && enabled},
            {"Lossless.dll", installed ? "Installed" : "Missing", false, false, false},
        };
      },
      [&](int index, int delta) {
        if (index == 0)
        {
          if (per_game)
          {
            EditPerGameBool(*game, "LSFG 2x", "Video_Settings", "LSFGEnabled",
                            Config::Get(Config::GFX_LSFG_ENABLED), delta);
            if (effective_enabled())
            {
              SetGameSetting(*game, "Video_Hacks", "SkipDuplicateXFBs", "True");
            }
          }
          else
          {
            const bool next = !Config::Get(Config::GFX_LSFG_ENABLED);
            Config::SetBase(Config::GFX_LSFG_ENABLED, next);
            if (next)
              Config::SetBase(Config::GFX_HACK_SKIP_DUPLICATE_XFBS, true);
            MarkConfigDirty();
          }
        }
        else if (index == 1)
        {
          const float global = normalize_flow(Config::Get(Config::GFX_LSFG_FLOW_SCALE));
          const float current = effective_flow();
          if (per_game)
          {
            const std::optional<std::string> current_local = local("LSFGFlowScale");
            std::vector<std::string> choices{UseGlobalValueLabel(flow_label(global)),
                                             "Quarter (recommended)", "Half"};
            int selected = current_local ? (current > 0.375f ? 2 : 1) : 0;
            selected = delta == 0 ? Dropdown("Flow resolution", choices, selected) :
                                    (selected + (delta < 0 ? -1 : 1) + 3) % 3;
            if (selected == 0)
              SetGameSetting(*game, "Video_Settings", "LSFGFlowScale", std::nullopt);
            else
              SetGameSetting(*game, "Video_Settings", "LSFGFlowScale",
                             selected == 1 ? "0.25" : "0.5");
          }
          else
          {
            int selected = current > 0.375f ? 1 : 0;
            selected = delta == 0 ? Dropdown("Flow resolution", {"Quarter (recommended)", "Half"},
                                             selected) :
                                    (selected + (delta < 0 ? -1 : 1) + 2) % 2;
            Config::SetBase(Config::GFX_LSFG_FLOW_SCALE, selected == 0 ? 0.25f : 0.5f);
            MarkConfigDirty();
          }
        }
        else if (index == 2)
        {
          if (per_game)
          {
            EditPerGameBool(*game, "LSFG performance mode", "Video_Settings", "LSFGPerformanceMode",
                            Config::Get(Config::GFX_LSFG_PERFORMANCE_MODE), delta);
          }
          else
          {
            Config::SetBase(Config::GFX_LSFG_PERFORMANCE_MODE,
                            !Config::Get(Config::GFX_LSFG_PERFORMANCE_MODE));
            MarkConfigDirty();
          }
        }
        return false;
      },
      false,
      [&](int index) {
        if (index < 0 || index > 2)
          return false;
        if (per_game)
        {
          static constexpr std::array<std::string_view, 3> keys = {"LSFGEnabled", "LSFGFlowScale",
                                                                   "LSFGPerformanceMode"};
          SetGameSetting(*game, "Video_Settings", keys[index], std::nullopt);
          if (index == 0)
            SetGameSetting(*game, "Video_Hacks", "SkipDuplicateXFBs", std::nullopt);
        }
        else if (index == 0)
          ResetConfigSetting(Config::GFX_LSFG_ENABLED);
        else if (index == 1)
          ResetConfigSetting(Config::GFX_LSFG_FLOW_SCALE);
        else
          ResetConfigSetting(Config::GFX_LSFG_PERFORMANCE_MODE);
        return true;
      });
  if (game)
    game->has_game_config = RegularFileExists(GameIniPath(*game));
}

void Launcher::GraphicsEnhancementsSettings(bool per_game, Game* game)
{
  struct AAChoice
  {
    u32 samples;
    bool ssaa;
    std::string label;
  };
  std::vector<AAChoice> aa_choices{{1, false, std::string(m_localization.Translate("None"))}};
  const std::vector<u32>& aa_modes = g_backend_info.AAModes;
  for (const u32 samples : aa_modes)
  {
    if (samples > 1)
      aa_choices.push_back({samples, false, std::to_string(samples) + "x MSAA"});
  }
  if (g_backend_info.bSupportsSSAA)
  {
    for (const u32 samples : aa_modes)
    {
      if (samples > 1)
        aa_choices.push_back({samples, true, std::to_string(samples) + "x SSAA"});
    }
  }

  struct FilteringChoice
  {
    AnisotropicFilteringMode anisotropy;
    TextureFilteringMode filtering;
    std::string_view label;
  };
  static constexpr std::array<FilteringChoice, 12> FILTERING_CHOICES{{
      {AnisotropicFilteringMode::Default, TextureFilteringMode::Default, "Default"},
      {AnisotropicFilteringMode::Force1x, TextureFilteringMode::Default, "1x Anisotropic"},
      {AnisotropicFilteringMode::Force2x, TextureFilteringMode::Default, "2x Anisotropic"},
      {AnisotropicFilteringMode::Force4x, TextureFilteringMode::Default, "4x Anisotropic"},
      {AnisotropicFilteringMode::Force8x, TextureFilteringMode::Default, "8x Anisotropic"},
      {AnisotropicFilteringMode::Force16x, TextureFilteringMode::Default, "16x Anisotropic"},
      {AnisotropicFilteringMode::Force1x, TextureFilteringMode::Nearest, "Force Nearest · 1x"},
      {AnisotropicFilteringMode::Force1x, TextureFilteringMode::Linear, "Force Linear · 1x"},
      {AnisotropicFilteringMode::Force2x, TextureFilteringMode::Linear, "Force Linear · 2x"},
      {AnisotropicFilteringMode::Force4x, TextureFilteringMode::Linear, "Force Linear · 4x"},
      {AnisotropicFilteringMode::Force8x, TextureFilteringMode::Linear, "Force Linear · 8x"},
      {AnisotropicFilteringMode::Force16x, TextureFilteringMode::Linear, "Force Linear · 16x"},
  }};
  static constexpr std::array<std::string_view, 7> RESAMPLING_LABELS = {
      "Default",          "Bilinear",       "Bicubic: B-Spline", "Bicubic: Mitchell",
      "Bicubic: Catmull", "Sharp Bilinear", "Area Sampling"};
  static constexpr std::array<std::string_view, 3> COLOR_SPACE_LABELS = {"NTSC-M", "NTSC-J", "PAL"};
  static constexpr std::array<std::string_view, 6> STEREO_MODE_LABELS = {
      "Off", "Side-by-Side", "Top-and-Bottom", "Anaglyph", "HDMI 3D", "Passive"};
  static constexpr std::array<float, 13> GAME_GAMMA_PRESETS = {
      2.20f, 2.25f, 2.30f, 2.35f, 2.40f, 2.45f, 2.50f, 2.55f, 2.60f, 2.65f, 2.70f, 2.75f, 2.80f};
  static constexpr std::array<float, 5> DISPLAY_GAMMA_PRESETS = {2.20f, 2.25f, 2.30f, 2.35f, 2.40f};
  static constexpr std::array<float, 9> HDR_PAPER_WHITE_PRESETS = {
      80.0f, 100.0f, 120.0f, 160.0f, 203.0f, 250.0f, 300.0f, 400.0f, 500.0f};
  static constexpr std::array<float, 11> STEREO_DEPTH_PRESETS = {
      0.0f, 10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f, 90.0f, 100.0f};
  static constexpr std::array<float, 10> STEREO_CONVERGENCE_PRESETS = {
      0.0f, 10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 75.0f, 100.0f, 150.0f, 200.0f};

  std::vector<std::string> post_shader_values{std::string{}};
  std::vector<std::string> post_shader_labels{std::string(m_localization.Translate("Off"))};
  for (std::string shader : VideoCommon::PostProcessing::GetShaderList())
  {
    post_shader_labels.push_back(shader);
    post_shader_values.push_back(std::move(shader));
  }

  const auto get = [&](std::string_view section, std::string_view key) {
    return per_game && game ? GetGameSetting(*game, section, key) : std::nullopt;
  };
  const auto parse_bool = [](const std::optional<std::string>& value, bool fallback) {
    if (!value)
      return fallback;
    const std::string normalized = Lower(*value);
    return normalized == "true" || normalized == "1" || normalized == "yes";
  };
  const auto bool_label = [&](std::string_view section, std::string_view key, bool global) {
    return per_game ? PerGameBoolLabel(*game, section, key, global) :
                      std::string(global ? "On" : "Off");
  };
  const auto effective_bool = [&](std::string_view section, std::string_view key, bool global) {
    return parse_bool(get(section, key), global);
  };
  const auto aa_index = [&](u32 samples, bool ssaa) {
    for (int index = 0; index < static_cast<int>(aa_choices.size()); ++index)
    {
      if (aa_choices[index].samples == samples && aa_choices[index].ssaa == ssaa)
        return index;
    }
    return 0;
  };
  const auto filtering_index = [&](AnisotropicFilteringMode anisotropy,
                                   TextureFilteringMode filtering) {
    for (int index = 0; index < static_cast<int>(FILTERING_CHOICES.size()); ++index)
    {
      if (FILTERING_CHOICES[index].anisotropy == anisotropy &&
          FILTERING_CHOICES[index].filtering == filtering)
        return index;
    }
    return 0;
  };
  const auto choice_label = [&](std::string_view section, std::string_view key, int global,
                                std::span<const std::string_view> labels) {
    const auto local = get(section, key);
    const int value = std::clamp(local ? std::atoi(local->c_str()) : global, 0,
                                 static_cast<int>(labels.size()) - 1);
    return per_game && !local ? GlobalValueLabel(labels[value]) : std::string(labels[value]);
  };
  const auto float_label = [&](std::string_view section, std::string_view key, float global,
                               int decimals) {
    const auto local = get(section, key);
    const float value = local ? std::strtof(local->c_str(), nullptr) : global;
    char formatted[32]{};
    std::snprintf(formatted, sizeof(formatted), decimals == 1 ? "%.1f" : "%.2f", value);
    return per_game && !local ? GlobalValueLabel(formatted) : std::string(formatted);
  };
  const auto edit_float = [&](std::string_view title, std::string_view section,
                              std::string_view key, const Config::Info<float>& info, float minimum,
                              float maximum, std::span<const float> presets, int decimals,
                              std::string_view suffix, int delta) {
    const auto local = get(section, key);
    float value = local ? std::strtof(local->c_str(), nullptr) : Config::Get(info);
    value = std::clamp(value, minimum, maximum);
    const float global = std::clamp(Config::Get(info), minimum, maximum);
    std::vector<float> values(presets.begin(), presets.end());
    const auto has_value = [&](float candidate) {
      return std::ranges::any_of(
          values, [&](float preset) { return std::abs(preset - candidate) < 0.0005f; });
    };
    if (!has_value(value))
    {
      values.push_back(value);
      std::ranges::sort(values);
    }
    if (!has_value(global))
    {
      values.push_back(global);
      std::ranges::sort(values);
    }
    const auto format = [&](float candidate) {
      char text[48]{};
      std::snprintf(text, sizeof(text),
                    decimals == 0 ? "%.0f" :
                    decimals == 1 ? "%.1f" :
                                    "%.2f",
                    candidate);
      return std::string(text) + std::string(suffix);
    };
    std::vector<std::string> choices;
    choices.reserve(values.size() + (per_game ? 1 : 0));
    if (per_game)
      choices.push_back(UseGlobalValueLabel(format(global)));
    for (const float preset : values)
      choices.push_back(format(preset));

    const auto current = std::ranges::find_if(
        values, [&](float preset) { return std::abs(preset - value) < 0.0005f; });
    int selected = static_cast<int>(current - values.begin());
    if (per_game)
      selected = local ? selected + 1 : 0;
    if (delta == 0)
      selected = Dropdown(title, choices, selected, true, false);
    else
      selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(choices.size())) %
                 static_cast<int>(choices.size());
    if (selected < 0)
      return;
    if (per_game && selected == 0)
    {
      SetGameSetting(*game, section, key, std::nullopt);
      return;
    }
    value = values[per_game ? selected - 1 : selected];
    if (per_game)
      SetGameSetting(*game, section, key, std::to_string(value));
    else
    {
      Config::SetBase(info, value);
      MarkConfigDirty();
    }
  };

  RunRows(
      per_game ? "Game graphics enhancements" : "Graphics enhancements",
      game ? game->title : std::string{},
      [&] {
        const u32 global_samples = Config::Get(Config::GFX_MSAA);
        const bool global_ssaa = Config::Get(Config::GFX_SSAA);
        const auto local_samples = get("Video_Settings", "MSAA");
        const auto local_ssaa = get("Video_Settings", "SSAA");
        const u32 samples = local_samples ?
                                static_cast<u32>(std::max(1, std::atoi(local_samples->c_str()))) :
                                global_samples;
        const bool ssaa = parse_bool(local_ssaa, global_ssaa);
        const bool local_aa = local_samples.has_value() || local_ssaa.has_value();
        const std::string aa_value = aa_choices[aa_index(samples, ssaa)].label;
        const std::string aa = per_game && !local_aa ? GlobalValueLabel(aa_value) : aa_value;

        const auto global_anisotropy = Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY);
        const auto global_filtering = Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING);
        const auto local_anisotropy = get("Video_Enhancements", "MaxAnisotropy");
        const auto local_filtering = get("Video_Enhancements", "ForceTextureFiltering");
        const auto anisotropy =
            local_anisotropy ?
                static_cast<AnisotropicFilteringMode>(std::atoi(local_anisotropy->c_str())) :
                global_anisotropy;
        const auto filtering =
            local_filtering ?
                static_cast<TextureFilteringMode>(std::atoi(local_filtering->c_str())) :
                global_filtering;
        const bool local_texture = local_anisotropy.has_value() || local_filtering.has_value();
        const bool fast_sampling =
            effective_bool("Video_Hacks", "FastTextureSampling",
                           Config::Get(Config::GFX_HACK_FAST_TEXTURE_SAMPLING));
        const int filter_index = filtering_index(anisotropy, filtering);

        const int global_resampling =
            std::clamp(static_cast<int>(Config::Get(Config::GFX_ENHANCE_OUTPUT_RESAMPLING)), 0, 6);
        const auto local_shader = get("Video_Enhancements", "PostProcessingShader");
        const std::string global_shader = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
        const std::string shader = local_shader ? *local_shader : global_shader;
        auto shader_iterator = std::ranges::find(post_shader_values, shader);
        const std::string shader_label =
            shader_iterator == post_shader_values.end() ?
                shader :
                post_shader_labels[shader_iterator - post_shader_values.begin()];
        const auto local_display_gamma = get("GFX.ColorCorrection", "SDRDisplayGammaSRGB");
        const bool display_srgb =
            parse_bool(local_display_gamma, Config::Get(Config::GFX_CC_SDR_DISPLAY_GAMMA_SRGB));
        const bool correct_color = effective_bool("GFX.ColorCorrection", "CorrectColorSpace",
                                                  Config::Get(Config::GFX_CC_CORRECT_COLOR_SPACE));
        const bool correct_gamma = effective_bool("GFX.ColorCorrection", "CorrectGamma",
                                                  Config::Get(Config::GFX_CC_CORRECT_GAMMA));
        const bool hdr_enabled = effective_bool("Video_Enhancements", "HDROutput",
                                                Config::Get(Config::GFX_ENHANCE_HDR_OUTPUT));
        const int global_stereo =
            std::clamp(static_cast<int>(Config::Get(Config::GFX_STEREO_MODE)), 0, 5);
        const auto local_stereo = get("Video_Stereoscopy", "StereoMode");
        const int stereo_mode =
            std::clamp(local_stereo ? std::atoi(local_stereo->c_str()) : global_stereo, 0, 5);
        const bool stereo_active = g_backend_info.bSupportsGeometryShaders && stereo_mode != 0;
        const bool full_resolution_available =
            stereo_active && (stereo_mode == static_cast<int>(StereoMode::SideBySide) ||
                              stereo_mode == static_cast<int>(StereoMode::TopAndBottom));

        std::vector<Row> rows{
            {"Anti-aliasing", aa, aa_choices.size() > 1},
            {"Texture filtering",
             fast_sampling ? (per_game && !local_texture ?
                                  GlobalValueLabel(FILTERING_CHOICES[filter_index].label) :
                                  std::string(FILTERING_CHOICES[filter_index].label)) :
                             "Disabled by Manual Texture Sampling",
             fast_sampling},
            {"Output resampling",
             choice_label("Video_Enhancements", "OutputResampling", global_resampling,
                          RESAMPLING_LABELS),
             g_backend_info.bSupportsPostProcessing},
            {"Post-processing effect",
             per_game && !local_shader ?
                 GlobalValueLabel(shader_label.empty() ? std::string_view("Off") :
                                                         std::string_view(shader_label)) :
                 (shader_label.empty() ? std::string("Off") : shader_label),
             g_backend_info.bSupportsPostProcessing},
            {"Scaled EFB copy", bool_label("Video_Hacks", "EFBScaledCopy",
                                           Config::Get(Config::GFX_HACK_COPY_EFB_SCALED))},
            {"Per-pixel lighting", bool_label("Video_Settings", "EnablePixelLighting",
                                              Config::Get(Config::GFX_ENABLE_PIXEL_LIGHTING))},
            {"Widescreen hack", bool_label("Video_Settings", "wideScreenHack",
                                           Config::Get(Config::GFX_WIDESCREEN_HACK))},
            {"Disable fog",
             bool_label("Video_Settings", "DisableFog", Config::Get(Config::GFX_DISABLE_FOG))},
            {"Force 24-bit color", bool_label("Video_Enhancements", "ForceTrueColor",
                                              Config::Get(Config::GFX_ENHANCE_FORCE_TRUE_COLOR))},
            {"Disable copy filter",
             bool_label("Video_Enhancements", "DisableCopyFilter",
                        Config::Get(Config::GFX_ENHANCE_DISABLE_COPY_FILTER))},
            {"Arbitrary mipmap detection",
             bool_label("Video_Enhancements", "ArbitraryMipmapDetection",
                        Config::Get(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION))},
            {"Correct color space",
             bool_label("GFX.ColorCorrection", "CorrectColorSpace",
                        Config::Get(Config::GFX_CC_CORRECT_COLOR_SPACE)),
             g_backend_info.bSupportsPostProcessing},
            {"Game color space",
             choice_label("GFX.ColorCorrection", "GameColorSpace",
                          static_cast<int>(Config::Get(Config::GFX_CC_GAME_COLOR_SPACE)),
                          COLOR_SPACE_LABELS),
             g_backend_info.bSupportsPostProcessing && correct_color},
            {"Correct SDR gamma",
             bool_label("GFX.ColorCorrection", "CorrectGamma",
                        Config::Get(Config::GFX_CC_CORRECT_GAMMA)),
             g_backend_info.bSupportsPostProcessing},
            {"Game gamma",
             float_label("GFX.ColorCorrection", "GameGamma", Config::Get(Config::GFX_CC_GAME_GAMMA),
                         2),
             g_backend_info.bSupportsPostProcessing},
            {"Display gamma",
             per_game && !local_display_gamma ? GlobalValueLabel(display_srgb ? "sRGB" : "Custom") :
                                                std::string(display_srgb ? "sRGB" : "Custom"),
             g_backend_info.bSupportsPostProcessing && correct_gamma},
            {"Custom display gamma",
             float_label("GFX.ColorCorrection", "SDRDisplayCustomGamma",
                         Config::Get(Config::GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA), 2),
             g_backend_info.bSupportsPostProcessing && correct_gamma && !display_srgb},
        };
        rows.push_back({"HDR post-processing",
                        g_backend_info.bSupportsHDROutput ?
                            bool_label("Video_Enhancements", "HDROutput",
                                       Config::Get(Config::GFX_ENHANCE_HDR_OUTPUT)) :
                            "Unsupported by Switch Vulkan",
                        g_backend_info.bSupportsHDROutput});
        rows.push_back({"HDR paper white",
                        float_label("GFX.ColorCorrection", "HDRPaperWhiteNits",
                                    Config::Get(Config::GFX_CC_HDR_PAPER_WHITE_NITS), 1) +
                            " nits",
                        g_backend_info.bSupportsHDROutput && hdr_enabled});
        rows.push_back(
            {"Stereoscopic 3D mode",
             choice_label("Video_Stereoscopy", "StereoMode", global_stereo, STEREO_MODE_LABELS),
             g_backend_info.bSupportsGeometryShaders});
        rows.push_back({"Stereoscopic depth",
                        float_label("Video_Stereoscopy", "StereoDepth",
                                    Config::Get(Config::GFX_STEREO_DEPTH), 1),
                        stereo_active});
        rows.push_back({"Stereoscopic convergence",
                        float_label("Video_Stereoscopy", "StereoConvergence",
                                    Config::Get(Config::GFX_STEREO_CONVERGENCE), 2),
                        stereo_active});
        rows.push_back({"Swap stereo eyes",
                        bool_label("Video_Stereoscopy", "StereoSwapEyes",
                                   Config::Get(Config::GFX_STEREO_SWAP_EYES)),
                        stereo_active});
        rows.push_back({"Full resolution per eye",
                        bool_label("Video_Stereoscopy", "StereoPerEyeResolutionFull",
                                   Config::Get(Config::GFX_STEREO_PER_EYE_RESOLUTION_FULL)),
                        full_resolution_available});
        return rows;
      },
      [&](int index, int delta) {
        if (index == 0)
        {
          const u32 global_samples = Config::Get(Config::GFX_MSAA);
          const bool global_ssaa = Config::Get(Config::GFX_SSAA);
          const auto local_samples = get("Video_Settings", "MSAA");
          const auto local_ssaa = get("Video_Settings", "SSAA");
          const u32 samples = local_samples ?
                                  static_cast<u32>(std::max(1, std::atoi(local_samples->c_str()))) :
                                  global_samples;
          const bool ssaa = parse_bool(local_ssaa, global_ssaa);
          std::vector<std::string> labels;
          int selected = aa_index(samples, ssaa);
          if (per_game)
          {
            labels.push_back(
                UseGlobalValueLabel(aa_choices[aa_index(global_samples, global_ssaa)].label));
            for (const AAChoice& choice : aa_choices)
              labels.push_back(choice.label);
            selected = local_samples || local_ssaa ? selected + 1 : 0;
          }
          else
          {
            for (const AAChoice& choice : aa_choices)
              labels.push_back(choice.label);
          }
          if (delta == 0)
            selected = Dropdown("Anti-aliasing", labels, selected, true, false);
          else
            selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(labels.size())) %
                       static_cast<int>(labels.size());
          if (per_game && selected == 0)
          {
            SetGameSettings(*game, {{"Video_Settings", "MSAA", std::nullopt},
                                    {"Video_Settings", "SSAA", std::nullopt}});
          }
          else
          {
            const AAChoice& choice = aa_choices[per_game ? selected - 1 : selected];
            if (per_game)
            {
              SetGameSettings(*game, {{"Video_Settings", "MSAA", std::to_string(choice.samples)},
                                      {"Video_Settings", "SSAA", choice.ssaa ? "True" : "False"}});
            }
            else
            {
              Config::ConfigChangeCallbackGuard guard;
              Config::SetBase(Config::GFX_MSAA, choice.samples);
              Config::SetBase(Config::GFX_SSAA, choice.ssaa);
              MarkConfigDirty();
            }
          }
        }
        else if (index == 1)
        {
          const auto global_anisotropy = Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY);
          const auto global_filtering = Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING);
          const auto local_anisotropy = get("Video_Enhancements", "MaxAnisotropy");
          const auto local_filtering = get("Video_Enhancements", "ForceTextureFiltering");
          const auto anisotropy =
              local_anisotropy ?
                  static_cast<AnisotropicFilteringMode>(std::atoi(local_anisotropy->c_str())) :
                  global_anisotropy;
          const auto filtering =
              local_filtering ?
                  static_cast<TextureFilteringMode>(std::atoi(local_filtering->c_str())) :
                  global_filtering;
          std::vector<std::string> labels;
          int selected = filtering_index(anisotropy, filtering);
          if (per_game)
          {
            labels.push_back(UseGlobalValueLabel(
                FILTERING_CHOICES[filtering_index(global_anisotropy, global_filtering)].label));
            for (const auto& choice : FILTERING_CHOICES)
              labels.emplace_back(choice.label);
            selected = local_anisotropy || local_filtering ? selected + 1 : 0;
          }
          else
          {
            for (const auto& choice : FILTERING_CHOICES)
              labels.emplace_back(choice.label);
          }
          if (delta == 0)
            selected = Dropdown("Texture filtering", labels, selected);
          else
            selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(labels.size())) %
                       static_cast<int>(labels.size());
          if (per_game && selected == 0)
          {
            SetGameSettings(*game, {{"Video_Enhancements", "MaxAnisotropy", std::nullopt},
                                    {"Video_Enhancements", "ForceTextureFiltering", std::nullopt}});
          }
          else
          {
            const FilteringChoice& choice = FILTERING_CHOICES[per_game ? selected - 1 : selected];
            if (per_game)
            {
              SetGameSettings(*game, {{"Video_Enhancements", "MaxAnisotropy",
                                       std::to_string(static_cast<int>(choice.anisotropy))},
                                      {"Video_Enhancements", "ForceTextureFiltering",
                                       std::to_string(static_cast<int>(choice.filtering))}});
            }
            else
            {
              Config::ConfigChangeCallbackGuard guard;
              Config::SetBase(Config::GFX_ENHANCE_MAX_ANISOTROPY, choice.anisotropy);
              Config::SetBase(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING, choice.filtering);
              MarkConfigDirty();
            }
          }
        }
        else if (index == 2)
        {
          const int global = std::clamp(
              static_cast<int>(Config::Get(Config::GFX_ENHANCE_OUTPUT_RESAMPLING)), 0, 6);
          const auto local = get("Video_Enhancements", "OutputResampling");
          int selected = std::clamp(local ? std::atoi(local->c_str()) : global, 0, 6);
          std::vector<std::string> labels;
          if (per_game)
          {
            labels.push_back(UseGlobalValueLabel(RESAMPLING_LABELS[global]));
            for (const auto label : RESAMPLING_LABELS)
              labels.emplace_back(label);
            selected = local ? selected + 1 : 0;
          }
          else
          {
            for (const auto label : RESAMPLING_LABELS)
              labels.emplace_back(label);
          }
          if (delta == 0)
            selected = Dropdown("Output resampling", labels, selected);
          else
            selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(labels.size())) %
                       static_cast<int>(labels.size());
          if (per_game)
            SetGameSetting(*game, "Video_Enhancements", "OutputResampling",
                           selected == 0 ?
                               std::nullopt :
                               std::optional<std::string>{std::to_string(selected - 1)});
          else
          {
            Config::SetBase(Config::GFX_ENHANCE_OUTPUT_RESAMPLING,
                            static_cast<OutputResamplingMode>(selected));
            MarkConfigDirty();
          }
        }
        else if (index == 3)
        {
          const std::string global = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
          const auto local = get("Video_Enhancements", "PostProcessingShader");
          std::string current = local ? *local : global;
          if (std::ranges::find(post_shader_values, current) == post_shader_values.end())
          {
            post_shader_values.push_back(current);
            post_shader_labels.push_back(current);
          }
          int selected = static_cast<int>(std::ranges::find(post_shader_values, current) -
                                          post_shader_values.begin());
          std::vector<std::string> labels;
          if (per_game)
          {
            const auto global_iterator = std::ranges::find(post_shader_values, global);
            const int global_index =
                global_iterator == post_shader_values.end() ?
                    0 :
                    static_cast<int>(global_iterator - post_shader_values.begin());
            labels.push_back(UseGlobalValueLabel(post_shader_labels[global_index]));
            labels.insert(labels.end(), post_shader_labels.begin(), post_shader_labels.end());
            selected = local ? selected + 1 : 0;
          }
          else
          {
            labels = post_shader_labels;
          }
          if (delta == 0)
            selected = Dropdown("Post-processing effect", labels, selected, true, false);
          else
            selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(labels.size())) %
                       static_cast<int>(labels.size());
          if (per_game)
            SetGameSetting(*game, "Video_Enhancements", "PostProcessingShader",
                           selected == 0 ?
                               std::nullopt :
                               std::optional<std::string>{post_shader_values[selected - 1]});
          else
          {
            Config::SetBase(Config::GFX_ENHANCE_POST_SHADER, post_shader_values[selected]);
            MarkConfigDirty();
          }
        }
        else if (index >= 4 && index <= 10)
        {
          static constexpr std::array<std::string_view, 7> SECTIONS = {
              "Video_Hacks",        "Video_Settings",     "Video_Settings",    "Video_Settings",
              "Video_Enhancements", "Video_Enhancements", "Video_Enhancements"};
          static constexpr std::array<std::string_view, 7> KEYS = {
              "EFBScaledCopy",  "EnablePixelLighting", "wideScreenHack",          "DisableFog",
              "ForceTrueColor", "DisableCopyFilter",   "ArbitraryMipmapDetection"};
          static constexpr std::array<std::string_view, 7> TITLES = {
              "Scaled EFB copy",           "Per-pixel lighting",
              "Widescreen hack",           "Disable fog",
              "Force 24-bit color",        "Disable copy filter",
              "Arbitrary mipmap detection"};
          const std::array<const Config::Info<bool>*, 7> infos = {
              &Config::GFX_HACK_COPY_EFB_SCALED,
              &Config::GFX_ENABLE_PIXEL_LIGHTING,
              &Config::GFX_WIDESCREEN_HACK,
              &Config::GFX_DISABLE_FOG,
              &Config::GFX_ENHANCE_FORCE_TRUE_COLOR,
              &Config::GFX_ENHANCE_DISABLE_COPY_FILTER,
              &Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION};
          const int item = index - 4;
          const bool global = Config::Get(*infos[item]);
          if (per_game)
            EditPerGameBool(*game, TITLES[item], SECTIONS[item], KEYS[item], global, delta);
          else
          {
            Config::SetBase(*infos[item], !global);
            MarkConfigDirty();
          }
        }
        else if (index == 11 || index == 13)
        {
          const bool color_space = index == 11;
          const auto& info =
              color_space ? Config::GFX_CC_CORRECT_COLOR_SPACE : Config::GFX_CC_CORRECT_GAMMA;
          const std::string_view key = color_space ? "CorrectColorSpace" : "CorrectGamma";
          const bool global = Config::Get(info);
          if (per_game)
            EditPerGameBool(*game, key, "GFX.ColorCorrection", key, global, delta);
          else
          {
            Config::SetBase(info, !global);
            MarkConfigDirty();
          }
        }
        else if (index == 12)
        {
          const int global =
              std::clamp(static_cast<int>(Config::Get(Config::GFX_CC_GAME_COLOR_SPACE)), 0, 2);
          const auto local = get("GFX.ColorCorrection", "GameColorSpace");
          int selected = std::clamp(local ? std::atoi(local->c_str()) : global, 0, 2);
          std::vector<std::string> choices;
          if (per_game)
          {
            choices.push_back(UseGlobalValueLabel(COLOR_SPACE_LABELS[global]));
            for (const auto label : COLOR_SPACE_LABELS)
              choices.emplace_back(label);
            selected = local ? selected + 1 : 0;
          }
          else
          {
            for (const auto label : COLOR_SPACE_LABELS)
              choices.emplace_back(label);
          }
          if (delta == 0)
            selected = Dropdown("Game color space", choices, selected);
          else
            selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(choices.size())) %
                       static_cast<int>(choices.size());
          if (per_game)
            SetGameSetting(*game, "GFX.ColorCorrection", "GameColorSpace",
                           selected == 0 ?
                               std::nullopt :
                               std::optional<std::string>{std::to_string(selected - 1)});
          else
          {
            Config::SetBase(Config::GFX_CC_GAME_COLOR_SPACE,
                            static_cast<ColorCorrectionRegion>(selected));
            MarkConfigDirty();
          }
        }
        else if (index == 14)
          edit_float("Game gamma", "GFX.ColorCorrection", "GameGamma", Config::GFX_CC_GAME_GAMMA,
                     Config::GFX_CC_GAME_GAMMA_MIN, Config::GFX_CC_GAME_GAMMA_MAX,
                     GAME_GAMMA_PRESETS, 2, {}, delta);
        else if (index == 15)
        {
          const bool global = Config::Get(Config::GFX_CC_SDR_DISPLAY_GAMMA_SRGB);
          if (per_game)
            EditPerGameBool(*game, "Display gamma", "GFX.ColorCorrection", "SDRDisplayGammaSRGB",
                            global, delta, "sRGB", "Custom");
          else
          {
            Config::SetBase(Config::GFX_CC_SDR_DISPLAY_GAMMA_SRGB, !global);
            MarkConfigDirty();
          }
        }
        else if (index == 16)
          edit_float("Custom display gamma", "GFX.ColorCorrection", "SDRDisplayCustomGamma",
                     Config::GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA, Config::GFX_CC_DISPLAY_GAMMA_MIN,
                     Config::GFX_CC_DISPLAY_GAMMA_MAX, DISPLAY_GAMMA_PRESETS, 2, {}, delta);
        else if (index == 17)
        {
          const bool global = Config::Get(Config::GFX_ENHANCE_HDR_OUTPUT);
          if (per_game)
            EditPerGameBool(*game, "HDR post-processing", "Video_Enhancements", "HDROutput", global,
                            delta);
          else
          {
            Config::SetBase(Config::GFX_ENHANCE_HDR_OUTPUT, !global);
            MarkConfigDirty();
          }
        }
        else if (index == 18)
          edit_float("HDR paper white", "GFX.ColorCorrection", "HDRPaperWhiteNits",
                     Config::GFX_CC_HDR_PAPER_WHITE_NITS, Config::GFX_CC_HDR_PAPER_WHITE_NITS_MIN,
                     Config::GFX_CC_HDR_PAPER_WHITE_NITS_MAX, HDR_PAPER_WHITE_PRESETS, 0, " nits",
                     delta);
        else if (index == 19)
        {
          const int global =
              std::clamp(static_cast<int>(Config::Get(Config::GFX_STEREO_MODE)), 0, 5);
          const auto local = get("Video_Stereoscopy", "StereoMode");
          int selected = std::clamp(local ? std::atoi(local->c_str()) : global, 0, 5);
          std::vector<std::string> choices;
          if (per_game)
          {
            choices.push_back(UseGlobalValueLabel(STEREO_MODE_LABELS[global]));
            for (const std::string_view label : STEREO_MODE_LABELS)
              choices.emplace_back(label);
            selected = local ? selected + 1 : 0;
          }
          else
          {
            for (const std::string_view label : STEREO_MODE_LABELS)
              choices.emplace_back(label);
          }
          if (delta == 0)
            selected = Dropdown("Stereoscopic 3D mode", choices, selected);
          else
            selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(choices.size())) %
                       static_cast<int>(choices.size());
          if (per_game)
          {
            SetGameSetting(*game, "Video_Stereoscopy", "StereoMode",
                           selected == 0 ?
                               std::nullopt :
                               std::optional<std::string>{std::to_string(selected - 1)});
          }
          else
          {
            Config::SetBase(Config::GFX_STEREO_MODE, static_cast<StereoMode>(selected));
            MarkConfigDirty();
          }
        }
        else if (index == 20)
          edit_float("Stereoscopic depth", "Video_Stereoscopy", "StereoDepth",
                     Config::GFX_STEREO_DEPTH, 0.0f, Config::GFX_STEREO_DEPTH_MAXIMUM,
                     STEREO_DEPTH_PRESETS, 0, {}, delta);
        else if (index == 21)
          edit_float("Stereoscopic convergence", "Video_Stereoscopy", "StereoConvergence",
                     Config::GFX_STEREO_CONVERGENCE, 0.0f, Config::GFX_STEREO_CONVERGENCE_MAXIMUM,
                     STEREO_CONVERGENCE_PRESETS, 0, {}, delta);
        else if (index == 22 || index == 23)
        {
          const bool swap = index == 22;
          const auto& info =
              swap ? Config::GFX_STEREO_SWAP_EYES : Config::GFX_STEREO_PER_EYE_RESOLUTION_FULL;
          const std::string_view key = swap ? "StereoSwapEyes" : "StereoPerEyeResolutionFull";
          const bool global = Config::Get(info);
          if (per_game)
            EditPerGameBool(*game, swap ? "Swap stereo eyes" : "Full resolution per eye",
                            "Video_Stereoscopy", key, global, delta);
          else
          {
            Config::SetBase(info, !global);
            MarkConfigDirty();
          }
        }
        return false;
      },
      false,
      [&](int index) {
        if (index < 0 || index > 23)
          return false;
        if (per_game)
        {
          if (index == 0)
          {
            SetGameSettings(*game, {{"Video_Settings", "MSAA", std::nullopt},
                                    {"Video_Settings", "SSAA", std::nullopt}});
          }
          else if (index == 1)
          {
            SetGameSettings(*game, {{"Video_Enhancements", "MaxAnisotropy", std::nullopt},
                                    {"Video_Enhancements", "ForceTextureFiltering", std::nullopt}});
          }
          else
          {
            static constexpr std::array<std::string_view, 22> sections = {
                "Video_Enhancements",  "Video_Enhancements",  "Video_Hacks",
                "Video_Settings",      "Video_Settings",      "Video_Settings",
                "Video_Enhancements",  "Video_Enhancements",  "Video_Enhancements",
                "GFX.ColorCorrection", "GFX.ColorCorrection", "GFX.ColorCorrection",
                "GFX.ColorCorrection", "GFX.ColorCorrection", "GFX.ColorCorrection",
                "Video_Enhancements",  "GFX.ColorCorrection", "Video_Stereoscopy",
                "Video_Stereoscopy",   "Video_Stereoscopy",   "Video_Stereoscopy",
                "Video_Stereoscopy"};
            static constexpr std::array<std::string_view, 22> keys = {"OutputResampling",
                                                                      "PostProcessingShader",
                                                                      "EFBScaledCopy",
                                                                      "EnablePixelLighting",
                                                                      "wideScreenHack",
                                                                      "DisableFog",
                                                                      "ForceTrueColor",
                                                                      "DisableCopyFilter",
                                                                      "ArbitraryMipmapDetection",
                                                                      "CorrectColorSpace",
                                                                      "GameColorSpace",
                                                                      "CorrectGamma",
                                                                      "GameGamma",
                                                                      "SDRDisplayGammaSRGB",
                                                                      "SDRDisplayCustomGamma",
                                                                      "HDROutput",
                                                                      "HDRPaperWhiteNits",
                                                                      "StereoMode",
                                                                      "StereoDepth",
                                                                      "StereoConvergence",
                                                                      "StereoSwapEyes",
                                                                      "StereoPerEyeResolutionFull"};
            SetGameSetting(*game, sections[index - 2], keys[index - 2], std::nullopt);
          }
        }
        else if (index == 0)
        {
          ResetConfigSetting(Config::GFX_MSAA);
          ResetConfigSetting(Config::GFX_SSAA);
        }
        else if (index == 1)
        {
          ResetConfigSetting(Config::GFX_ENHANCE_MAX_ANISOTROPY);
          ResetConfigSetting(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING);
        }
        else if (index == 2)
          ResetConfigSetting(Config::GFX_ENHANCE_OUTPUT_RESAMPLING);
        else if (index == 3)
          ResetConfigSetting(Config::GFX_ENHANCE_POST_SHADER);
        else if (index == 4)
          ResetConfigSetting(Config::GFX_HACK_COPY_EFB_SCALED);
        else if (index == 5)
          ResetConfigSetting(Config::GFX_ENABLE_PIXEL_LIGHTING);
        else if (index == 6)
          ResetConfigSetting(Config::GFX_WIDESCREEN_HACK);
        else if (index == 7)
          ResetConfigSetting(Config::GFX_DISABLE_FOG);
        else if (index == 8)
          ResetConfigSetting(Config::GFX_ENHANCE_FORCE_TRUE_COLOR);
        else if (index == 9)
          ResetConfigSetting(Config::GFX_ENHANCE_DISABLE_COPY_FILTER);
        else if (index == 10)
          ResetConfigSetting(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION);
        else if (index == 11)
          ResetConfigSetting(Config::GFX_CC_CORRECT_COLOR_SPACE);
        else if (index == 12)
          ResetConfigSetting(Config::GFX_CC_GAME_COLOR_SPACE);
        else if (index == 13)
          ResetConfigSetting(Config::GFX_CC_CORRECT_GAMMA);
        else if (index == 14)
          ResetConfigSetting(Config::GFX_CC_GAME_GAMMA);
        else if (index == 15)
          ResetConfigSetting(Config::GFX_CC_SDR_DISPLAY_GAMMA_SRGB);
        else if (index == 16)
          ResetConfigSetting(Config::GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA);
        else if (index == 17)
          ResetConfigSetting(Config::GFX_ENHANCE_HDR_OUTPUT);
        else if (index == 18)
          ResetConfigSetting(Config::GFX_CC_HDR_PAPER_WHITE_NITS);
        else if (index == 19)
          ResetConfigSetting(Config::GFX_STEREO_MODE);
        else if (index == 20)
          ResetConfigSetting(Config::GFX_STEREO_DEPTH);
        else if (index == 21)
          ResetConfigSetting(Config::GFX_STEREO_CONVERGENCE);
        else if (index == 22)
          ResetConfigSetting(Config::GFX_STEREO_SWAP_EYES);
        else
          ResetConfigSetting(Config::GFX_STEREO_PER_EYE_RESOLUTION_FULL);
        return true;
      });
  if (game)
    game->has_game_config = RegularFileExists(GameIniPath(*game));
}

void Launcher::GraphicsHacksSettings(bool per_game, Game* game)
{
  struct HackSetting
  {
    std::string_view label;
    std::string_view section;
    std::string_view key;
    const Config::Info<bool>* info;
    bool inverted;
  };
  static const std::array<HackSetting, 15> SETTINGS{{
      {"Skip EFB access from CPU", "Video_Hacks", "EFBAccessEnable",
       &Config::GFX_HACK_EFB_ACCESS_ENABLE, true},
      {"Ignore EFB format changes", "Video_Hacks", "EFBEmulateFormatChanges",
       &Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, true},
      {"Store EFB copies to texture only", "Video_Hacks", "EFBToTextureEnable",
       &Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, false},
      {"Defer EFB copies to RAM", "Video_Hacks", "DeferEFBCopies",
       &Config::GFX_HACK_DEFER_EFB_COPIES, false},
      {"Deferred EFB-access invalidation", "Video_Hacks", "EFBAccessDeferInvalidation",
       &Config::GFX_HACK_EFB_DEFER_INVALIDATION, false},
      {"GPU texture decoding", "Video_Settings", "EnableGPUTextureDecoding",
       &Config::GFX_ENABLE_GPU_TEXTURE_DECODING, false},
      {"Store XFB copies to texture only", "Video_Hacks", "XFBToTextureEnable",
       &Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, false},
      {"Immediately present XFB", "Video_Hacks", "ImmediateXFBEnable",
       &Config::GFX_HACK_IMMEDIATE_XFB, false},
      {"Skip presenting duplicate frames", "Video_Hacks", "SkipDuplicateXFBs",
       &Config::GFX_HACK_SKIP_DUPLICATE_XFBS, false},
      {"Fast depth calculation", "Video_Settings", "FastDepthCalc", &Config::GFX_FAST_DEPTH_CALC,
       false},
      {"Disable bounding box", "Video_Hacks", "BBoxEnable", &Config::GFX_HACK_BBOX_ENABLE, true},
      {"Vertex rounding", "Video_Hacks", "VertexRounding", &Config::GFX_HACK_VERTEX_ROUNDING,
       false},
      {"Save texture cache to state", "Video_Settings", "SaveTextureCacheToState",
       &Config::GFX_SAVE_TEXTURE_CACHE_TO_STATE, false},
      {"VBI skip", "Video_Hacks", "VISkip", &Config::GFX_HACK_VI_SKIP, false},
      {"Manual texture sampling", "Video_Hacks", "FastTextureSampling",
       &Config::GFX_HACK_FAST_TEXTURE_SAMPLING, true},
  }};

  const auto get = [&](std::string_view section, std::string_view key) {
    return per_game && game ? GetGameSetting(*game, section, key) : std::nullopt;
  };
  const auto parse_bool = [](const std::optional<std::string>& value, bool fallback) {
    if (!value)
      return fallback;
    const std::string normalized = Lower(*value);
    return normalized == "true" || normalized == "1" || normalized == "yes";
  };
  const auto config_value = [&](const HackSetting& setting) {
    return parse_bool(get(setting.section, setting.key), Config::Get(*setting.info));
  };
  const auto shown_value = [&](const HackSetting& setting) {
    const bool value = config_value(setting);
    return setting.inverted ? !value : value;
  };
  const auto accuracy_label = [&] {
    const auto local = get("Video_Settings", "SafeTextureCacheColorSamples");
    const int value = std::clamp(local ? std::atoi(local->c_str()) :
                                         Config::Get(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES),
                                 0, 512);
    std::string label = value == 0   ? "Safe (0)" :
                        value == 128 ? "Default (128)" :
                        value == 512 ? "Fast (512)" :
                                       "Custom (" + std::to_string(value) + ")";
    if (per_game && !local)
      label = GlobalValueLabel(label);
    return label;
  };

  RunRows(
      per_game ? "Game graphics hacks" : "Graphics hacks", game ? game->title : std::string{},
      [&] {
        const bool efb_to_texture = config_value(SETTINGS[2]);
        const bool xfb_to_texture = config_value(SETTINGS[6]);
        const bool immediate_xfb = config_value(SETTINGS[7]);
        const bool vi_skip = config_value(SETTINGS[13]);
        const bool arbitrary_mipmap =
            parse_bool(get("Video_Enhancements", "ArbitraryMipmapDetection"),
                       Config::Get(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION));
        std::vector<Row> rows;
        rows.reserve(16);
        for (int index = 0; index < 5; ++index)
        {
          const HackSetting& setting = SETTINGS[index];
          const bool enabled = index != 3 || !(efb_to_texture && xfb_to_texture);
          rows.push_back({std::string(setting.label),
                          per_game ?
                              PerGameBoolLabel(*game, setting.section, setting.key,
                                               Config::Get(*setting.info), setting.inverted) :
                              std::string(shown_value(setting) ? "On" : "Off"),
                          enabled});
        }
        rows.push_back({"Texture cache accuracy", accuracy_label()});
        for (int index = 5; index < static_cast<int>(SETTINGS.size()); ++index)
        {
          const HackSetting& setting = SETTINGS[index];
          bool enabled = true;
          if (index == 5)
            enabled = g_backend_info.bSupportsGPUTextureDecoding && !arbitrary_mipmap;
          else if (index == 8)
            enabled = !immediate_xfb && !vi_skip;
          else if (index == 10)
            enabled = g_backend_info.bSupportsBBox;
          rows.push_back({std::string(setting.label),
                          per_game ?
                              PerGameBoolLabel(*game, setting.section, setting.key,
                                               Config::Get(*setting.info), setting.inverted) :
                              std::string(shown_value(setting) ? "On" : "Off"),
                          enabled});
        }
        return rows;
      },
      [&](int row, int delta) {
        if (row == 5)
        {
          const auto local = get("Video_Settings", "SafeTextureCacheColorSamples");
          int value = std::clamp(local ? std::atoi(local->c_str()) :
                                         Config::Get(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES),
                                 0, 512);
          const int global =
              std::clamp(Config::Get(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES), 0, 512);
          std::vector<int> values{0, 128, 512};
          if (!std::ranges::contains(values, value))
          {
            values.push_back(value);
            std::ranges::sort(values);
          }
          if (!std::ranges::contains(values, global))
          {
            values.push_back(global);
            std::ranges::sort(values);
          }
          const auto format = [&](int samples) {
            return samples == 0   ? std::string{m_localization.Translate("Safe (0)")} :
                   samples == 128 ? std::string{m_localization.Translate("Default (128)")} :
                   samples == 512 ? std::string{m_localization.Translate("Fast (512)")} :
                                    std::string(m_localization.Translate("Custom")) + " (" +
                                        std::to_string(samples) + ")";
          };
          std::vector<std::string> choices;
          choices.reserve(values.size() + (per_game ? 1 : 0));
          if (per_game)
            choices.push_back(UseGlobalValueLabel(format(global)));
          for (const int samples : values)
            choices.push_back(format(samples));
          int selected = static_cast<int>(std::ranges::find(values, value) - values.begin());
          if (per_game)
            selected = local ? selected + 1 : 0;
          if (delta == 0)
            selected = Dropdown("Texture cache accuracy", choices, selected, true, false);
          else
            selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(choices.size())) %
                       static_cast<int>(choices.size());
          if (selected < 0)
            return false;
          if (per_game && selected == 0)
          {
            SetGameSetting(*game, "Video_Settings", "SafeTextureCacheColorSamples", std::nullopt);
            return false;
          }
          value = values[per_game ? selected - 1 : selected];
          if (per_game)
            SetGameSetting(*game, "Video_Settings", "SafeTextureCacheColorSamples",
                           std::to_string(value));
          else
          {
            Config::SetBase(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES, value);
            MarkConfigDirty();
          }
          return false;
        }

        const int setting_index = row < 5 ? row : row - 1;
        const HackSetting& setting = SETTINGS[setting_index];
        const bool global = Config::Get(*setting.info);
        if (per_game)
        {
          EditPerGameBool(*game, setting.label, setting.section, setting.key, global, delta, "On",
                          "Off", setting.inverted);
        }
        else
        {
          Config::SetBase(*setting.info, !global);
          MarkConfigDirty();
        }
        return false;
      },
      false,
      [&](int row) {
        if (row < 0 || row > static_cast<int>(SETTINGS.size()))
          return false;
        if (row == 5)
        {
          if (per_game)
            SetGameSetting(*game, "Video_Settings", "SafeTextureCacheColorSamples", std::nullopt);
          else
            ResetConfigSetting(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES);
          return true;
        }
        const int setting_index = row < 5 ? row : row - 1;
        if (setting_index < 0 || setting_index >= static_cast<int>(SETTINGS.size()))
          return false;
        const HackSetting& setting = SETTINGS[setting_index];
        if (per_game)
          SetGameSetting(*game, setting.section, setting.key, std::nullopt);
        else
          ResetConfigSetting(*setting.info);
        return true;
      });
  if (game)
    game->has_game_config = RegularFileExists(GameIniPath(*game));
}

void Launcher::AudioSettings(bool per_game, Game* game)
{
  static constexpr std::array<int, 11> VOLUME_PRESETS = {0,  10, 20, 30, 40, 50,
                                                         60, 70, 80, 90, 100};
  static constexpr std::array<int, 10> LATENCY_PRESETS = {0, 10, 20, 30, 40, 60, 80, 100, 150, 200};
  static constexpr std::array<int, 13> BUFFER_PRESETS = {16,  24,  32,  48,  64,  80, 96,
                                                         128, 160, 192, 256, 384, 512};
  const auto get = [&](std::string_view section, std::string_view key) {
    return per_game && game ? GetGameSetting(*game, section, key) : std::nullopt;
  };
  const auto int_label = [&](std::string_view section, std::string_view key, int global,
                             std::string_view suffix) {
    const auto local = get(section, key);
    const int value = local ? std::atoi(local->c_str()) : global;
    const std::string label = std::to_string(value) + std::string(suffix);
    return per_game && !local ? GlobalValueLabel(label) : label;
  };
  const auto bool_label = [&](std::string_view section, std::string_view key, bool global,
                              std::string_view on, std::string_view off) {
    const auto local = get(section, key);
    const bool value = local ? (*local == "True" || *local == "true" || *local == "1") : global;
    return per_game && !local ? GlobalValueLabel(value ? on : off) : std::string(value ? on : off);
  };
  const auto edit_integer = [&](std::string_view title, std::string_view section,
                                std::string_view key, const Config::Info<int>& info,
                                std::span<const int> presets, std::string_view suffix, int delta) {
    const auto local = get(section, key);
    const int minimum = presets.front();
    const int maximum = presets.back();
    int value = std::clamp(local ? std::atoi(local->c_str()) : Config::Get(info), minimum, maximum);
    const int global = std::clamp(Config::Get(info), minimum, maximum);
    std::vector<int> values(presets.begin(), presets.end());
    if (!std::ranges::contains(values, value))
    {
      values.push_back(value);
      std::ranges::sort(values);
    }
    if (!std::ranges::contains(values, global))
    {
      values.push_back(global);
      std::ranges::sort(values);
    }
    const auto format = [&](int candidate) {
      return std::to_string(candidate) + std::string(suffix);
    };
    std::vector<std::string> choices;
    choices.reserve(values.size() + (per_game ? 1 : 0));
    if (per_game)
      choices.push_back(UseGlobalValueLabel(format(global)));
    for (const int preset : values)
      choices.push_back(format(preset));
    int selected = static_cast<int>(std::ranges::find(values, value) - values.begin());
    if (per_game)
      selected = local ? selected + 1 : 0;
    if (delta == 0)
      selected = Dropdown(title, choices, selected, true, false);
    else
      selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(choices.size())) %
                 static_cast<int>(choices.size());
    if (selected < 0)
      return;
    if (per_game && selected == 0)
    {
      SetGameSetting(*game, section, key, std::nullopt);
      return;
    }
    value = values[per_game ? selected - 1 : selected];
    if (per_game)
      SetGameSetting(*game, section, key, std::to_string(value));
    else
    {
      Config::SetBase(info, value);
      MarkConfigDirty();
    }
  };
  RunRows(
      per_game ? "Game audio" : "Audio", game ? game->title : std::string{},
      [&] {
        std::vector<Row> rows;
        rows.push_back(
            {"Volume", int_label("DSP", "Volume", Config::Get(Config::MAIN_AUDIO_VOLUME), "%")});
        rows.push_back(
            {"DSP emulation", bool_label("Core", "DSPHLE", Config::Get(Config::MAIN_DSP_HLE),
                                         "HLE (recommended)", "LLE")});
        rows.push_back(
            {"Audio latency",
             int_label("Core", "AudioLatency", Config::Get(Config::MAIN_AUDIO_LATENCY), " ms")});
        rows.push_back(
            {"Audio buffer size", int_label("Core", "AudioBufferSize",
                                            Config::Get(Config::MAIN_AUDIO_BUFFER_SIZE), " ms")});
        rows.push_back({"Fill audio gaps",
                        bool_label("Core", "AudioFillGaps",
                                   Config::Get(Config::MAIN_AUDIO_FILL_GAPS), "On", "Off")});
        rows.push_back({"Preserve pitch",
                        bool_label("Core", "AudioPreservePitch",
                                   Config::Get(Config::MAIN_AUDIO_PRESERVE_PITCH), "On", "Off")});
        rows.push_back({"Mute when disabling speed limit",
                        bool_label("DSP", "MuteOnDisabledSpeedLimit",
                                   Config::Get(Config::MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT),
                                   "On", "Off")});
        return rows;
      },
      [&](int index, int delta) {
        if (index == 0)
          edit_integer("Volume", "DSP", "Volume", Config::MAIN_AUDIO_VOLUME, VOLUME_PRESETS, "%",
                       delta);
        else if (index == 1)
        {
          const bool global = Config::Get(Config::MAIN_DSP_HLE);
          if (per_game)
            EditPerGameBool(*game, "DSP emulation", "Core", "DSPHLE", global, delta,
                            "HLE (recommended)", "LLE");
          else
          {
            Config::SetBase(Config::MAIN_DSP_HLE, !global);
            MarkConfigDirty();
          }
        }
        else if (index == 2)
          edit_integer("Audio latency", "Core", "AudioLatency", Config::MAIN_AUDIO_LATENCY,
                       LATENCY_PRESETS, " ms", delta);
        else if (index == 3)
          edit_integer("Audio buffer size", "Core", "AudioBufferSize",
                       Config::MAIN_AUDIO_BUFFER_SIZE, BUFFER_PRESETS, " ms", delta);
        else
        {
          static constexpr std::array<std::string_view, 3> sections = {"Core", "Core", "DSP"};
          static constexpr std::array<std::string_view, 3> keys = {
              "AudioFillGaps", "AudioPreservePitch", "MuteOnDisabledSpeedLimit"};
          const std::array<const Config::Info<bool>*, 3> infos = {
              &Config::MAIN_AUDIO_FILL_GAPS, &Config::MAIN_AUDIO_PRESERVE_PITCH,
              &Config::MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT};
          const int boolean_index = index - 4;
          const bool global = Config::Get(*infos[boolean_index]);
          if (per_game)
          {
            static constexpr std::array<std::string_view, 3> titles = {
                "Fill audio gaps", "Preserve pitch", "Mute when disabling speed limit"};
            EditPerGameBool(*game, titles[boolean_index], sections[boolean_index],
                            keys[boolean_index], global, delta, "On", "Off");
          }
          else
          {
            Config::SetBase(*infos[boolean_index], !global);
            MarkConfigDirty();
          }
        }
        return false;
      },
      false,
      [&](int index) {
        static constexpr std::array<std::string_view, 7> sections = {"DSP",  "Core", "Core", "Core",
                                                                     "Core", "Core", "DSP"};
        static constexpr std::array<std::string_view, 7> keys = {"Volume",
                                                                 "DSPHLE",
                                                                 "AudioLatency",
                                                                 "AudioBufferSize",
                                                                 "AudioFillGaps",
                                                                 "AudioPreservePitch",
                                                                 "MuteOnDisabledSpeedLimit"};
        if (index < 0 || index >= static_cast<int>(keys.size()))
          return false;
        if (per_game)
        {
          SetGameSetting(*game, sections[index], keys[index], std::nullopt);
          return true;
        }
        switch (index)
        {
        case 0:
          ResetConfigSetting(Config::MAIN_AUDIO_VOLUME);
          break;
        case 1:
          ResetConfigSetting(Config::MAIN_DSP_HLE);
          break;
        case 2:
          ResetConfigSetting(Config::MAIN_AUDIO_LATENCY);
          break;
        case 3:
          ResetConfigSetting(Config::MAIN_AUDIO_BUFFER_SIZE);
          break;
        case 4:
          ResetConfigSetting(Config::MAIN_AUDIO_FILL_GAPS);
          break;
        case 5:
          ResetConfigSetting(Config::MAIN_AUDIO_PRESERVE_PITCH);
          break;
        case 6:
          ResetConfigSetting(Config::MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT);
          break;
        }
        return true;
      });
  if (game)
    game->has_game_config = RegularFileExists(GameIniPath(*game));
}

void Launcher::ConsoleSettings(bool per_game, Game* game)
{
  static constexpr std::array<std::string_view, 6> GC_LANGUAGES = {"English", "German",  "French",
                                                                   "Spanish", "Italian", "Dutch"};
  static constexpr std::array<std::string_view, 10> WII_LANGUAGES = {"Japanese",
                                                                     "English",
                                                                     "German",
                                                                     "French",
                                                                     "Spanish",
                                                                     "Italian",
                                                                     "Dutch",
                                                                     "Simplified Chinese",
                                                                     "Traditional Chinese",
                                                                     "Korean"};
  const SystemLanguageDefaults& system_language = GetSystemLanguageDefaults();
  const int automatic_gc_language =
      std::clamp(system_language.gamecube_language, 0, static_cast<int>(GC_LANGUAGES.size()) - 1);
  const int automatic_wii_language = std::clamp(static_cast<int>(system_language.wii_language), 0,
                                                static_cast<int>(WII_LANGUAGES.size()) - 1);
  const std::string automatic_gc_label =
      "Auto (" + std::string(GC_LANGUAGES[automatic_gc_language]) + ")";
  const std::string automatic_wii_label =
      "Auto (" + std::string(WII_LANGUAGES[automatic_wii_language]) + ")";
  const auto get = [&](std::string_view section, std::string_view key) {
    return per_game && game ? GetGameSetting(*game, section, key) : std::nullopt;
  };
  const auto language_label = [&](bool wii, std::string_view section, std::string_view key,
                                  int global, std::span<const std::string_view> labels) {
    int value = global;
    if (const auto local = get(section, key))
    {
      value = std::atoi(local->c_str());
      value = std::clamp(value, 0, static_cast<int>(labels.size()) - 1);
      return std::string(labels[value]);
    }
    value = std::clamp(value, 0, static_cast<int>(labels.size()) - 1);
    const bool automatic = wii ? IsWiiLanguageAuto() : IsGameCubeLanguageAuto();
    const std::string global_label =
        automatic ? (wii ? automatic_wii_label : automatic_gc_label) : std::string(labels[value]);
    return per_game ? GlobalValueLabel(global_label) : global_label;
  };
  const auto bool_label = [&](std::string_view section, std::string_view key, bool global) {
    return per_game ? PerGameBoolLabel(*game, section, key, global) :
                      std::string(global ? "On" : "Off");
  };
  RunRows(
      per_game ? "Game console settings" : "GameCube & Wii", game ? game->title : std::string{},
      [&] {
        std::vector<Row> rows{
            {"GameCube language",
             language_label(false, "Core", "GameCubeLanguage",
                            Config::Get(Config::MAIN_GC_LANGUAGE), GC_LANGUAGES)},
            {"Wii language", language_label(true, "Wii", "Language",
                                            Config::Get(Config::SYSCONF_LANGUAGE), WII_LANGUAGES)},
            {"Wii widescreen",
             bool_label("Wii", "Widescreen", Config::Get(Config::SYSCONF_WIDESCREEN))},
            {"Progressive scan",
             bool_label("Core", "ProgressiveScan", Config::Get(Config::SYSCONF_PROGRESSIVE_SCAN))},
            {"PAL60", bool_label("Core", "PAL60", Config::Get(Config::SYSCONF_PAL60))},
        };
        if (!per_game)
        {
          rows.push_back({"Wii system settings", ">", true, false, false});
          rows.push_back({"GameCube Slot A / B", ">", true, false, false});
        }
        return rows;
      },
      [&](int index, int delta) {
        if (index <= 1)
        {
          const bool wii = index == 1;
          const std::string_view section = wii ? "Wii" : "Core";
          const std::string_view key = wii ? "Language" : "GameCubeLanguage";
          const std::span<const std::string_view> labels =
              wii ? std::span<const std::string_view>(WII_LANGUAGES) :
                    std::span<const std::string_view>(GC_LANGUAGES);
          const int global_value = wii ? static_cast<int>(Config::Get(Config::SYSCONF_LANGUAGE)) :
                                         Config::Get(Config::MAIN_GC_LANGUAGE);
          const bool global_automatic = wii ? IsWiiLanguageAuto() : IsGameCubeLanguageAuto();

          std::vector<std::string> choices;
          choices.reserve(labels.size() + 1);
          const std::string automatic_label = wii ? automatic_wii_label : automatic_gc_label;
          if (per_game)
          {
            const int inherited_value =
                std::clamp(global_value, 0, static_cast<int>(labels.size()) - 1);
            const std::string inherited =
                global_automatic ? automatic_label : std::string(labels[inherited_value]);
            choices.emplace_back(UseGlobalValueLabel(inherited));
          }
          else
          {
            choices.emplace_back(automatic_label);
          }
          for (const std::string_view label : labels)
            choices.emplace_back(label);

          int selected = 0;
          if (const auto local = get(section, key))
          {
            selected =
                std::clamp(std::atoi(local->c_str()), 0, static_cast<int>(labels.size()) - 1) + 1;
          }
          else if (!per_game && !global_automatic)
          {
            selected = std::clamp(global_value, 0, static_cast<int>(labels.size()) - 1) + 1;
          }

          if (delta == 0)
          {
            selected = Dropdown(wii ? "Wii language" : "GameCube language", choices, selected);
          }
          else
          {
            const int choice_count = static_cast<int>(choices.size());
            selected = (selected + (delta < 0 ? -1 : 1) + choice_count) % choice_count;
          }

          if (per_game)
          {
            if (selected == 0)
              SetGameSetting(*game, section, key, std::nullopt);
            else
              SetGameSetting(*game, section, key, std::to_string(selected - 1));
          }
          else
          {
            if (wii)
              SetWiiLanguageAuto(selected == 0);
            else
              SetGameCubeLanguageAuto(selected == 0);

            if (selected == 0)
              ApplyAutoLanguageDefaults();
            else if (wii)
              Config::SetBase(Config::SYSCONF_LANGUAGE, static_cast<u32>(selected - 1));
            else
              Config::SetBase(Config::MAIN_GC_LANGUAGE, selected - 1);
          }
        }
        else if (index <= 4)
        {
          const std::array<std::string_view, 3> sections = {"Wii", "Core", "Core"};
          const std::array<std::string_view, 3> keys = {"Widescreen", "ProgressiveScan", "PAL60"};
          const std::array<bool, 3> globals = {Config::Get(Config::SYSCONF_WIDESCREEN),
                                               Config::Get(Config::SYSCONF_PROGRESSIVE_SCAN),
                                               Config::Get(Config::SYSCONF_PAL60)};
          const int item = index - 2;
          if (per_game)
          {
            static constexpr std::array<std::string_view, 3> titles = {"Wii widescreen",
                                                                       "Progressive scan", "PAL60"};
            EditPerGameBool(*game, titles[item], sections[item], keys[item], globals[item], delta);
          }
          else if (item == 0)
            Config::SetBase(Config::SYSCONF_WIDESCREEN, !globals[item]);
          else if (item == 1)
            Config::SetBase(Config::SYSCONF_PROGRESSIVE_SCAN, !globals[item]);
          else
            Config::SetBase(Config::SYSCONF_PAL60, !globals[item]);
        }
        else if (!per_game && index == 5)
        {
          WiiSystemSettings();
        }
        else if (!per_game && index == 6)
        {
          GameCubeDeviceSettings();
        }
        if (!per_game)
          MarkConfigDirty();
        return false;
      },
      false,
      [&](int index) {
        if (index < 0 || index > 4)
          return false;
        if (per_game)
        {
          static constexpr std::array<std::string_view, 5> sections = {"Core", "Wii", "Wii", "Core",
                                                                       "Core"};
          static constexpr std::array<std::string_view, 5> keys = {
              "GameCubeLanguage", "Language", "Widescreen", "ProgressiveScan", "PAL60"};
          SetGameSetting(*game, sections[index], keys[index], std::nullopt);
          return true;
        }
        if (index == 0)
        {
          SetGameCubeLanguageAuto(true);
          ApplyAutoLanguageDefaults();
          MarkConfigDirty();
        }
        else if (index == 1)
        {
          SetWiiLanguageAuto(true);
          ApplyAutoLanguageDefaults();
          MarkConfigDirty();
        }
        else if (index == 2)
          ResetConfigSetting(Config::SYSCONF_WIDESCREEN);
        else if (index == 3)
          ResetConfigSetting(Config::SYSCONF_PROGRESSIVE_SCAN);
        else
          ResetConfigSetting(Config::SYSCONF_PAL60);
        return true;
      });
  if (game)
    game->has_game_config = RegularFileExists(GameIniPath(*game));
}

void Launcher::WiiSystemSettings()
{
  static constexpr std::array<std::string_view, 3> SOUND_MODES = {"Mono", "Stereo", "Surround"};
  static constexpr std::array<std::string_view, 2> SENSOR_POSITIONS = {"Bottom", "Top"};
  static constexpr std::array<std::string_view, 11> SD_SIZE_LABELS = {
      "Auto",  "64 MiB",       "128 MiB",      "256 MiB",       "512 MiB",      "1 GiB",
      "2 GiB", "4 GiB (SDHC)", "8 GiB (SDHC)", "16 GiB (SDHC)", "32 GiB (SDHC)"};
  static constexpr u64 MIB = 1024ULL * 1024ULL;
  static constexpr std::array<u64, 11> SD_SIZES = {0,          64 * MIB,    128 * MIB,  256 * MIB,
                                                   512 * MIB,  1024 * MIB,  2048 * MIB, 4096 * MIB,
                                                   8192 * MIB, 16384 * MIB, 32768 * MIB};
  static constexpr std::array<u32, 5> IR_SENSITIVITY_PRESETS = {1, 2, 3, 4, 5};
  static constexpr std::array<u32, 10> SPEAKER_VOLUME_PRESETS = {0,  16, 32, 48,  64,
                                                                 80, 88, 96, 112, 127};

  const auto size_index = [&] {
    const u64 configured = Config::Get(Config::MAIN_WII_SD_CARD_FILESIZE);
    const auto found = std::ranges::find(SD_SIZES, configured);
    return found == SD_SIZES.end() ? 0 : static_cast<int>(found - SD_SIZES.begin());
  };
  const auto path_label = [](const std::string& path, std::string_view default_label) {
    return path.empty() ? std::string(default_label) : path;
  };
  const auto choose_path = [&](const Config::Info<std::string>& info, bool folder,
                               std::span<const std::string_view> extensions,
                               std::string_view title) {
    const std::string configured = Config::Get(info);
    const int choice = Dropdown(title, {"Use Dolphin default", "Choose custom path"}, -1);
    if (choice == 0)
    {
      Config::SetBase(info, std::string{});
      MarkConfigDirty();
      return;
    }
    if (choice != 1)
      return;
    const std::string start =
        configured.empty() ? std::string{} : (folder ? configured : ParentPath(configured));
    const std::string selected = FileBrowser(start, folder, !folder, false, extensions, title);
    if (!selected.empty())
    {
      Config::SetBase(info, selected);
      MarkConfigDirty();
    }
  };
  const auto edit_u32 = [&](std::string_view title, const Config::Info<u32>& info,
                            std::span<const u32> presets, int delta) {
    const u32 minimum = presets.front();
    const u32 maximum = presets.back();
    u32 value = std::clamp(Config::Get(info), minimum, maximum);
    std::vector<u32> values(presets.begin(), presets.end());
    if (!std::ranges::contains(values, value))
    {
      values.push_back(value);
      std::ranges::sort(values);
    }
    std::vector<std::string> choices;
    choices.reserve(values.size());
    for (const u32 preset : values)
      choices.push_back(std::to_string(preset));
    int selected = static_cast<int>(std::ranges::find(values, value) - values.begin());
    if (delta == 0)
      selected = Dropdown(title, choices, selected, true, false);
    else
      selected = (selected + (delta < 0 ? -1 : 1) + static_cast<int>(choices.size())) %
                 static_cast<int>(choices.size());
    if (selected < 0)
      return;
    value = values[selected];
    Config::SetBase(info, value);
    MarkConfigDirty();
  };

  RunRows(
      "Wii system settings", "SD card, sensor bar and Wii Remote audio",
      [&] {
        const int sound =
            std::clamp(static_cast<int>(Config::Get(Config::SYSCONF_SOUND_MODE)), 0, 2);
        const int sensor =
            std::clamp(static_cast<int>(Config::Get(Config::SYSCONF_SENSOR_BAR_POSITION)), 0, 1);
        return std::vector<Row>{
            {"Sound mode", std::string(SOUND_MODES[sound])},
            {"Insert SD card", Config::Get(Config::MAIN_WII_SD_CARD) ? "On" : "Off"},
            {"Allow writes to SD card", Config::Get(Config::MAIN_ALLOW_SD_WRITES) ? "On" : "Off",
             Config::Get(Config::MAIN_WII_SD_CARD)},
            {"SD card image path",
             path_label(Config::Get(Config::MAIN_WII_SD_CARD_IMAGE_PATH),
                        m_localization.Translate("Dolphin default")),
             true, false, false, true, false},
            {"Automatically sync SD folder",
             Config::Get(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC) ? "On" : "Off"},
            {"SD sync folder",
             path_label(Config::Get(Config::MAIN_WII_SD_CARD_SYNC_FOLDER_PATH),
                        m_localization.Translate("Dolphin default")),
             true, false, false, true, false},
            {"SD card file size", std::string(SD_SIZE_LABELS[size_index()])},
            {"Sensor bar position", std::string(SENSOR_POSITIONS[sensor])},
            {"IR sensitivity", std::to_string(std::clamp(
                                   Config::Get(Config::SYSCONF_SENSOR_BAR_SENSITIVITY), 1U, 5U))},
            {"Enable Wii Remote speaker",
             Config::Get(Config::MAIN_WIIMOTE_ENABLE_SPEAKER) ? "On" : "Off"},
            {"Wii Remote speaker volume",
             std::to_string(std::clamp(Config::Get(Config::SYSCONF_SPEAKER_VOLUME), 0U, 127U)),
             Config::Get(Config::MAIN_WIIMOTE_ENABLE_SPEAKER)},
            {"Enable Wii Remote rumble", Config::Get(Config::SYSCONF_WIIMOTE_MOTOR) ? "On" : "Off"},
        };
      },
      [&](int index, int delta) {
        if (index == 0)
        {
          const int current =
              std::clamp(static_cast<int>(Config::Get(Config::SYSCONF_SOUND_MODE)), 0, 2);
          Config::SetBase(
              Config::SYSCONF_SOUND_MODE,
              static_cast<u32>(SelectChoice("Wii sound mode", SOUND_MODES, current, delta)));
        }
        else if (index == 1)
        {
          Config::SetBase(Config::MAIN_WII_SD_CARD, !Config::Get(Config::MAIN_WII_SD_CARD));
        }
        else if (index == 2)
        {
          Config::SetBase(Config::MAIN_ALLOW_SD_WRITES, !Config::Get(Config::MAIN_ALLOW_SD_WRITES));
        }
        else if (index == 3)
        {
          static constexpr std::array<std::string_view, 1> extensions = {".raw"};
          choose_path(Config::MAIN_WII_SD_CARD_IMAGE_PATH, false, extensions, "Wii SD card image");
          return false;
        }
        else if (index == 4)
        {
          Config::SetBase(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC,
                          !Config::Get(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC));
        }
        else if (index == 5)
        {
          choose_path(Config::MAIN_WII_SD_CARD_SYNC_FOLDER_PATH, true, {}, "Wii SD sync folder");
          return false;
        }
        else if (index == 6)
        {
          const int selected =
              SelectChoice("SD card file size", SD_SIZE_LABELS, size_index(), delta);
          Config::SetBase(Config::MAIN_WII_SD_CARD_FILESIZE, SD_SIZES[selected]);
        }
        else if (index == 7)
        {
          const int current =
              std::clamp(static_cast<int>(Config::Get(Config::SYSCONF_SENSOR_BAR_POSITION)), 0, 1);
          Config::SetBase(Config::SYSCONF_SENSOR_BAR_POSITION,
                          static_cast<u32>(SelectChoice("Sensor bar position", SENSOR_POSITIONS,
                                                        current, delta)));
        }
        else if (index == 8)
        {
          edit_u32("IR sensitivity", Config::SYSCONF_SENSOR_BAR_SENSITIVITY, IR_SENSITIVITY_PRESETS,
                   delta);
          return false;
        }
        else if (index == 9)
        {
          Config::SetBase(Config::MAIN_WIIMOTE_ENABLE_SPEAKER,
                          !Config::Get(Config::MAIN_WIIMOTE_ENABLE_SPEAKER));
        }
        else if (index == 10)
        {
          edit_u32("Wii Remote speaker volume", Config::SYSCONF_SPEAKER_VOLUME,
                   SPEAKER_VOLUME_PRESETS, delta);
          return false;
        }
        else
        {
          Config::SetBase(Config::SYSCONF_WIIMOTE_MOTOR,
                          !Config::Get(Config::SYSCONF_WIIMOTE_MOTOR));
        }
        MarkConfigDirty();
        return false;
      },
      false,
      [&](int index) {
        switch (index)
        {
        case 0:
          ResetConfigSetting(Config::SYSCONF_SOUND_MODE);
          break;
        case 1:
          ResetConfigSetting(Config::MAIN_WII_SD_CARD);
          break;
        case 2:
          ResetConfigSetting(Config::MAIN_ALLOW_SD_WRITES);
          break;
        case 3:
          ResetConfigSetting(Config::MAIN_WII_SD_CARD_IMAGE_PATH);
          break;
        case 4:
          ResetConfigSetting(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC);
          break;
        case 5:
          ResetConfigSetting(Config::MAIN_WII_SD_CARD_SYNC_FOLDER_PATH);
          break;
        case 6:
          ResetConfigSetting(Config::MAIN_WII_SD_CARD_FILESIZE);
          break;
        case 7:
          ResetConfigSetting(Config::SYSCONF_SENSOR_BAR_POSITION);
          break;
        case 8:
          ResetConfigSetting(Config::SYSCONF_SENSOR_BAR_SENSITIVITY);
          break;
        case 9:
          ResetConfigSetting(Config::MAIN_WIIMOTE_ENABLE_SPEAKER);
          break;
        case 10:
          ResetConfigSetting(Config::SYSCONF_SPEAKER_VOLUME);
          break;
        case 11:
          ResetConfigSetting(Config::SYSCONF_WIIMOTE_MOTOR);
          break;
        default:
          return false;
        }
        return true;
      },
      [](int index) { return index == 3 || index == 5; });
}

void Launcher::GameCubeDeviceSettings()
{
  const auto device_name = [](ExpansionInterface::EXIDeviceType device) -> std::string_view {
    using ExpansionInterface::EXIDeviceType;
    switch (device)
    {
    case EXIDeviceType::None:
      return "None";
    case EXIDeviceType::Dummy:
      return "Dummy";
    case EXIDeviceType::MemoryCard:
      return "Memory Card";
    case EXIDeviceType::MemoryCardFolder:
      return "GCI Folder";
    case EXIDeviceType::Gecko:
      return "USB Gecko";
    case EXIDeviceType::AGP:
      return "Advance Game Port";
    case EXIDeviceType::Microphone:
      return "Microphone";
    default:
      return "Unsupported";
    }
  };
  RunRows(
      "GameCube device settings", "IPL and expansion slots",
      [&] {
        return std::vector<Row>{
            {"Skip GameCube Main Menu", Config::Get(Config::MAIN_SKIP_IPL) ? "On" : "Off"},
            {"Slot A",
             std::string(device_name(
                 Config::Get(Config::GetInfoForEXIDevice(ExpansionInterface::Slot::A)))),
             true, false, false},
            {"Slot B",
             std::string(device_name(
                 Config::Get(Config::GetInfoForEXIDevice(ExpansionInterface::Slot::B)))),
             true, false, false},
        };
      },
      [&](int index, int) {
        if (index == 0)
        {
          Config::SetBase(Config::MAIN_SKIP_IPL, !Config::Get(Config::MAIN_SKIP_IPL));
          MarkConfigDirty();
        }
        else
        {
          GameCubeSlotSettings(index - 1);
        }
        return false;
      },
      false,
      [&](int index) {
        if (index != 0)
          return false;
        ResetConfigSetting(Config::MAIN_SKIP_IPL);
        return true;
      });
}

void Launcher::GameCubeSlotSettings(int slot_number)
{
  using ExpansionInterface::EXIDeviceType;
  const ExpansionInterface::Slot slot =
      slot_number == 0 ? ExpansionInterface::Slot::A : ExpansionInterface::Slot::B;
  static constexpr std::array<EXIDeviceType, 7> DEVICE_VALUES = {
      EXIDeviceType::None,       EXIDeviceType::Dummy,
      EXIDeviceType::MemoryCard, EXIDeviceType::MemoryCardFolder,
      EXIDeviceType::Gecko,      EXIDeviceType::AGP,
      EXIDeviceType::Microphone};
  static constexpr std::array<std::string_view, 7> DEVICE_LABELS = {
      "None", "Dummy", "Memory Card", "GCI Folder", "USB Gecko", "Advance Game Port", "Microphone"};
  const auto current_device_index = [&] {
    const EXIDeviceType current = Config::Get(Config::GetInfoForEXIDevice(slot));
    const auto found = std::ranges::find(DEVICE_VALUES, current);
    return found == DEVICE_VALUES.end() ? 0 : static_cast<int>(found - DEVICE_VALUES.begin());
  };
  const auto path_label = [&](const std::string& configured, const std::string& resolved) {
    return configured.empty() ? std::string(m_localization.Translate("Default: ")) + resolved :
                                configured;
  };
  const auto choose_path = [&](const Config::Info<std::string>& info, bool folder,
                               std::span<const std::string_view> extensions,
                               std::string_view title) {
    const std::string configured = Config::Get(info);
    const int choice = Dropdown(title, {"Use Dolphin default", "Choose custom path"}, -1);
    if (choice == 0)
    {
      Config::SetBase(info, std::string{});
      MarkConfigDirty();
      return;
    }
    if (choice != 1)
      return;
    const std::string start =
        configured.empty() ? std::string{} : (folder ? configured : ParentPath(configured));
    const std::string selected = FileBrowser(start, folder, !folder, false, extensions, title);
    if (!selected.empty())
    {
      Config::SetBase(info, selected);
      MarkConfigDirty();
    }
  };

  const std::string slot_name = std::string("GameCube Slot ") + (slot_number == 0 ? "A" : "B");
  RunRows(
      slot_name, "EXI device configuration",
      [&] {
        const EXIDeviceType device = Config::Get(Config::GetInfoForEXIDevice(slot));
        std::vector<Row> rows{{"Device", std::string(DEVICE_LABELS[current_device_index()])}};
        if (device == EXIDeviceType::MemoryCard)
        {
          const std::string configured = Config::Get(Config::GetInfoForMemcardPath(slot));
          rows.push_back({"Memory card path",
                          path_label(configured, Config::GetMemcardPath(slot, std::nullopt)), true,
                          false, false, true, false});
        }
        else if (device == EXIDeviceType::MemoryCardFolder)
        {
          const std::string configured = Config::Get(Config::GetInfoForGCIPath(slot));
          rows.push_back({"GCI folder path",
                          path_label(configured, Config::GetGCIFolderPath(slot, std::nullopt)),
                          true, false, false, true, false});
        }
        else if (device == EXIDeviceType::AGP)
        {
          const std::string configured = Config::Get(Config::GetInfoForAGPCartPath(slot));
          rows.push_back({"GBA cartridge path", configured.empty() ? "Not selected" : configured,
                          true, false, false, true, configured.empty()});
        }
        return rows;
      },
      [&](int index, int delta) {
        const EXIDeviceType device = Config::Get(Config::GetInfoForEXIDevice(slot));
        if (index == 0)
        {
          const int selected =
              SelectChoice(slot_name + " device", DEVICE_LABELS, current_device_index(), delta);
          Config::SetBase(Config::GetInfoForEXIDevice(slot), DEVICE_VALUES[selected]);
          MarkConfigDirty();
        }
        else if (device == EXIDeviceType::MemoryCard)
        {
          static constexpr std::array<std::string_view, 2> extensions = {".raw", ".gcp"};
          choose_path(Config::GetInfoForMemcardPath(slot), false, extensions,
                      slot_name + " memory card");
        }
        else if (device == EXIDeviceType::MemoryCardFolder)
        {
          choose_path(Config::GetInfoForGCIPath(slot), true, {}, slot_name + " GCI folder");
        }
        else if (device == EXIDeviceType::AGP)
        {
          static constexpr std::array<std::string_view, 1> extensions = {".gba"};
          choose_path(Config::GetInfoForAGPCartPath(slot), false, extensions,
                      slot_name + " GBA cartridge");
        }
        return false;
      },
      false,
      [&](int index) {
        const EXIDeviceType device = Config::Get(Config::GetInfoForEXIDevice(slot));
        if (index == 0)
          ResetConfigSetting(Config::GetInfoForEXIDevice(slot));
        else if (device == EXIDeviceType::MemoryCard)
          ResetConfigSetting(Config::GetInfoForMemcardPath(slot));
        else if (device == EXIDeviceType::MemoryCardFolder)
          ResetConfigSetting(Config::GetInfoForGCIPath(slot));
        else if (device == EXIDeviceType::AGP)
          ResetConfigSetting(Config::GetInfoForAGPCartPath(slot));
        else
          return false;
        return true;
      },
      [](int index) { return index >= 0; });
}

ControllerTarget Launcher::GetControllerTarget(bool wii, int port, bool per_game, Game* game,
                                               bool create)
{
  ControllerTarget global{ControllerConfigPath(wii), ControllerSectionName(wii, port), false};
  if (!per_game || !game)
    return global;

  const std::string setting_key =
      std::string(wii ? "WiimoteProfile" : "PadProfile") + std::to_string(port + 1);
  const std::optional<std::string> configured = GetGameSetting(*game, "Controls", setting_key);
  std::string configured_name;
  if (configured && !configured->empty())
    configured_name = ExistingProfileName(*configured);

  const std::string directory = ControllerProfileDirectory(wii);
  if (!create)
  {
    if (configured_name.empty())
    {
      global.inherited = true;
      return global;
    }
    return {directory + configured_name + ".ini", "Profile", false};
  }

  const std::string generated_name = GeneratedControllerProfileName(game->key, wii, port);
  const ControllerTarget generated{directory + generated_name + ".ini", "Profile", false};
  if (configured_name == generated_name && RegularFileExists(generated.path))
  {
    if (ReadControllerValue(global, "Device").empty())
      WriteControllerValue(global, "Device", SwitchControllerDevice(port));
    return generated;
  }

  File::CreateDirs(directory);
  // A Device entry enables Dolphin's per-game controller profile layer.
  if (ReadControllerValue(global, "Device").empty())
    WriteControllerValue(global, "Device", SwitchControllerDevice(port));
  ControllerTarget source = global;
  if (!configured_name.empty() && configured_name != generated_name)
    source = {directory + configured_name + ".ini", "Profile", false};
  CopyControllerTarget(source, generated);
  SetGameSetting(*game, "Controls", setting_key, generated_name);
  return generated;
}

std::vector<InputBinding> Launcher::GetControllerBindings(bool wii, std::string_view extension,
                                                          bool extension_only, bool triforce,
                                                          bool orientation_hotkeys) const
{
  std::vector<InputBinding> bindings;
  const auto add = [&](std::string label, std::string key, std::string expression) {
    bindings.push_back({std::move(label), std::move(key), std::move(expression)});
  };
  if (!wii)
  {
    for (const auto& [label, key, expression] :
         std::array<std::array<const char*, 3>, 6>{{{{"A", "Buttons/A", "`A`"}},
                                                    {{"B", "Buttons/B", "`B`"}},
                                                    {{"X", "Buttons/X", "`X`"}},
                                                    {{"Y", "Buttons/Y", "`Y`"}},
                                                    {{"Z", "Buttons/Z", "`R2`"}},
                                                    {{"Start", "Buttons/Start", "`Start`"}}}})
      add(label, key, expression);
    for (const auto& [label, key, expression] :
         std::array<std::array<const char*, 3>, 4>{{{{"D-Pad Up", "D-Pad/Up", "`Up`"}},
                                                    {{"D-Pad Down", "D-Pad/Down", "`Down`"}},
                                                    {{"D-Pad Left", "D-Pad/Left", "`Left`"}},
                                                    {{"D-Pad Right", "D-Pad/Right", "`Right`"}}}})
      add(label, key, expression);
    for (const auto& [label, key, expression] : std::array<std::array<const char*, 3>, 8>{{
             {{"Control Stick Up", "Main Stick/Up", "`Y0+`"}},
             {{"Control Stick Down", "Main Stick/Down", "`Y0-`"}},
             {{"Control Stick Left", "Main Stick/Left", "`X0-`"}},
             {{"Control Stick Right", "Main Stick/Right", "`X0+`"}},
             {{"C-Stick Up", "C-Stick/Up", "`Y1+`"}},
             {{"C-Stick Down", "C-Stick/Down", "`Y1-`"}},
             {{"C-Stick Left", "C-Stick/Left", "`X1-`"}},
             {{"C-Stick Right", "C-Stick/Right", "`X1+`"}},
         }})
      add(label, key, expression);
    for (const auto& [label, key, expression] : std::array<std::array<const char*, 3>, 4>{
             {{{"Trigger L", "Triggers/L", "`L`"}},
              {{"Trigger R", "Triggers/R", "`R`"}},
              {{"Analog L", "Triggers/L-Analog", "`Trigger L`"}},
              {{"Analog R", "Triggers/R-Analog", "`Trigger R`"}}}})
      add(label, key, expression);
    if (triforce)
    {
      add("Triforce Test", "Triforce/Test", "`L3`");
      add("Triforce Service", "Triforce/Service", "`R3`");
      add("Triforce Coin", "Triforce/Coin", "`Select`");
    }
    return bindings;
  }

  if (orientation_hotkeys)
  {
    add("Sideways Toggle", "Hotkeys/Sideways Toggle", "");
    add("Upright Toggle", "Hotkeys/Upright Toggle", "");
    add("Sideways Hold", "Hotkeys/Sideways Hold", "");
    add("Upright Hold", "Hotkeys/Upright Hold", "");
    return bindings;
  }

  if (!extension_only)
  {
    for (const auto& [label, key, expression] :
         std::array<std::array<const char*, 3>, 7>{{{{"A", "Buttons/A", "`A`"}},
                                                    {{"B", "Buttons/B", "`R2`"}},
                                                    {{"1", "Buttons/1", "`X`"}},
                                                    {{"2", "Buttons/2", "`B`"}},
                                                    {{"Minus", "Buttons/-", "`Select`"}},
                                                    {{"Plus", "Buttons/+", "`Start`"}},
                                                    {{"Home", "Buttons/Home", "`L3`"}}}})
      add(label, key, expression);
    for (const auto& [label, key, expression] :
         std::array<std::array<const char*, 3>, 4>{{{{"D-Pad Up", "D-Pad/Up", "`Up`"}},
                                                    {{"D-Pad Down", "D-Pad/Down", "`Down`"}},
                                                    {{"D-Pad Left", "D-Pad/Left", "`Left`"}},
                                                    {{"D-Pad Right", "D-Pad/Right", "`Right`"}}}})
      add(label, key, expression);
    for (const auto& [label, key, expression] : std::array<std::array<const char*, 3>, 8>{{
             {{"Pointer Up", "IR/Up", "`Y1+`"}},
             {{"Pointer Down", "IR/Down", "`Y1-`"}},
             {{"Pointer Left", "IR/Left", "`X1-`"}},
             {{"Pointer Right", "IR/Right", "`X1+`"}},
             {{"Pointer recenter", "IMUIR/Recenter", "`R3`"}},
             {{"Shake X", "Shake/X", "`R3`"}},
             {{"Shake Y", "Shake/Y", "`R3`"}},
             {{"Shake Z", "Shake/Z", "`R3`"}},
         }})
      add(label, key, expression);
    for (const auto& [label, key, expression] : std::array<std::array<const char*, 3>, 12>{{
             {{"Accelerometer Up", "IMUAccelerometer/Up", "`Accel Up`"}},
             {{"Accelerometer Down", "IMUAccelerometer/Down", "`Accel Down`"}},
             {{"Accelerometer Left", "IMUAccelerometer/Left", "`Accel Left`"}},
             {{"Accelerometer Right", "IMUAccelerometer/Right", "`Accel Right`"}},
             {{"Accelerometer Forward", "IMUAccelerometer/Forward", "`Accel Forward`"}},
             {{"Accelerometer Backward", "IMUAccelerometer/Backward", "`Accel Backward`"}},
             {{"Gyroscope Pitch Up", "IMUGyroscope/Pitch Up", "`Gyro Pitch Up`"}},
             {{"Gyroscope Pitch Down", "IMUGyroscope/Pitch Down", "`Gyro Pitch Down`"}},
             {{"Gyroscope Roll Left", "IMUGyroscope/Roll Left", "`Gyro Roll Left`"}},
             {{"Gyroscope Roll Right", "IMUGyroscope/Roll Right", "`Gyro Roll Right`"}},
             {{"Gyroscope Yaw Left", "IMUGyroscope/Yaw Left", "`Gyro Yaw Left`"}},
             {{"Gyroscope Yaw Right", "IMUGyroscope/Yaw Right", "`Gyro Yaw Right`"}},
         }})
      add(label, key, expression);
    return bindings;
  }

  if (extension == "Nunchuk")
  {
    add("Nunchuk C", "Nunchuk/Buttons/C", "`L`");
    add("Nunchuk Z", "Nunchuk/Buttons/Z", "`Z`");
    add("Nunchuk Stick Up", "Nunchuk/Stick/Up", "`Y0+`");
    add("Nunchuk Stick Down", "Nunchuk/Stick/Down", "`Y0-`");
    add("Nunchuk Stick Left", "Nunchuk/Stick/Left", "`X0-`");
    add("Nunchuk Stick Right", "Nunchuk/Stick/Right", "`X0+`");
    add("Nunchuk Shake X", "Nunchuk/Shake/X", "`R3`");
    add("Nunchuk Shake Y", "Nunchuk/Shake/Y", "`R3`");
    add("Nunchuk Shake Z", "Nunchuk/Shake/Z", "`R3`");
    add("Nunchuk Accel Up", "Nunchuk/IMUAccelerometer/Up", "`Nunchuk Accel Up`");
    add("Nunchuk Accel Down", "Nunchuk/IMUAccelerometer/Down", "`Nunchuk Accel Down`");
    add("Nunchuk Accel Left", "Nunchuk/IMUAccelerometer/Left", "`Nunchuk Accel Left`");
    add("Nunchuk Accel Right", "Nunchuk/IMUAccelerometer/Right", "`Nunchuk Accel Right`");
    add("Nunchuk Accel Forward", "Nunchuk/IMUAccelerometer/Forward", "`Nunchuk Accel Forward`");
    add("Nunchuk Accel Backward", "Nunchuk/IMUAccelerometer/Backward", "`Nunchuk Accel Backward`");
  }
  else if (extension == "Classic")
  {
    for (const auto& [label, control, expression] :
         std::array<std::array<const char*, 3>, 9>{{{{"Classic A", "A", "`A`"}},
                                                    {{"Classic B", "B", "`B`"}},
                                                    {{"Classic X", "X", "`X`"}},
                                                    {{"Classic Y", "Y", "`Y`"}},
                                                    {{"Classic ZL", "ZL", "`Z`"}},
                                                    {{"Classic ZR", "ZR", "`R2`"}},
                                                    {{"Classic Minus", "-", "`Select`"}},
                                                    {{"Classic Plus", "+", "`Start`"}},
                                                    {{"Classic Home", "Home", "`L3`"}}}})
      add(label, "Classic/Buttons/" + std::string(control), expression);
    for (const auto& [label, key, expression] : std::array<std::array<const char*, 3>, 8>{{
             {{"Classic Left Up", "Classic/Left Stick/Up", "`Y0+`"}},
             {{"Classic Left Down", "Classic/Left Stick/Down", "`Y0-`"}},
             {{"Classic Left Left", "Classic/Left Stick/Left", "`X0-`"}},
             {{"Classic Left Right", "Classic/Left Stick/Right", "`X0+`"}},
             {{"Classic Right Up", "Classic/Right Stick/Up", "`Y1+`"}},
             {{"Classic Right Down", "Classic/Right Stick/Down", "`Y1-`"}},
             {{"Classic Right Left", "Classic/Right Stick/Left", "`X1-`"}},
             {{"Classic Right Right", "Classic/Right Stick/Right", "`X1+`"}},
         }})
      add(label, key, expression);
    for (const auto& [label, key, expression] : std::array<std::array<const char*, 3>, 8>{
             {{{"Classic L", "Classic/Triggers/L", "`L`"}},
              {{"Classic R", "Classic/Triggers/R", "`R`"}},
              {{"Classic analog L", "Classic/Triggers/L-Analog", "`L`"}},
              {{"Classic analog R", "Classic/Triggers/R-Analog", "`R`"}},
              {{"Classic D-Pad Up", "Classic/D-Pad/Up", "`Up`"}},
              {{"Classic D-Pad Down", "Classic/D-Pad/Down", "`Down`"}},
              {{"Classic D-Pad Left", "Classic/D-Pad/Left", "`Left`"}},
              {{"Classic D-Pad Right", "Classic/D-Pad/Right", "`Right`"}}}})
      add(label, key, expression);
  }
  else if (extension == "Guitar")
  {
    add("Green fret", "Guitar/Frets/Green", "`A`");
    add("Red fret", "Guitar/Frets/Red", "`B`");
    add("Yellow fret", "Guitar/Frets/Yellow", "`X`");
    add("Blue fret", "Guitar/Frets/Blue", "`Y`");
    add("Orange fret", "Guitar/Frets/Orange", "`R`");
    add("Strum Up", "Guitar/Strum/Up", "`Up`");
    add("Strum Down", "Guitar/Strum/Down", "`Down`");
    add("Whammy", "Guitar/Whammy/Bar", "`R2`");
    add("Stick Up", "Guitar/Stick/Up", "`Y0+`");
    add("Stick Down", "Guitar/Stick/Down", "`Y0-`");
    add("Stick Left", "Guitar/Stick/Left", "`X0-`");
    add("Stick Right", "Guitar/Stick/Right", "`X0+`");
    add("Slider left", "Guitar/Slider Bar/Left", "`L`");
    add("Slider right", "Guitar/Slider Bar/Right", "`R`");
    add("Minus", "Guitar/Buttons/-", "`Select`");
    add("Plus", "Guitar/Buttons/+", "`Start`");
  }
  else if (extension == "Drums")
  {
    add("Red pad", "Drums/Pads/Red", "`B`");
    add("Yellow pad", "Drums/Pads/Yellow", "`X`");
    add("Blue pad", "Drums/Pads/Blue", "`Y`");
    add("Orange pad", "Drums/Pads/Orange", "`R`");
    add("Green pad", "Drums/Pads/Green", "`A`");
    add("Bass pedal", "Drums/Pads/Bass", "`Z`");
    add("Stick Up", "Drums/Stick/Up", "`Y0+`");
    add("Stick Down", "Drums/Stick/Down", "`Y0-`");
    add("Stick Left", "Drums/Stick/Left", "`X0-`");
    add("Stick Right", "Drums/Stick/Right", "`X0+`");
    add("Minus", "Drums/Buttons/-", "`Select`");
    add("Plus", "Drums/Buttons/+", "`Start`");
  }
  else if (extension == "Turntable")
  {
    add("Green Left", "Turntable/Buttons/Green Left", "`Left`");
    add("Red Left", "Turntable/Buttons/Red Left", "`B`");
    add("Blue Left", "Turntable/Buttons/Blue Left", "`X`");
    add("Green Right", "Turntable/Buttons/Green Right", "`Right`");
    add("Red Right", "Turntable/Buttons/Red Right", "`A`");
    add("Blue Right", "Turntable/Buttons/Blue Right", "`Y`");
    add("Euphoria", "Turntable/Buttons/Euphoria", "`R3`");
    add("Stick Up", "Turntable/Stick/Up", "`Y0+`");
    add("Stick Down", "Turntable/Stick/Down", "`Y0-`");
    add("Stick Left", "Turntable/Stick/Left", "`X0-`");
    add("Stick Right", "Turntable/Stick/Right", "`X0+`");
    add("Left table reverse", "Turntable/Table Left/Left", "`L`");
    add("Left table forward", "Turntable/Table Left/Right", "`Z`");
    add("Right table reverse", "Turntable/Table Right/Left", "`R`");
    add("Right table forward", "Turntable/Table Right/Right", "`R2`");
    add("Effect dial left", "Turntable/Effect/Left", "`Left`");
    add("Effect dial right", "Turntable/Effect/Right", "`Right`");
    add("Crossfade left", "Turntable/Crossfade/Left", "`L`");
    add("Crossfade right", "Turntable/Crossfade/Right", "`R`");
    add("Minus", "Turntable/Buttons/-", "`Select`");
    add("Plus", "Turntable/Buttons/+", "`Start`");
  }
  else if (extension == "uDraw" || extension == "Drawsome")
  {
    const std::string prefix = std::string(extension) + "/";
    add("Stylus Up", prefix + "Stylus/Up", "`Y1+`");
    add("Stylus Down", prefix + "Stylus/Down", "`Y1-`");
    add("Stylus Left", prefix + "Stylus/Left", "`X1-`");
    add("Stylus Right", prefix + "Stylus/Right", "`X1+`");
    add("Stylus pressure", prefix + "Touch/Pressure", "`R2`");
    add("Lift stylus", prefix + "Touch/Lift", "`R`");
    if (extension == "uDraw")
    {
      add("Rocker Up", "uDraw/Buttons/Rocker Up", "`Up`");
      add("Rocker Down", "uDraw/Buttons/Rocker Down", "`Down`");
    }
  }
  else if (extension == "TaTaCon")
  {
    add("Center Left", "TaTaCon/Center/Left", "`B`");
    add("Center Right", "TaTaCon/Center/Right", "`A`");
    add("Rim Left", "TaTaCon/Rim/Left", "`X`");
    add("Rim Right", "TaTaCon/Rim/Right", "`Y`");
  }
  else if (extension == "Shinkansen")
  {
    add("Up", "Shinkansen/Buttons/Up", "`Up`");
    add("Down", "Shinkansen/Buttons/Down", "`Down`");
    add("Left", "Shinkansen/Buttons/Left", "`Left`");
    add("Right", "Shinkansen/Buttons/Right", "`Right`");
    add("A", "Shinkansen/Buttons/A", "`A`");
    add("B", "Shinkansen/Buttons/B", "`B`");
    add("C", "Shinkansen/Buttons/C", "`X`");
    add("D", "Shinkansen/Buttons/D", "`Y`");
    add("Select", "Shinkansen/Buttons/SELECT", "`Select`");
    add("Start", "Shinkansen/Buttons/START", "`Start`");
    add("Brake lever", "Shinkansen/Levers/L", "`Z`");
    add("Power lever", "Shinkansen/Levers/R", "`R2`");
  }
  return bindings;
}

void Launcher::RenderControllerCapture(const InputBinding& binding, int position, int count,
                                       bool releasing, std::string_view current,
                                       std::string_view status)
{
  ClearBackground();
  DrawHeader("Control mapping", "Press-to-bind");
  const int panel_width = std::min(900, m_width - 180);
  const int panel_height = 330;
  const int panel_x = (m_width - panel_width) / 2;
  const int panel_y = (m_height - panel_height) / 2 - 20;
  GlassPanel(panel_x, panel_y, panel_width, panel_height);
  Border(panel_x, panel_y, panel_width, panel_height, 3, m_selection);
  DrawTextCentered(m_font_small, m_width / 2, panel_y + 36,
                   std::string(m_localization.Translate("Control")) + " " +
                       std::to_string(position + 1) + " " +
                       std::string(m_localization.Translate("of")) + " " + std::to_string(count),
                   m_dim);
  DrawTextCentered(m_font_large, m_width / 2, panel_y + 94, m_localization.Translate(binding.label),
                   m_value);
  DrawTextCentered(m_font, m_width / 2, panel_y + 180,
                   m_localization.Translate(releasing ?
                                                "Release the current control" :
                                                "Press a button, trigger or stick direction"),
                   m_text);
  const std::string current_line =
      std::string(m_localization.Translate("Current: ")) + std::string(current);
  DrawTextCentered(m_font_small, m_width / 2, panel_y + 238,
                   status.empty() ? current_line : std::string(m_localization.Translate(status)),
                   status.empty() ? m_dim : m_highlight);
  DrawTextCentered(m_font_small, m_width / 2, m_height - 72,
                   m_localization.Translate("Hold Plus to clear       Hold Minus to cancel"),
                   m_dim);
  DrawTextCentered(m_font_small, m_width / 2, m_height - 38,
                   m_localization.Translate("Touch left to clear       Touch right to cancel"),
                   m_dim);
  SDL_RenderPresent(m_renderer);
}

std::optional<std::string> Launcher::CaptureControllerInput(const InputBinding& binding,
                                                            int position, int count,
                                                            std::string_view current)
{
  while (BeginFrame())
  {
    SDL_GameControllerUpdate();
    bool held = false;
    if (m_controller && SDL_GameControllerGetAttached(m_controller))
    {
      for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button)
        held |= SDL_GameControllerGetButton(m_controller,
                                            static_cast<SDL_GameControllerButton>(button)) != 0;
      for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; ++axis)
        held |= std::abs(static_cast<int>(SDL_GameControllerGetAxis(
                    m_controller, static_cast<SDL_GameControllerAxis>(axis)))) > 16000;
    }
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      if (FeedTouch(event, &touch_x, &touch_y) == TouchKind::Tap && touch_y >= m_height - 100)
        return std::nullopt;
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
        return std::nullopt;
    }
    if (!held)
      break;
    RenderControllerCapture(binding, position, count, true, current);
    WaitForNextFrame();
  }

  int held_special = -1;
  Uint32 held_since = 0;
  while (BeginFrame())
  {
    if (!m_controller || !SDL_GameControllerGetAttached(m_controller))
      return std::nullopt;
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::Tap && touch_y >= m_height - 100)
        return touch_x < m_width / 2 ? std::optional<std::string>{std::string{}} : std::nullopt;
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_START ||
            event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
        {
          held_special = event.cbutton.button;
          held_since = SDL_GetTicks();
        }
        else if (const auto expression = SwitchExpressionForButton(event.cbutton.button))
        {
          return expression;
        }
      }
      else if (event.type == SDL_CONTROLLERBUTTONUP && event.cbutton.button == held_special)
      {
        return SwitchExpressionForButton(static_cast<Uint8>(held_special));
      }
      else if (event.type == SDL_CONTROLLERAXISMOTION)
      {
        if (const auto expression = SwitchExpressionForAxis(event.caxis.axis, event.caxis.value))
          return expression;
      }
      else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
      {
        return std::nullopt;
      }
    }
    std::string status;
    if (held_special >= 0)
    {
      const Uint32 held_for = SDL_GetTicks() - held_since;
      if (held_for >= 800)
      {
        if (held_special == SDL_CONTROLLER_BUTTON_START)
          return std::string{};
        return std::nullopt;
      }
      status = held_special == SDL_CONTROLLER_BUTTON_START ? "Keep holding Plus to clear" :
                                                             "Keep holding Minus to cancel";
    }
    RenderControllerCapture(binding, position, count, false, current, status);
    WaitForNextFrame();
  }
  return std::nullopt;
}

void Launcher::ControllerMappingSettings(bool wii, int port, bool per_game, Game* game,
                                         bool extension_only, bool triforce,
                                         bool orientation_hotkeys)
{
  ControllerTarget shown_target = GetControllerTarget(wii, port, per_game, game, false);
  const std::string extension =
      wii ? ReadControllerValue(shown_target, "Extension", "Nunchuk") : std::string{};
  const std::vector<InputBinding> bindings =
      GetControllerBindings(wii, extension, extension_only, triforce, orientation_hotkeys);
  if (bindings.empty())
  {
    RenderMessage("Extension mapping",
                  std::array<std::string, 3>{
                      "This specialized extension is selected and will be emulated.",
                      "Its advanced controls can be supplied through a Dolphin profile.",
                      "Use Profiles to import that mapping, or select a common extension."},
                  true);
    return;
  }
  const std::string title = triforce ? "Triforce control mapping" :
                            orientation_hotkeys ? "Orientation hotkeys" :
                            extension_only      ? "Extension mapping" :
                                                  "Control mapping";
  const std::string context = triforce ? "Triforce Baseboard at port " + std::to_string(port + 1) :
                                         std::string(wii ? "Wii Remote " : "GameCube controller ") +
                                             std::to_string(port + 1);
  auto& saved = m_row_positions[title + '\n' + context];
  const int count = static_cast<int>(bindings.size());
  int selection = std::clamp(saved.first, 0, count - 1);
  int top = std::max(0, saved.second);
  constexpr int row_height = 46;
  constexpr int list_top = 118;
  const int visible = std::max(1, (m_height - list_top - 72) / row_height);
  const auto finish = [&] { saved = {selection, top}; };
  const auto assign_selected = [&] {
    if (!m_controller || !SDL_GameControllerGetAttached(m_controller))
    {
      RenderMessage("Control mapping", std::array<std::string, 1>{"No controller is connected."},
                    true);
      BeginScreenFx();
      return;
    }
    shown_target = GetControllerTarget(wii, port, per_game, game, false);
    const InputBinding& binding = bindings[selection];
    const std::string current = BindingDisplayName(
        ReadControllerValue(shown_target, binding.key, binding.default_expression));
    const auto expression = CaptureControllerInput(binding, selection, count, current);
    if (expression)
    {
      const ControllerTarget target = GetControllerTarget(wii, port, per_game, game, true);
      const bool profile_saved = WriteControllerValue(target, binding.key, *expression);
      bool game_profile_saved = true;
      if (profile_saved && per_game && game)
      {
        const std::string setting_key =
            std::string(wii ? "WiimoteProfile" : "PadProfile") + std::to_string(port + 1);
        game_profile_saved = SetGameSetting(*game, "Controls", setting_key,
                                            GeneratedControllerProfileName(game->key, wii, port));
      }
      if (profile_saved && game_profile_saved)
        Toast(expression->empty() ? "Control cleared" : "Control assigned", 650);
      else
        RenderMessage("Control mapping",
                      std::array<std::string, 1>{
                          per_game ? "The per-game controller configuration could not be saved." :
                                     "The controller configuration could not be saved."},
                      true);
    }
    BeginScreenFx();
  };
  const auto show_selected_info = [&] {
    shown_target = GetControllerTarget(wii, port, per_game, game, false);
    const InputBinding& binding = bindings[selection];
    const std::string current = BindingDisplayName(
        ReadControllerValue(shown_target, binding.key, binding.default_expression));
    std::string kind = "Controller mapping";
    std::string description =
        "Assigns this emulated Dolphin control to a Switch controller input. Press A, then press "
        "or move the desired physical control; holding Plus clears the binding and holding Minus "
        "cancels.";
    if (binding.key == "Triforce/Test")
    {
      kind = "Triforce cabinet input";
      description = "Opens the arcade cabinet test menu. Dolphin requires SegaBoot for this menu; "
                    "pressing Test without it is rejected safely.";
    }
    else if (binding.key == "Triforce/Service")
    {
      kind = "Triforce cabinet input";
      description = "Sends the cabinet Service switch used to enter operator menus and service "
                    "the selected Triforce game.";
    }
    else if (binding.key == "Triforce/Coin")
    {
      kind = "Triforce cabinet input";
      description = "Sends a coin-slot input to the selected Triforce player so the game can add "
                    "a credit.";
    }
    else if (binding.key == "Hotkeys/Sideways Toggle")
    {
      kind = "Wii Remote orientation";
      description =
          "Reverses the Sideways Wii Remote setting each time the mapped button is pressed. Press "
          "it again to restore the previous orientation.";
    }
    else if (binding.key == "Hotkeys/Upright Toggle")
    {
      kind = "Wii Remote orientation";
      description =
          "Reverses the Upright Wii Remote setting each time the mapped button is pressed. Press "
          "it again to restore the previous orientation.";
    }
    else if (binding.key == "Hotkeys/Sideways Hold")
    {
      kind = "Wii Remote orientation";
      description =
          "Reverses the Sideways Wii Remote setting only while the mapped button is held. Release "
          "it to restore the previous orientation.";
    }
    else if (binding.key == "Hotkeys/Upright Hold")
    {
      kind = "Wii Remote orientation";
      description =
          "Reverses the Upright Wii Remote setting only while the mapped button is held. Release "
          "it to restore the previous orientation.";
    }
    ShowInfoCard(title, binding.label, kind, description, current,
                 per_game ? "Per-game setting" : "Global setting");
    BeginScreenFx();
  };

  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, count, visible))
        continue;
      if (touch == TouchKind::Tap)
      {
        if (touch_y < (m_width >= 1600 ? 112 : 80) || touch_y >= m_height - 40)
        {
          finish();
          return;
        }
        const int column_width = std::min(980, m_width - 180);
        const int column_x = (m_width - column_width) / 2;
        for (int row = 0; row < visible && top + row < count; ++row)
        {
          const int y = list_top + row * row_height;
          if (touch_x >= column_x && touch_x < column_x + column_width && touch_y >= y &&
              touch_y < y + row_height)
          {
            selection = top + row;
            assign_selected();
            break;
          }
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == BUTTON_CONFIRM)
          assign_selected();
        else if (event.cbutton.button == BUTTON_SETTINGS)
          show_selected_info();
        else if (event.cbutton.button == BUTTON_CANCEL)
        {
          finish();
          return;
        }
      }
      else if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_RETURN)
          assign_selected();
        else if (event.key.keysym.sym == SDLK_x)
          show_selected_info();
        else if (event.key.keysym.sym == SDLK_ESCAPE)
        {
          finish();
          return;
        }
      }
      if (selection < top)
        top = selection;
      if (selection >= top + visible)
        top = selection - visible + 1;
    }

    shown_target = GetControllerTarget(wii, port, per_game, game, false);
    ClearBackground();
    DrawHeader(title, context);
    const int column_width = std::min(980, m_width - 180);
    const int column_x = (m_width - column_width) / 2;
    const int label_x = column_x + 40;
    const int value_x = column_x + column_width - 40;
    GlassPanel(column_x - 12, list_top - 10, column_width + 24, visible * row_height + 18);
    const float target = static_cast<float>(list_top + (selection - top) * row_height + 1);
    m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                        target :
                        m_highlight_y + (target - m_highlight_y) * 0.30f;
    FillRect(column_x, static_cast<int>(m_highlight_y), column_width, row_height - 2, m_focus);
    FillRect(column_x, static_cast<int>(m_highlight_y), 5, row_height - 2, m_selection);
    for (int row = 0; row < visible && top + row < count; ++row)
    {
      const int index = top + row;
      const int y = list_top + row * row_height + (row_height - TTF_FontHeight(m_font)) / 2;
      const bool current = index == selection;
      DrawText(m_font, label_x, y, m_localization.Translate(bindings[index].label),
               current ? m_value : m_text);
      const std::string expression = BindingDisplayName(ReadControllerValue(
          shown_target, bindings[index].key, bindings[index].default_expression));
      DrawTextRight(m_font, value_x, y, Ellipsize(m_font, expression, column_width / 2),
                    current ? m_value : m_dim);
    }
    if (count > visible)
    {
      const int track_height = visible * row_height;
      const int track_x = column_x + column_width + 16;
      const int track_y = list_top - 2;
      FillRect(track_x, track_y, 4, track_height, SDL_Color{40, 44, 54, 255});
      const int thumb_height = track_height * visible / count;
      const int denominator = std::max(1, count - visible);
      FillRect(track_x, track_y + (track_height - thumb_height) * top / denominator, 4,
               thumb_height, m_selection);
    }
    DrawSettingsFooter("A  Assign       X  Info       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  finish();
}

void Launcher::ControllerProfileSettings(bool wii, int port, bool per_game, Game* game)
{
  const std::string setting_key =
      std::string(wii ? "WiimoteProfile" : "PadProfile") + std::to_string(port + 1);
  RunRows(
      "Controller profiles",
      std::string(wii ? "Wii Remote " : "GameCube controller ") + std::to_string(port + 1),
      [&] {
        std::string active(m_localization.Translate("Current mapping"));
        if (per_game && game)
        {
          if (const auto configured = GetGameSetting(*game, "Controls", setting_key))
            active = *configured;
          else
            active = m_localization.Translate("Global mapping");
        }
        return std::vector<Row>{
            {"Active profile", active, false, false, true, true, false},
            {"Load profile", per_game ? "For this game" : "Into this port", true, false, false},
            {"Save as profile", "Reusable Dolphin profile", true, false, false},
            {per_game ? "Use global mapping" : "Reset Switch defaults",
             per_game ? "Remove game override" : "Player defaults", true, false, false}};
      },
      [&](int index, int) {
        if (index == 1)
        {
          std::vector<std::string> profiles = ListControllerProfiles(wii);
          if (profiles.empty())
          {
            Toast("No user profiles found", 1000);
            return false;
          }
          std::vector<std::string> choices{std::string(m_localization.Translate("Cancel"))};
          choices.insert(choices.end(), profiles.begin(), profiles.end());
          const int choice = Dropdown("Load controller profile", choices, 0, true, false);
          if (choice <= 0 || choice > static_cast<int>(profiles.size()))
            return false;
          const std::string& selected_profile = profiles[choice - 1];
          const ControllerTarget previous = GetControllerTarget(wii, port, per_game, game, false);
          const std::string assigned_device =
              ReadControllerValue(previous, "Device", SwitchControllerDevice(port));
          if (per_game && game)
          {
            SetGameSetting(*game, "Controls", setting_key, selected_profile);
            const ControllerTarget destination = GetControllerTarget(wii, port, true, game, true);
            WriteControllerValue(destination, "Device", assigned_device);
          }
          else
          {
            const ControllerTarget source{
                ControllerProfileDirectory(wii) + selected_profile + ".ini", "Profile", false};
            const ControllerTarget destination =
                GetControllerTarget(wii, port, false, nullptr, true);
            CopyControllerTarget(source, destination);
            WriteControllerValue(destination, "Device", assigned_device);
          }
          Toast("Profile loaded", 750);
        }
        else if (index == 2)
        {
          std::string name;
          if (!PromptText("Profile name", {}, &name, false, false,
                          "Save the current Dolphin controller mapping"))
            return false;
          name = SafeProfileName(name);
          const std::string directory = ControllerProfileDirectory(wii);
          File::CreateDirs(directory);
          const ControllerTarget source = GetControllerTarget(wii, port, per_game, game, false);
          const ControllerTarget destination{directory + name + ".ini", "Profile", false};
          CopyControllerTarget(source, destination);
          if (per_game && game)
            SetGameSetting(*game, "Controls", setting_key, name);
          Toast("Profile saved", 750);
        }
        else if (index == 3)
        {
          if (per_game && game)
          {
            SetGameSetting(*game, "Controls", setting_key, std::nullopt);
            Toast("Using global controller mapping", 750);
          }
          else if (Confirm("Reset controller mapping?",
                           std::array<std::string, 2>{
                               "All bindings for this player will return to Switch defaults.",
                               "Other players are not changed."},
                           true))
          {
            ResetControllerTarget(GetControllerTarget(wii, port, false, nullptr, true));
            Toast("Switch defaults restored", 750);
          }
        }
        return false;
      });
}

void Launcher::WiiMotionSettings(int port, bool per_game, Game* game)
{
  const auto read = [&](std::string_view key, std::string_view fallback) {
    return ReadControllerValue(GetControllerTarget(true, port, per_game, game, false), key,
                               fallback);
  };
  const auto read_int = [&](std::string_view key, int fallback) {
    const std::string value = read(key, std::to_string(fallback));
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return end != value.c_str() ? static_cast<int>(parsed) : fallback;
  };
  const auto read_bool = [&](std::string_view key, bool fallback) {
    const std::string value = Lower(Trim(read(key, fallback ? "True" : "False")));
    if (value == "false" || value == "0" || value == "no")
      return false;
    // Some Boolean settings contain input expressions.
    return !value.empty();
  };
  RunRows(
      "Wii motion & pointer", "Wii Remote " + std::to_string(port + 1),
      [&] {
        const int total_yaw = std::clamp(read_int("IMUIR/Total Yaw", 70), 10, 180);
        const int sensitivity = std::clamp(7000 / total_yaw, 40, 200);
        return std::vector<Row>{
            {"Sideways Wii Remote", read_bool("Options/Sideways Wiimote", false) ? "On" : "Off"},
            {"Upright Wii Remote", read_bool("Options/Upright Wiimote", false) ? "On" : "Off"},
            {"Attach MotionPlus", read_bool("Extension/Attach MotionPlus", true) ? "On" : "Off"},
            {"Gyro pointer", read_bool("IMUIR/Enabled", true) ? "On" : "Off"},
            {"Pointer sensitivity", std::to_string(sensitivity) + "%"},
            {"Gyro dead zone", std::to_string(read_int("IMUGyroscope/Dead Zone", 2)) + " deg/s"},
            {"Auto-calibration",
             std::to_string(read_int("IMUGyroscope/Calibration Period", 3)) + " s"},
            {"Accelerometer influence",
             std::to_string(read_int("IMUIR/Accelerometer Influence", 1)) + "%"},
            {"Right-stick pointer dead zone", std::to_string(read_int("IR/Dead Zone", 0)) + "%"},
            {"IR relative input", read_bool("IR/Relative Input", true) ? "On" : "Off"},
            {"IR total yaw", std::to_string(read_int("IR/Total Yaw", 25)) + " deg"},
            {"IR total pitch", std::to_string(read_int("IR/Total Pitch", 20)) + " deg"},
            {"Pointer recenter", BindingDisplayName(read("IMUIR/Recenter", "`R3`")), true, false,
             false, true, false},
            {"Calibration guide", "Keep controller still", true, false, false},
        };
      },
      [&](int index, int delta) {
        const ControllerTarget target = GetControllerTarget(true, port, per_game, game, true);
        if (index <= 3)
        {
          static constexpr std::array<std::string_view, 4> keys = {
              "Options/Sideways Wiimote", "Options/Upright Wiimote", "Extension/Attach MotionPlus",
              "IMUIR/Enabled"};
          const bool current = read_bool(keys[index], index == 2 || index == 3);
          WriteControllerValue(target, keys[index], current ? "False" : "True");
        }
        else if (index == 4)
        {
          const int yaw = std::clamp(read_int("IMUIR/Total Yaw", 70), 10, 180);
          int sensitivity = std::clamp(7000 / yaw, 40, 200);
          sensitivity = std::clamp(sensitivity + (delta < 0 ? -5 : 5), 40, 200);
          WriteControllerValue(target, "IMUIR/Total Yaw",
                               std::to_string(std::clamp(7000 / sensitivity, 10, 180)));
        }
        else if (index >= 5 && index <= 8)
        {
          static constexpr std::array<std::string_view, 4> keys = {
              "IMUGyroscope/Dead Zone", "IMUGyroscope/Calibration Period",
              "IMUIR/Accelerometer Influence", "IR/Dead Zone"};
          static constexpr std::array<int, 4> defaults = {2, 3, 1, 0};
          static constexpr std::array<int, 4> maxima = {30, 10, 10, 40};
          const int item = index - 5;
          int value = read_int(keys[item], defaults[item]);
          value = std::clamp(value + (delta < 0 ? -1 : 1), 0, maxima[item]);
          WriteControllerValue(target, keys[item], std::to_string(value));
        }
        else if (index == 9)
        {
          const bool current = read_bool("IR/Relative Input", true);
          WriteControllerValue(target, "IR/Relative Input", current ? "False" : "True");
        }
        else if (index == 10 || index == 11)
        {
          const std::string_view key = index == 10 ? "IR/Total Yaw" : "IR/Total Pitch";
          const int fallback = index == 10 ? 25 : 20;
          int value = std::clamp(read_int(key, fallback), 5, 360);
          value = std::clamp(value + (delta < 0 ? -5 : 5), 5, 360);
          WriteControllerValue(target, key, std::to_string(value));
        }
        else if (index == 12)
        {
          const InputBinding recenter{"Pointer recenter", "IMUIR/Recenter", "`R3`"};
          if (delta < 0)
            WriteControllerValue(target, recenter.key, std::string{});
          else if (delta > 0)
            WriteControllerValue(target, recenter.key, recenter.default_expression);
          else if (const auto expression = CaptureControllerInput(
                       recenter, 0, 1,
                       BindingDisplayName(read(recenter.key, recenter.default_expression))))
            WriteControllerValue(target, recenter.key, *expression);
          BeginScreenFx();
        }
        else
        {
          RenderMessage("Gyroscope calibration",
                        std::array<std::string, 4>{
                            "Place the Joy-Con or controller on a stable surface.",
                            "Dolphin learns gyro bias while input remains inside the dead zone.",
                            "Auto-calibration controls how many stable seconds are required.",
                            "Use Pointer recenter during play to reset yaw and pitch."},
                        true);
        }
        return false;
      },
      false,
      [&](int index) {
        static constexpr std::array<std::string_view, 13> keys = {"Options/Sideways Wiimote",
                                                                  "Options/Upright Wiimote",
                                                                  "Extension/Attach MotionPlus",
                                                                  "IMUIR/Enabled",
                                                                  "IMUIR/Total Yaw",
                                                                  "IMUGyroscope/Dead Zone",
                                                                  "IMUGyroscope/Calibration Period",
                                                                  "IMUIR/Accelerometer Influence",
                                                                  "IR/Dead Zone",
                                                                  "IR/Relative Input",
                                                                  "IR/Total Yaw",
                                                                  "IR/Total Pitch",
                                                                  "IMUIR/Recenter"};
        static constexpr std::array<std::string_view, 13> defaults = {
            "False", "False", "True", "True", "70", "2", "3", "1", "0", "True", "25", "20", "`R3`"};
        if (index < 0 || index >= static_cast<int>(keys.size()))
          return false;
        const ControllerTarget target = GetControllerTarget(true, port, per_game, game, true);
        return WriteControllerValue(target, keys[index], std::string(defaults[index]));
      });
}

void Launcher::ControllerPortSettings(bool wii, int port, bool per_game, Game* game)
{
  const auto read = [&](std::string_view key, std::string_view fallback) {
    return ReadControllerValue(GetControllerTarget(wii, port, per_game, game, false), key,
                               fallback);
  };
  const auto read_int = [&](std::string_view key, int fallback) {
    const std::string value = read(key, std::to_string(fallback));
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return end != value.c_str() ? static_cast<int>(parsed) : fallback;
  };
  const auto player_number = [&] {
    const std::string device = read("Device", SwitchControllerDevice(port));
    if (device.starts_with("Switch/") && device.size() > 7 && std::isdigit(device[7]))
      return std::clamp(device[7] - '0', 0, 3);
    return port;
  };

  if (!wii)
  {
    static constexpr std::array<SerialInterface::SIDevices, 5> types = {
        SerialInterface::SIDEVICE_GC_CONTROLLER, SerialInterface::SIDEVICE_GC_STEERING,
        SerialInterface::SIDEVICE_DANCEMAT, SerialInterface::SIDEVICE_GC_TARUKONGA,
        SerialInterface::SIDEVICE_AM_BASEBOARD};
    static constexpr std::array<std::string_view, 5> type_names = {
        "Standard Controller", "Steering Wheel", "Dance Mat", "DK Bongos", "Triforce Baseboard"};
    const auto game_type_override = [&]() -> std::optional<SerialInterface::SIDevices> {
      if (!per_game || !game)
        return std::nullopt;
      const auto value = GetGameSetting(*game, "Controls", "PadType" + std::to_string(port));
      if (!value)
        return std::nullopt;
      return static_cast<SerialInterface::SIDevices>(std::atoi(value->c_str()));
    };
    const auto current_type = [&] {
      if (const auto local = game_type_override())
        return *local;
      return Config::Get(Config::GetInfoForSIDevice(port));
    };
    const auto effective_type = [&] {
      if (per_game && game && port == 0 && game->platform == "Triforce" && !game_type_override())
      {
        return SerialInterface::SIDEVICE_AM_BASEBOARD;
      }
      return current_type();
    };
    const auto type_index = [&] {
      const auto iterator = std::ranges::find(types, effective_type());
      return iterator == types.end() ? 0 : static_cast<int>(iterator - types.begin());
    };
    RunRows(
        "GameCube controller " + std::to_string(port + 1), game ? game->title : std::string{},
        [&] {
          const int rumble = read("Rumble/Motor", "`Motor`").empty() ?
                                 0 :
                                 std::clamp(read_int("Rumble/Motor/Range", 100), 0, 100);
          return std::vector<Row>{
              {"Emulated device", std::string(type_names[type_index()])},
              {"Switch player", "Controller " + std::to_string(player_number() + 1)},
              {"Joy-Con layout", std::string(ciface::Switch::GetJoyConLayoutName(
                                     ciface::Switch::GetJoyConLayout(player_number())))},
              {"Interactive mapping",
               effective_type() == SerialInterface::SIDEVICE_AM_BASEBOARD ?
                   "Arcade buttons, sticks and triggers" :
                   "Buttons, sticks and triggers",
               true, false, false},
              {"Rumble strength", std::to_string(rumble) + "%"},
              {"Control Stick dead zone",
               std::to_string(read_int("Main Stick/Dead Zone", 0)) + "%"},
              {"C-Stick dead zone", std::to_string(read_int("C-Stick/Dead Zone", 0)) + "%"},
              {"Trigger dead zone", std::to_string(read_int("Triggers/Dead Zone", 0)) + "%"},
              {"Profiles",
               per_game && GetControllerTarget(false, port, true, game, false).inherited ?
                   "Global mapping" :
                   "Load / save",
               true, false, false},
              {per_game ? "Reset game mapping" : "Reset Switch defaults",
               per_game ? "Use global mapping" : "This player only", true, true, false},
          };
        },
        [&](int index, int delta) {
          if (index == 0)
          {
            int selected = type_index();
            if (delta == 0)
            {
              std::vector<std::string> names;
              for (const std::string_view name : type_names)
                names.emplace_back(name);
              selected = Dropdown("Emulated GameCube device", names, selected);
            }
            else
              selected = (selected + (delta < 0 ? -1 : 1) + types.size()) % types.size();
            const std::string key = "PadType" + std::to_string(port);
            if (per_game && game)
              SetGameSetting(*game, "Controls", key,
                             std::to_string(static_cast<int>(types[selected])));
            else
            {
              Config::SetBase(Config::GetInfoForSIDevice(port), types[selected]);
              MarkConfigDirty();
            }
          }
          else if (index == 1)
          {
            std::vector<std::string> choices{"Switch controller 1", "Switch controller 2",
                                             "Switch controller 3", "Switch controller 4"};
            int selected = player_number();
            selected = delta == 0 ? Dropdown("Physical controller", choices, selected) :
                                    (selected + (delta < 0 ? -1 : 1) + 4) % 4;
            WriteControllerValue(GetControllerTarget(false, port, per_game, game, true), "Device",
                                 SwitchControllerDevice(selected));
          }
          else if (index == 2)
          {
            static constexpr std::array<ciface::Switch::JoyConLayout, 4> layouts = {
                ciface::Switch::JoyConLayout::Auto, ciface::Switch::JoyConLayout::Dual,
                ciface::Switch::JoyConLayout::Left, ciface::Switch::JoyConLayout::Right};
            std::vector<std::string> choices;
            for (const auto layout : layouts)
              choices.emplace_back(ciface::Switch::GetJoyConLayoutName(layout));
            int selected = static_cast<int>(ciface::Switch::GetJoyConLayout(player_number()));
            selected = delta == 0 ?
                           Dropdown("Physical Joy-Con layout", choices, selected) :
                           (selected + (delta < 0 ? -1 : 1) + layouts.size()) % layouts.size();
            ciface::Switch::SetJoyConLayout(player_number(), layouts[selected]);
            MarkConfigDirty();
          }
          else if (index == 3)
            ControllerMappingSettings(false, port, per_game, game, false,
                                      effective_type() == SerialInterface::SIDEVICE_AM_BASEBOARD);
          else if (index >= 4 && index <= 7)
          {
            static constexpr std::array<std::string_view, 4> keys = {
                "Rumble/Motor/Range", "Main Stick/Dead Zone", "C-Stick/Dead Zone",
                "Triggers/Dead Zone"};
            static constexpr std::array<int, 4> maxima = {100, 40, 40, 25};
            const int item = index - 4;
            const ControllerTarget target = GetControllerTarget(false, port, per_game, game, true);
            int value = item == 0 && read("Rumble/Motor", "`Motor`").empty() ?
                            0 :
                            read_int(keys[item], item == 0 ? 100 : 0);
            value = std::clamp(value + (delta < 0 ? -5 : 5), 0, maxima[item]);
            WriteControllerValue(target, keys[item], std::to_string(value));
            if (item == 0)
              WriteControllerValue(target, "Rumble/Motor", value == 0 ? "" : "`Motor`");
          }
          else if (index == 8)
            ControllerProfileSettings(false, port, per_game, game);
          else if (index == 9 &&
                   Confirm("Reset controller mapping?",
                           std::array<std::string, 2>{
                               per_game ? "This game will use the global mapping." :
                                          "This player will return to Switch defaults.",
                               "Other controller ports are not changed."},
                           true))
          {
            if (per_game && game)
              SetGameSetting(*game, "Controls", "PadProfile" + std::to_string(port + 1),
                             std::nullopt);
            else
              ResetControllerTarget(GetControllerTarget(false, port, false, nullptr, true));
          }
          return false;
        },
        false,
        [&](int index) {
          if (index == 0)
          {
            if (per_game)
              SetGameSetting(*game, "Controls", "PadType" + std::to_string(port), std::nullopt);
            else
              ResetConfigSetting(Config::GetInfoForSIDevice(port));
            return true;
          }
          if (index == 2)
          {
            ciface::Switch::SetJoyConLayout(player_number(), ciface::Switch::JoyConLayout::Auto);
            MarkConfigDirty();
            return true;
          }
          const ControllerTarget target = GetControllerTarget(false, port, per_game, game, true);
          if (index == 1)
            return WriteControllerValue(target, "Device", SwitchControllerDevice(port));
          if (index >= 4 && index <= 7)
          {
            static constexpr std::array<std::string_view, 4> keys = {
                "Rumble/Motor/Range", "Main Stick/Dead Zone", "C-Stick/Dead Zone",
                "Triggers/Dead Zone"};
            static constexpr std::array<std::string_view, 4> defaults = {"100", "0", "0", "0"};
            const int item = index - 4;
            const bool saved =
                WriteControllerValue(target, keys[item], std::string(defaults[item]));
            if (item == 0)
              return WriteControllerValue(target, "Rumble/Motor", "`Motor`") && saved;
            return saved;
          }
          return false;
        });
    return;
  }

  static constexpr std::array<std::string_view, 10> extension_values = {
      "None",      "Nunchuk", "Classic",  "Guitar",  "Drums",
      "Turntable", "uDraw",   "Drawsome", "TaTaCon", "Shinkansen"};
  static constexpr std::array<std::string_view, 10> extension_names = {"None",
                                                                       "Nunchuk",
                                                                       "Classic Controller",
                                                                       "Guitar",
                                                                       "Drum Kit",
                                                                       "DJ Turntable",
                                                                       "uDraw GameTablet",
                                                                       "Drawsome Tablet",
                                                                       "Taiko Drum",
                                                                       "Shinkansen Controller"};
  const auto extension_index = [&] {
    const std::string extension = read("Extension", "Nunchuk");
    const auto iterator = std::ranges::find(extension_values, std::string_view(extension));
    return iterator == extension_values.end() ?
               0 :
               static_cast<int>(iterator - extension_values.begin());
  };
  RunRows(
      "Wii Remote " + std::to_string(port + 1), game ? game->title : std::string{},
      [&] {
        const int ext = extension_index();
        const int rumble = read("Rumble/Motor", "`Motor`").empty() ?
                               0 :
                               std::clamp(read_int("Rumble/Motor/Range", 100), 0, 100);
        std::string stick_deadzone = "Not used";
        if (extension_values[ext] == "Nunchuk")
          stick_deadzone = std::to_string(read_int("Nunchuk/Stick/Dead Zone", 0)) + "%";
        else if (extension_values[ext] == "Classic")
          stick_deadzone = std::to_string(read_int("Classic/Left Stick/Dead Zone", 0)) + "%";
        return std::vector<Row>{
            {"Switch player", "Controller " + std::to_string(player_number() + 1)},
            {"Joy-Con layout", std::string(ciface::Switch::GetJoyConLayoutName(
                                   ciface::Switch::GetJoyConLayout(player_number())))},
            {"Extension", std::string(extension_names[ext])},
            {"Wii Remote mapping", "Buttons, pointer and motion sensors", true, false, false},
            {"Extension mapping", extension_values[ext] == "None" ? "No extension" : "Configure",
             extension_values[ext] != "None", false, false},
            {"Motion & pointer", "Gyro, IR, orientation and calibration", true, false, false},
            {"Orientation hotkeys", "Hold and toggle", true, false, false},
            {"Rumble strength", std::to_string(rumble) + "%"},
            {"Extension stick dead zone", stick_deadzone,
             extension_values[ext] == "Nunchuk" || extension_values[ext] == "Classic"},
            {"Profiles",
             per_game && GetControllerTarget(true, port, true, game, false).inherited ?
                 "Global mapping" :
                 "Load / save",
             true, false, false},
            {per_game ? "Reset game mapping" : "Reset Switch defaults",
             per_game ? "Use global mapping" : "This player only", true, true, false},
        };
      },
      [&](int index, int delta) {
        if (index == 0)
        {
          std::vector<std::string> choices{"Switch controller 1", "Switch controller 2",
                                           "Switch controller 3", "Switch controller 4"};
          int selected = player_number();
          selected = delta == 0 ? Dropdown("Physical controller", choices, selected) :
                                  (selected + (delta < 0 ? -1 : 1) + 4) % 4;
          WriteControllerValue(GetControllerTarget(true, port, per_game, game, true), "Device",
                               SwitchControllerDevice(selected));
        }
        else if (index == 1)
        {
          static constexpr std::array<ciface::Switch::JoyConLayout, 4> layouts = {
              ciface::Switch::JoyConLayout::Auto, ciface::Switch::JoyConLayout::Dual,
              ciface::Switch::JoyConLayout::Left, ciface::Switch::JoyConLayout::Right};
          std::vector<std::string> choices;
          for (const auto layout : layouts)
            choices.emplace_back(ciface::Switch::GetJoyConLayoutName(layout));
          int selected = static_cast<int>(ciface::Switch::GetJoyConLayout(player_number()));
          selected = delta == 0 ?
                         Dropdown("Physical Joy-Con layout", choices, selected) :
                         (selected + (delta < 0 ? -1 : 1) + layouts.size()) % layouts.size();
          ciface::Switch::SetJoyConLayout(player_number(), layouts[selected]);
          MarkConfigDirty();
        }
        else if (index == 2)
        {
          int selected = extension_index();
          if (delta == 0)
          {
            std::vector<std::string> names;
            for (const std::string_view name : extension_names)
              names.emplace_back(name);
            selected = Dropdown("Wii Remote extension", names, selected);
          }
          else
            selected = (selected + (delta < 0 ? -1 : 1) + extension_values.size()) %
                       extension_values.size();
          WriteControllerValue(GetControllerTarget(true, port, per_game, game, true), "Extension",
                               std::string(extension_values[selected]));
        }
        else if (index == 3)
          ControllerMappingSettings(true, port, per_game, game, false);
        else if (index == 4)
          ControllerMappingSettings(true, port, per_game, game, true);
        else if (index == 5)
          WiiMotionSettings(port, per_game, game);
        else if (index == 6)
          ControllerMappingSettings(true, port, per_game, game, false, false, true);
        else if (index == 7)
        {
          const ControllerTarget target = GetControllerTarget(true, port, per_game, game, true);
          int value =
              read("Rumble/Motor", "`Motor`").empty() ? 0 : read_int("Rumble/Motor/Range", 100);
          value = std::clamp(value + (delta < 0 ? -5 : 5), 0, 100);
          WriteControllerValue(target, "Rumble/Motor/Range", std::to_string(value));
          WriteControllerValue(target, "Rumble/Motor", value == 0 ? "" : "`Motor`");
        }
        else if (index == 8)
        {
          const int ext = extension_index();
          const std::string key = extension_values[ext] == "Classic" ?
                                      "Classic/Left Stick/Dead Zone" :
                                      "Nunchuk/Stick/Dead Zone";
          int value = std::clamp(read_int(key, 0) + (delta < 0 ? -1 : 1), 0, 40);
          WriteControllerValue(GetControllerTarget(true, port, per_game, game, true), key,
                               std::to_string(value));
        }
        else if (index == 9)
          ControllerProfileSettings(true, port, per_game, game);
        else if (index == 10 && Confirm("Reset Wii Remote mapping?",
                                       std::array<std::string, 2>{
                                           per_game ? "This game will use the global mapping." :
                                                      "This remote will return to Switch defaults.",
                                           "Other Wii Remotes are not changed."},
                                       true))
        {
          if (per_game && game)
            SetGameSetting(*game, "Controls", "WiimoteProfile" + std::to_string(port + 1),
                           std::nullopt);
          else
            ResetControllerTarget(GetControllerTarget(true, port, false, nullptr, true));
        }
        return false;
      },
      false,
      [&](int index) {
        if (index == 1)
        {
          ciface::Switch::SetJoyConLayout(player_number(), ciface::Switch::JoyConLayout::Auto);
          MarkConfigDirty();
          return true;
        }
        const ControllerTarget target = GetControllerTarget(true, port, per_game, game, true);
        if (index == 0)
          return WriteControllerValue(target, "Device", SwitchControllerDevice(port));
        if (index == 2)
          return WriteControllerValue(target, "Extension", "Nunchuk");
        if (index == 7)
        {
          const bool range_saved = WriteControllerValue(target, "Rumble/Motor/Range", "100");
          return WriteControllerValue(target, "Rumble/Motor", "`Motor`") && range_saved;
        }
        if (index == 8)
        {
          const int ext = extension_index();
          const std::string_view key = extension_values[ext] == "Classic" ?
                                           "Classic/Left Stick/Dead Zone" :
                                           "Nunchuk/Stick/Dead Zone";
          return WriteControllerValue(target, key, "0");
        }
        return false;
      });
}

void Launcher::ControllerSettings(bool per_game, Game* game)
{
  const auto get = [&](std::string_view key) {
    return per_game && game ? GetGameSetting(*game, "Controls", key) : std::nullopt;
  };
  const auto gc_type_name = [](SerialInterface::SIDevices type) -> std::string_view {
    switch (type)
    {
    case SerialInterface::SIDEVICE_GC_CONTROLLER:
      return "Standard";
    case SerialInterface::SIDEVICE_GC_STEERING:
      return "Steering Wheel";
    case SerialInterface::SIDEVICE_DANCEMAT:
      return "Dance Mat";
    case SerialInterface::SIDEVICE_GC_TARUKONGA:
      return "DK Bongos";
    case SerialInterface::SIDEVICE_AM_BASEBOARD:
      return "Triforce Baseboard";
    default:
      return "Disabled";
    }
  };
  RunRows(
      per_game ? "Game controllers" : "Controller / Input", game ? game->title : std::string{},
      [&] {
        std::vector<Row> rows;
        for (int index = 0; index < 4; ++index)
        {
          const auto local = get("PadType" + std::to_string(index));
          const auto global = Config::Get(Config::GetInfoForSIDevice(index));
          auto device =
              local ? static_cast<SerialInterface::SIDevices>(std::atoi(local->c_str())) : global;
          if (per_game && game && index == 0 && game->platform == "Triforce" && !local)
            device = SerialInterface::SIDEVICE_AM_BASEBOARD;
          const ControllerTarget target = GetControllerTarget(false, index, per_game, game, false);
          const std::string physical =
              ReadControllerValue(target, "Device", SwitchControllerDevice(index));
          int player = index;
          if (physical.starts_with("Switch/") && physical.size() > 7 && std::isdigit(physical[7]))
            player = std::clamp(physical[7] - '0', 0, 3);
          std::string device_label =
              std::string(gc_type_name(device)) +
              (device == SerialInterface::SIDEVICE_NONE ? "" : " · P" + std::to_string(player + 1));
          if (per_game && !local)
            device_label = GlobalValueLabel(device_label);
          rows.push_back({"GameCube controller " + std::to_string(index + 1),
                          std::move(device_label), true, false, true, true, false});
        }
        for (int index = 0; index < 4; ++index)
        {
          const auto local = get("WiimoteSource" + std::to_string(index));
          const auto global = Config::Get(Config::GetInfoForWiimoteSource(index));
          const auto source =
              local ? static_cast<WiimoteSource>(std::atoi(local->c_str())) : global;
          const ControllerTarget target = GetControllerTarget(true, index, per_game, game, false);
          const std::string physical =
              ReadControllerValue(target, "Device", SwitchControllerDevice(index));
          int player = index;
          if (physical.starts_with("Switch/") && physical.size() > 7 && std::isdigit(physical[7]))
            player = std::clamp(physical[7] - '0', 0, 3);
          const std::string extension = ReadControllerValue(target, "Extension", "Nunchuk");
          std::string source_label = source == WiimoteSource::None ?
                                         std::string("Disabled") :
                                         extension + " · P" + std::to_string(player + 1);
          if (per_game && !local)
            source_label = GlobalValueLabel(source_label);
          rows.push_back({"Wii Remote " + std::to_string(index + 1), std::move(source_label), true,
                          false, true, true, false});
        }
        const bool wii_game = !per_game || (game && game->platform.starts_with("Wii"));
        rows.push_back(
            {"Touchscreen Wii pointer",
             wii_game ?
                 (per_game ?
                      PerGameBoolLabel(*game, "Core", "SwitchTouchscreenPointer",
                                       Config::Get(Config::MAIN_SWITCH_TOUCHSCREEN_POINTER)) :
                      (Config::Get(Config::MAIN_SWITCH_TOUCHSCREEN_POINTER) ? "On" : "Off")) :
                 "Wii games only",
             wii_game});
        return rows;
      },
      [&](int index, int delta) {
        if (index < 4)
        {
          const std::string key = "PadType" + std::to_string(index);
          const auto local = get(key);
          const auto global = Config::Get(Config::GetInfoForSIDevice(index));
          if (delta != 0)
          {
            if (per_game && delta < 0 && local)
              SetGameSetting(*game, "Controls", key, std::nullopt);
            else
            {
              const auto current =
                  local ? static_cast<SerialInterface::SIDevices>(std::atoi(local->c_str())) :
                          global;
              const auto next = current == SerialInterface::SIDEVICE_NONE ?
                                    SerialInterface::SIDEVICE_GC_CONTROLLER :
                                    SerialInterface::SIDEVICE_NONE;
              if (per_game)
                SetGameSetting(*game, "Controls", key, std::to_string(static_cast<int>(next)));
              else
              {
                Config::SetBase(Config::GetInfoForSIDevice(index), next);
                MarkConfigDirty();
              }
            }
          }
          else
          {
            const auto current =
                local ? static_cast<SerialInterface::SIDevices>(std::atoi(local->c_str())) : global;
            if (current == SerialInterface::SIDEVICE_NONE)
            {
              if (per_game)
                SetGameSetting(
                    *game, "Controls", key,
                    std::to_string(static_cast<int>(SerialInterface::SIDEVICE_GC_CONTROLLER)));
              else
              {
                Config::SetBase(Config::GetInfoForSIDevice(index),
                                SerialInterface::SIDEVICE_GC_CONTROLLER);
                MarkConfigDirty();
              }
            }
            ControllerPortSettings(false, index, per_game, game);
          }
        }
        else if (index < 8)
        {
          const int wiimote = index - 4;
          const std::string key = "WiimoteSource" + std::to_string(wiimote);
          const auto local = get(key);
          const auto global = Config::Get(Config::GetInfoForWiimoteSource(wiimote));
          if (delta != 0)
          {
            if (per_game && delta < 0 && local)
              SetGameSetting(*game, "Controls", key, std::nullopt);
            else
            {
              const auto current =
                  local ? static_cast<WiimoteSource>(std::atoi(local->c_str())) : global;
              const auto next =
                  current == WiimoteSource::None ? WiimoteSource::Emulated : WiimoteSource::None;
              if (per_game)
                SetGameSetting(*game, "Controls", key, std::to_string(static_cast<int>(next)));
              else
              {
                Config::SetBase(Config::GetInfoForWiimoteSource(wiimote), next);
                MarkConfigDirty();
              }
            }
          }
          else
          {
            const auto current =
                local ? static_cast<WiimoteSource>(std::atoi(local->c_str())) : global;
            if (current == WiimoteSource::None)
            {
              if (per_game)
                SetGameSetting(*game, "Controls", key,
                               std::to_string(static_cast<int>(WiimoteSource::Emulated)));
              else
              {
                Config::SetBase(Config::GetInfoForWiimoteSource(wiimote), WiimoteSource::Emulated);
                MarkConfigDirty();
              }
            }
            ControllerPortSettings(true, wiimote, per_game, game);
          }
        }
        else if (index == 8)
        {
          if (per_game)
          {
            if (game && game->platform.starts_with("Wii"))
            {
              EditPerGameBool(*game, "Touchscreen Wii pointer", "Core", "SwitchTouchscreenPointer",
                              Config::Get(Config::MAIN_SWITCH_TOUCHSCREEN_POINTER), delta);
            }
          }
          else
          {
            Config::SetBase(Config::MAIN_SWITCH_TOUCHSCREEN_POINTER,
                            !Config::Get(Config::MAIN_SWITCH_TOUCHSCREEN_POINTER));
            MarkConfigDirty();
          }
        }
        return false;
      },
      false,
      [&](int index) {
        if (index >= 0 && index < 4)
        {
          const std::string key = "PadType" + std::to_string(index);
          if (per_game)
            SetGameSetting(*game, "Controls", key, std::nullopt);
          else
            ResetConfigSetting(Config::GetInfoForSIDevice(index));
          return true;
        }
        if (index >= 4 && index < 8)
        {
          const int wiimote = index - 4;
          const std::string key = "WiimoteSource" + std::to_string(wiimote);
          if (per_game)
            SetGameSetting(*game, "Controls", key, std::nullopt);
          else
            ResetConfigSetting(Config::GetInfoForWiimoteSource(wiimote));
          return true;
        }
        if (index == 8)
        {
          if (per_game)
            SetGameSetting(*game, "Core", "SwitchTouchscreenPointer", std::nullopt);
          else
            ResetConfigSetting(Config::MAIN_SWITCH_TOUCHSCREEN_POINTER);
          return true;
        }
        return false;
      });
  if (game)
    game->has_game_config = RegularFileExists(GameIniPath(*game));
}

void Launcher::GameModsSettings(Game* game)
{
  if (!game || game->game_id.empty())
  {
    RenderMessage("Patches & cheats",
                  std::array<std::string, 1>{"This title does not expose a Dolphin game ID."},
                  true);
    return;
  }

  std::vector<Tools::ModEntry> mods = Tools::LoadGameMods(game->game_id, game->revision);
  const auto reload = [&] { mods = Tools::LoadGameMods(game->game_id, game->revision); };
  RunRows(
      "Patches, cheats & Riivolution", game->title,
      [&] {
        const auto local = GetGameSetting(*game, "Core", "EnableCheats");
        const bool cheats = local ? Lower(*local) == "true" || *local == "1" :
                                    Config::Get(Config::MAIN_ENABLE_CHEATS);
        const std::string cheat_value = cheats ? "Enabled" : "Disabled";
        const std::string cheat_label = local ? cheat_value : GlobalValueLabel(cheat_value);
        std::vector<Row> rows{
            {"Download Gecko codes", "GameTDB / RC24", true, false, false},
            {"Launch with Riivolution XML", "Choose patch set", true, false, false},
            {"Cheat engine", cheat_label}};
        rows.reserve(mods.size() + 3);
        for (const Tools::ModEntry& mod : mods)
        {
          const std::string type = mod.type == Tools::ModType::Patch        ? "Patch" :
                                   mod.type == Tools::ModType::ActionReplay ? "AR" :
                                                                              "Gecko";
          rows.push_back({type + " · " + mod.name, mod.enabled ? "On" : "Off", true, false, true,
                          false, true});
        }
        return rows;
      },
      [&](int index, int delta) {
        if (index == 0)
        {
          std::size_t added = 0;
          std::string error;
          const std::string database_id =
              game->game_tdb_id.empty() ? game->game_id : game->game_tdb_id;
          Tools::Result result = Tools::Result::IoError;
          RunBusyTask("Downloading Gecko codes", database_id, [&] {
            result = Tools::DownloadGeckoCodes(game->game_id, database_id, game->revision, &added,
                                               &error);
          });
          InvalidateGameSettingCache(*game);
          if (result == Tools::Result::Success)
            Toast(std::to_string(added) + " Gecko codes installed", 1200);
          else if (result == Tools::Result::Duplicate)
            Toast("All downloaded codes are already installed", 1200);
          else
            RenderMessage(
                "Gecko download failed",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
          reload();
        }
        else if (index == 1)
        {
          static constexpr std::array<std::string_view, 1> xml_extensions = {".xml"};
          const std::string xml = FileBrowser(ParentPath(game->path), false, true, false,
                                              xml_extensions, "Select Riivolution XML");
          if (xml.empty())
            return false;
          const std::string descriptor =
              File::GetUserPath(D_RIIVOLUTION_IDX) + "Presets/" + game->key + ".json";
          std::string error;
          const Tools::Result result = Tools::CreateRiivolutionPreset(
              game->path, game->game_id, game->revision, xml, descriptor, &error);
          if (result == Tools::Result::Success)
          {
            m_pending_launch = LaunchRequest{descriptor, game->game_id, game->revision};
            m_pending_launch->game_config_path = game->config_override_path;
            return true;
          }
          RenderMessage(
              "Riivolution setup failed",
              std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
        }
        else if (index == 2)
        {
          EditPerGameBool(*game, "Cheat engine", "Core", "EnableCheats",
                          Config::Get(Config::MAIN_ENABLE_CHEATS), delta, "Enabled", "Disabled");
        }
        else
        {
          const Tools::ModEntry mod = mods[index - 3];
          std::string error;
          const Tools::Result result = Tools::SetGameModEnabled(
              game->game_id, game->revision, mod.type, mod.index, !mod.enabled, &error);
          InvalidateGameSettingCache(*game);
          if (result != Tools::Result::Success)
            RenderMessage(
                "Could not update mod",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
          reload();
        }
        game->has_game_config = RegularFileExists(GameIniPath(*game));
        return false;
      },
      false,
      [&](int index) {
        if (index == 2)
        {
          SetGameSetting(*game, "Core", "EnableCheats", std::nullopt);
          game->has_game_config = RegularFileExists(GameIniPath(*game));
          return true;
        }
        if (index < 3 || index - 3 >= static_cast<int>(mods.size()))
          return false;
        const Tools::ModEntry mod = mods[index - 3];
        std::string error;
        const Tools::Result result = Tools::SetGameModEnabled(game->game_id, game->revision,
                                                              mod.type, mod.index, false, &error);
        InvalidateGameSettingCache(*game);
        reload();
        game->has_game_config = RegularFileExists(GameIniPath(*game));
        if (result != Tools::Result::Success)
        {
          RenderMessage(
              "Could not reset mod",
              std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
          return false;
        }
        return true;
      });
}

void Launcher::GCSaveManager(int slot)
{
  std::string card_path = Tools::GetDefaultMemcardPath(slot);
  std::string list_error;
  std::vector<Tools::GCSaveEntry> saves;
  const auto scan = [&] {
    list_error.clear();
    saves = Tools::ListGCSaves(card_path, &list_error);
  };
  RunBusyTask("GameCube memory card", std::string("Scanning slot ") + (slot ? "B" : "A"), scan);
  if (!list_error.empty())
  {
    RenderMessage("GameCube memory card",
                  std::array<std::string, 3>{list_error,
                                             std::string(m_localization.Translate("Slot")) + " " +
                                                 (slot ? "B" : "A"),
                                             card_path});
    return;
  }
  const auto reload = [&] { RunBusyTask("GameCube memory card", "Refreshing saves", scan); };
  RunRows(
      std::string("GameCube saves · Slot ") + (slot ? "B" : "A"), card_path,
      [&] {
        std::vector<Row> rows{{"Import GCI / GCS / SAV", "Add to memory card", true, false, false}};
        for (const auto& save : saves)
          rows.push_back({save.title,
                          save.game_code + " · " + std::to_string(save.blocks) + " blocks", true,
                          false, false, false, false});
        return rows;
      },
      [&](int index, int) {
        if (index == 0)
        {
          static constexpr std::array<std::string_view, 3> extensions = {".gci", ".gcs", ".sav"};
          const std::string path =
              FileBrowser({}, false, true, false, extensions, "Import GameCube save");
          if (path.empty())
            return false;
          std::string error;
          Tools::Result result = Tools::Result::IoError;
          RunBusyTask("Importing GameCube save", FileName(path),
                      [&] { result = Tools::ImportGCSave(card_path, path, &error); });
          if (result == Tools::Result::Success)
            Toast("GameCube save imported", 900);
          else
            RenderMessage(
                "Import failed",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
          reload();
          return false;
        }
        const int save_index = index - 1;
        if (save_index < 0 || save_index >= static_cast<int>(saves.size()))
          return false;
        const Tools::GCSaveEntry save = saves[save_index];
        const int choice = Dropdown(save.title, {"Export as GCI", "Delete"}, -1, false, true);
        if (choice == 0)
        {
          const std::string folder = FileBrowser({}, true, false, false);
          if (folder.empty())
            return false;
          std::string error;
          Tools::Result result = Tools::Result::IoError;
          RunBusyTask("Exporting GameCube save", save.title, [&] {
            result = Tools::ExportGCSave(card_path, save.directory_index, folder, &error);
          });
          if (result == Tools::Result::Success)
            Toast("GameCube save exported", 900);
          else
            RenderMessage(
                "Export failed",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
        }
        else if (choice == 1 &&
                 Confirm("Delete GameCube save?",
                         std::array<std::string, 2>{
                             save.title, std::string(m_localization.Translate(
                                             "This cannot be undone from the memory card."))}))
        {
          std::string error;
          Tools::Result result = Tools::Result::IoError;
          RunBusyTask("Deleting GameCube save", save.title, [&] {
            result = Tools::DeleteGCSave(card_path, save.directory_index, &error);
          });
          if (result == Tools::Result::Success)
            Toast("GameCube save deleted", 1000);
          else
            RenderMessage(
                "Delete failed",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
          reload();
        }
        return false;
      });
}

void Launcher::WiiSaveManager()
{
  std::vector<Tools::WiiSaveEntry> saves;
  const auto scan = [&] { saves = Tools::ListWiiSaves(); };
  RunBusyTask("Wii save manager", "Scanning emulated NAND", scan);
  const auto reload = [&] { RunBusyTask("Wii save manager", "Refreshing saves", scan); };
  RunRows(
      "Wii save manager", "Encrypted data.bin and emulated NAND",
      [&] {
        std::vector<Row> rows{
            {"Import data.bin", "Install to Wii NAND", true, false, false},
            {"Export all saves", std::to_string(saves.size()) + " found", true, false, false}};
        for (const auto& save : saves)
          rows.push_back({save.name, save.description, true, false, false, false, false});
        return rows;
      },
      [&](int index, int) {
        if (index == 0)
        {
          static constexpr std::array<std::string_view, 1> extensions = {".bin"};
          const std::string path =
              FileBrowser({}, false, true, false, extensions, "Import Wii data.bin");
          if (path.empty())
            return false;
          const bool overwrite = Confirm(
              "Import Wii save?",
              std::array<std::string, 2>{
                  FileName(path), std::string(m_localization.Translate(
                                      "An existing save for this title will be overwritten."))});
          if (!overwrite)
            return false;
          std::string error;
          Tools::Result result = Tools::Result::IoError;
          RunBusyTask("Importing Wii save", FileName(path),
                      [&] { result = Tools::ImportWiiSave(path, true, &error); });
          if (result == Tools::Result::Success)
            Toast("Wii save imported", 900);
          else
            RenderMessage(
                "Import failed",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
          reload();
        }
        else if (index == 1)
        {
          const std::string folder = FileBrowser({}, true, false, false);
          if (folder.empty())
            return false;
          std::size_t count = 0;
          std::string error;
          Tools::Result result = Tools::Result::IoError;
          RunBusyTask("Exporting Wii saves", folder,
                      [&] { result = Tools::ExportAllWiiSaves(folder, &count, &error); });
          if (result == Tools::Result::Success)
            Toast(std::to_string(count) + " Wii saves exported", 1100);
          else
            RenderMessage(
                "Export failed",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
        }
        else
        {
          const int save_index = index - 2;
          if (save_index < 0 || save_index >= static_cast<int>(saves.size()))
            return false;
          const Tools::WiiSaveEntry save = saves[save_index];
          const int choice = Dropdown(save.name, {"Export data.bin", "Delete"}, -1, false, true);
          if (choice == 0)
          {
            const std::string folder = FileBrowser({}, true, false, false);
            if (folder.empty())
              return false;
            std::string error;
            Tools::Result result = Tools::Result::IoError;
            RunBusyTask("Exporting Wii save", save.name,
                        [&] { result = Tools::ExportWiiSave(save.title_id, folder, &error); });
            if (result == Tools::Result::Success)
              Toast("Wii save exported", 900);
            else
              RenderMessage(
                  "Export failed",
                  std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
          }
          else if (choice == 1 &&
                   Confirm("Delete Wii save?",
                           std::array<std::string, 2>{
                               save.name, std::string(m_localization.Translate(
                                              "The installed channel or game is kept."))}))
          {
            std::string error;
            Tools::Result result = Tools::Result::IoError;
            RunBusyTask("Deleting Wii save", save.name,
                        [&] { result = Tools::DeleteWiiSave(save.title_id, &error); });
            if (result == Tools::Result::Success)
              Toast("Wii save deleted", 1000);
            else
              RenderMessage(
                  "Delete failed",
                  std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
            reload();
          }
        }
        return false;
      });
}

void Launcher::InstallWAD()
{
  static constexpr std::array<std::string_view, 1> extensions = {".wad"};
  const std::string path = FileBrowser({}, false, true, false, extensions, "Install WAD");
  if (path.empty() ||
      !Confirm("Install WAD?",
               std::array<std::string, 2>{FileName(path), std::string(m_localization.Translate(
                                                              "This writes to emulated NAND."))}))
    return;

  std::string error;
  Tools::Result result = Tools::Result::IoError;
  RunBusyTask("Installing Wii content", FileName(path),
              [&] { result = Tools::InstallWAD(path, &error); });
  if (result == Tools::Result::Success)
  {
    m_library_refresh_requested = true;
    Toast("WAD installed", 1000);
  }
  else
    RenderMessage("WAD install failed",
                  std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
}

void Launcher::InstalledContentManager()
{
  std::vector<Tools::InstalledTitle> titles;
  const auto scan = [&] { titles = Tools::ListInstalledWiiTitles(); };
  RunBusyTask("Installed Wii content", "Scanning emulated NAND", scan);
  const auto reload = [&] { RunBusyTask("Installed Wii content", "Refreshing titles", scan); };
  RunRows(
      "Installed Wii content", "WAD channels and NAND titles",
      [&] {
        std::vector<Row> rows{{"Install WAD", "Add channel to emulated NAND", true, false, false}};
        for (const auto& title : titles)
          rows.push_back({title.name, title.system_title ? "System title" : "Installed channel",
                          true, false, false, false, true});
        return rows;
      },
      [&](int index, int) {
        if (index == 0)
        {
          InstallWAD();
          reload();
          return false;
        }
        const int title_index = index - 1;
        if (title_index < 0 || title_index >= static_cast<int>(titles.size()))
          return false;
        const Tools::InstalledTitle title = titles[title_index];
        const std::vector<std::string> actions =
            title.system_title ?
                std::vector<std::string>{"Boot installed title"} :
                std::vector<std::string>{"Boot installed title", "Uninstall (keep save)"};
        const int choice = Dropdown(title.name, actions, -1, false, true);
        if (choice == 0)
        {
          m_pending_launch = LaunchRequest{{}, title.game_id, title.revision, title.title_id};
          return true;
        }
        if (choice == 1 &&
            Confirm("Uninstall Wii title?",
                    std::array<std::string, 3>{
                        title.name,
                        std::string(m_localization.Translate("Save data is not deleted.")),
                        std::string(m_localization.Translate(
                            title.system_title ? "Warning: this is Wii system content." :
                                                 "The channel can be reinstalled from its WAD."))}))
        {
          std::string error;
          Tools::Result result = Tools::Result::IoError;
          RunBusyTask("Uninstalling Wii content", title.name,
                      [&] { result = Tools::UninstallWiiTitle(title.title_id, &error); });
          if (result == Tools::Result::Success)
          {
            m_library_refresh_requested = true;
            Toast("Wii title uninstalled", 900);
          }
          else
            RenderMessage(
                "Uninstall failed",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
          reload();
        }
        return false;
      });
}

void Launcher::NANDManager()
{
  Tools::NANDStatus status{};
  RunBusyTask("Wii NAND tools", "Checking emulated NAND", [&] { status = Tools::CheckNAND(); });
  RunRows(
      "Wii NAND tools", File::GetUserPath(D_WIIROOT_IDX),
      [&] {
        return std::vector<Row>{
            {"NAND integrity", status.bad ? "Problems found" : "OK", false},
            {"Back up NAND", "Copy to SD / USB / SMB", true, false, false},
            {"Check and repair NAND",
             status.bad ? std::to_string(status.titles_to_remove) + " incomplete titles" :
                          "No repair needed",
             status.bad, false, false},
            {"Import BootMii NAND", "nand.bin + keys.bin", true, false, false},
            {"Extract Wii client certificates", "Choose personal BootMii backup", true, false,
             false},
        };
      },
      [&](int index, int) {
        std::string error;
        if (index == 1)
        {
          const std::string folder = FileBrowser({}, true, false, false);
          if (folder.empty())
            return false;
          const std::string destination =
              JoinPath(folder, "Dolphin-NAND-Backup-" + std::to_string(std::time(nullptr)));
          Tools::Result result = Tools::Result::IoError;
          RunBusyTask("Backing up Wii NAND", destination,
                      [&] { result = Tools::BackupNAND(destination, &error); });
          if (result == Tools::Result::Success)
            RenderMessage("NAND backup complete", std::array<std::string, 1>{destination});
          else
            RenderMessage(
                "NAND backup failed",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
        }
        else if (index == 2 && status.bad &&
                 Confirm("Repair Wii NAND?",
                         std::array<std::string, 3>{
                             "Dolphin found inconsistent or incomplete NAND content.",
                             "A full automatic backup will be created before repair.",
                             "Incomplete titles may be removed; saves can be affected."},
                         true))
        {
          const std::string backup = File::GetUserPath(D_BACKUP_IDX) + "NAND/pre-repair-" +
                                     std::to_string(std::time(nullptr));
          Tools::Result backup_result = Tools::Result::IoError;
          Tools::Result result = Tools::Result::IoError;
          RunBusyTask("Repairing Wii NAND", "Creating safety backup, then repairing", [&] {
            backup_result = Tools::BackupNAND(backup, &error);
            if (backup_result == Tools::Result::Success)
            {
              error.clear();
              result = Tools::RepairNAND(&error);
              if (result == Tools::Result::Success)
                status = Tools::CheckNAND();
            }
          });
          if (backup_result != Tools::Result::Success)
          {
            RenderMessage("Repair cancelled",
                          std::array<std::string, 2>{std::string(m_localization.Translate(
                                                         "The required safety backup failed.")),
                                                     error});
            return false;
          }
          if (result == Tools::Result::Success)
            Toast("NAND repair complete", 1100);
          else
            RenderMessage(
                "NAND repair failed",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
        }
        else if (index == 3)
        {
          static constexpr std::array<std::string_view, 1> extensions = {".bin"};
          const std::string nand =
              FileBrowser({}, false, true, false, extensions, "Select BootMii nand.bin");
          if (nand.empty())
            return false;
          std::string keys;
          if (File::GetSize(nand) == 0x21000000ULL)
            keys = FileBrowser(ParentPath(nand), false, true, false, extensions,
                               "Select BootMii keys.bin");
          if (File::GetSize(nand) == 0x21000000ULL && keys.empty())
            return false;
          if (!Confirm("Replace emulated Wii NAND?",
                       std::array<std::string, 3>{
                           FileName(nand),
                           std::string(m_localization.Translate("Back up the current NAND first.")),
                           std::string(m_localization.Translate(
                               "Imported console data replaces matching files."))}))
            return false;
          Tools::Result result = Tools::Result::IoError;
          RunBusyTask("Importing BootMii NAND", FileName(nand), [&] {
            result = Tools::ImportBootMiiNAND(nand, keys, &error);
            if (result == Tools::Result::Success)
              status = Tools::CheckNAND();
          });
          if (result == Tools::Result::Success)
            Toast("BootMii NAND imported", 1200);
          else
            RenderMessage(
                "NAND import failed",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
        }
        else if (index == 4)
          ExtractCertificatesFromNAND();
        return false;
      });
}

void Launcher::SaveDataSettings()
{
  std::size_t wii_save_count = 0;
  bool nand_bad = false;
  const auto refresh = [&] {
    wii_save_count = Tools::ListWiiSaves().size();
    nand_bad = Tools::CheckNAND().bad;
  };
  RunBusyTask("Save data & Wii NAND", "Scanning Dolphin user data", refresh);
  RunRows(
      "Save data & Wii NAND", {},
      [&] {
        return std::vector<Row>{
            {"GameCube memory card · Slot A", Tools::GetDefaultMemcardPath(0), true, false, false,
             true, false},
            {"GameCube memory card · Slot B", Tools::GetDefaultMemcardPath(1), true, false, false,
             true, false},
            {"Wii save manager", std::to_string(wii_save_count) + " saves", true, false, false},
            {"Wii NAND tools", nand_bad ? "Attention required" : "Healthy", true, false, false},
        };
      },
      [&](int index, int) {
        if (index == 0 || index == 1)
          GCSaveManager(index);
        else if (index == 2)
          WiiSaveManager();
        else
          NANDManager();
        if (m_pending_launch)
          return true;
        RunBusyTask("Save data & Wii NAND", "Refreshing Dolphin user data", refresh);
        return false;
      });
}

void Launcher::GameCubeNetworkSettings()
{
  using ExpansionInterface::EXIDeviceType;
  static constexpr std::array<EXIDeviceType, 5> devices = {
      EXIDeviceType::None, EXIDeviceType::EthernetBuiltIn, EXIDeviceType::EthernetXLink,
      EXIDeviceType::EthernetTapServer, EXIDeviceType::ModemTapServer};
  static constexpr std::array<std::string_view, 5> labels = {
      "Disabled", "Broadband Adapter (HLE)", "Broadband Adapter (XLink Kai)",
      "Broadband Adapter (TCP tapserver)", "Modem Adapter (TCP tapserver)"};
  RunRows(
      "GameCube networking", "Game-native BBA and modem emulation",
      [&] {
        const EXIDeviceType current = Config::Get(Config::MAIN_SERIAL_PORT_1);
        auto iterator = std::ranges::find(devices, current);
        const std::size_t selected = iterator == devices.end() ? 0 : iterator - devices.begin();
        return std::vector<Row>{
            {"SP1 network device", std::string(labels[selected])},
            {"HLE DNS override", Config::Get(Config::MAIN_BBA_BUILTIN_DNS),
             current == EXIDeviceType::EthernetBuiltIn, false, false, true, false},
            {"HLE local IP override", Config::Get(Config::MAIN_BBA_BUILTIN_IP),
             current == EXIDeviceType::EthernetBuiltIn, false, false, true, false},
            {"XLink Kai server", Config::Get(Config::MAIN_BBA_XLINK_IP),
             current == EXIDeviceType::EthernetXLink, false, false, true, false},
            {"XLink chat OSD", Config::Get(Config::MAIN_BBA_XLINK_CHAT_OSD) ? "On" : "Off",
             current == EXIDeviceType::EthernetXLink},
            {"BBA tapserver destination", Config::Get(Config::MAIN_BBA_TAPSERVER_DESTINATION),
             current == EXIDeviceType::EthernetTapServer, false, false, true, false},
            {"Modem tapserver destination", Config::Get(Config::MAIN_MODEM_TAPSERVER_DESTINATION),
             current == EXIDeviceType::ModemTapServer, false, false, true, false},
        };
      },
      [&](int index, int delta) {
        if (index == 0)
        {
          const EXIDeviceType current = Config::Get(Config::MAIN_SERIAL_PORT_1);
          auto iterator = std::ranges::find(devices, current);
          int selected =
              iterator == devices.end() ? 0 : static_cast<int>(iterator - devices.begin());
          if (delta == 0)
          {
            std::vector<std::string> choices;
            for (const auto label : labels)
              choices.emplace_back(label);
            selected = Dropdown("SP1 network device", choices, selected);
            if (selected < 0)
              return false;
          }
          else
          {
            selected = (selected + (delta < 0 ? -1 : 1) + devices.size()) % devices.size();
          }
          Config::SetBase(Config::MAIN_SERIAL_PORT_1, devices[selected]);
          MarkConfigDirty();
          return false;
        }
        if (index == 4)
        {
          Config::SetBase(Config::MAIN_BBA_XLINK_CHAT_OSD,
                          !Config::Get(Config::MAIN_BBA_XLINK_CHAT_OSD));
          MarkConfigDirty();
          return false;
        }
        std::string value;
        const Config::Info<std::string>* setting = nullptr;
        std::string title;
        if (index == 1)
        {
          setting = &Config::MAIN_BBA_BUILTIN_DNS;
          title = "DNS IP (blank = system DNS)";
        }
        else if (index == 2)
        {
          setting = &Config::MAIN_BBA_BUILTIN_IP;
          title = "Local IP (blank = automatic)";
        }
        else if (index == 3)
        {
          setting = &Config::MAIN_BBA_XLINK_IP;
          title = "XLink Kai server IP";
        }
        else if (index == 5)
        {
          setting = &Config::MAIN_BBA_TAPSERVER_DESTINATION;
          title = "BBA tapserver host:port";
        }
        else if (index == 6)
        {
          setting = &Config::MAIN_MODEM_TAPSERVER_DESTINATION;
          title = "Modem tapserver host:port";
        }
        if (!setting)
          return false;
        value = Config::Get(*setting);
        if (!PromptText(title, value, &value, false, index <= 2,
                        index >= 5 ? "libnx uses TCP; enter a reachable host:port." : ""))
          return false;
        if (index >= 5)
        {
          const std::size_t colon = value.rfind(':');
          const long port =
              colon == std::string::npos ? 0 : std::strtol(value.c_str() + colon + 1, nullptr, 10);
          if (colon == std::string::npos || colon == 0 || port < 1 || port > 65535)
          {
            RenderMessage("Invalid tapserver destination",
                          std::array<std::string, 1>{
                              "Enter a host and TCP port, for example 192.168.1.2:5500"},
                          true);
            return false;
          }
        }
        Config::SetBase(*setting, value);
        MarkConfigDirty();
        return false;
      },
      false,
      [&](int index) {
        switch (index)
        {
        case 0:
          ResetConfigSetting(Config::MAIN_SERIAL_PORT_1);
          break;
        case 1:
          ResetConfigSetting(Config::MAIN_BBA_BUILTIN_DNS);
          break;
        case 2:
          ResetConfigSetting(Config::MAIN_BBA_BUILTIN_IP);
          break;
        case 3:
          ResetConfigSetting(Config::MAIN_BBA_XLINK_IP);
          break;
        case 4:
          ResetConfigSetting(Config::MAIN_BBA_XLINK_CHAT_OSD);
          break;
        case 5:
          ResetConfigSetting(Config::MAIN_BBA_TAPSERVER_DESTINATION);
          break;
        case 6:
          ResetConfigSetting(Config::MAIN_MODEM_TAPSERVER_DESTINATION);
          break;
        default:
          return false;
        }
        return true;
      },
      [](int index) { return index == 1 || index == 2 || index == 3 || index == 5 || index == 6; });
}

void Launcher::AchievementSettings()
{
  auto& manager = AchievementManager::GetInstance();
  const auto save = [&] {
    MarkConfigDirty();
    FlushPendingSaves();
  };

  RunRows(
      "RetroAchievements", "Native Dolphin integration",
      [&] {
        const bool enabled = Config::Get(Config::RA_ENABLED);
        const bool signed_in = manager.HasAPIToken();
        std::string account = signed_in ? Config::Get(Config::RA_USERNAME) + " · " +
                                              std::string(m_localization.Translate("signed in")) :
                                          std::string(m_localization.Translate("Not signed in"));
        if (enabled && signed_in && manager.GetClient())
        {
          std::lock_guard lock{manager.GetLock()};
          const std::string_view display_name = manager.GetPlayerDisplayName();
          if (!display_name.empty())
          {
            account = std::string(display_name) + " · " + std::to_string(manager.GetPlayerScore()) +
                      " " + std::string(m_localization.Translate("points"));
          }
        }
        return std::vector<Row>{
            {"Enable RetroAchievements", enabled ? "On" : "Off"},
            {"Account", account, enabled, false, false, true, false},
            {"Hardcore mode", Config::Get(Config::RA_HARDCORE_ENABLED) ? "On" : "Off", enabled},
            {"Unofficial achievements", Config::Get(Config::RA_UNOFFICIAL_ENABLED) ? "On" : "Off",
             enabled},
            {"Encore mode", Config::Get(Config::RA_ENCORE_ENABLED) ? "On" : "Off", enabled},
            {"Spectator mode", Config::Get(Config::RA_SPECTATOR_ENABLED) ? "On" : "Off", enabled},
            {"Leaderboard tracker",
             Config::Get(Config::RA_LEADERBOARD_TRACKER_ENABLED) ? "On" : "Off", enabled},
            {"Challenge indicators",
             Config::Get(Config::RA_CHALLENGE_INDICATORS_ENABLED) ? "On" : "Off", enabled},
            {"Progress notifications", Config::Get(Config::RA_PROGRESS_ENABLED) ? "On" : "Off",
             enabled},
        };
      },
      [&](int index, int) {
        if (index == 0)
        {
          const bool enabled = !Config::Get(Config::RA_ENABLED);
          Config::SetBase(Config::RA_ENABLED, enabled);
          if (enabled)
            manager.Init(nullptr);
          else
            manager.Shutdown();
          save();
          return false;
        }

        if (index == 1)
        {
          if (manager.HasAPIToken())
          {
            if (Confirm("Log out of RetroAchievements?",
                        std::array<std::string, 2>{Config::Get(Config::RA_USERNAME),
                                                   std::string(m_localization.Translate(
                                                       "The saved API token will be removed."))}))
            {
              manager.Logout();
              save();
              Toast("RetroAchievements account logged out", 1200);
            }
            return false;
          }

          std::string username = Config::Get(Config::RA_USERNAME);
          if (!PromptText("RetroAchievements username", username, &username, false, false))
            return false;
          std::string password;
          if (!PromptText("RetroAchievements password", {}, &password, true, false,
                          "Your password is exchanged for an API token and is not saved."))
          {
            return false;
          }

          username = Trim(std::move(username));
          if (username.empty())
          {
            Toast("Enter a RetroAchievements username", 1200);
            return false;
          }
          Config::SetBase(Config::RA_USERNAME, username);
          manager.Init(nullptr);
          struct LoginState
          {
            std::mutex mutex;
            std::condition_variable condition;
            bool complete = false;
            int result = RC_NO_RESPONSE;
          };
          auto state = std::make_shared<LoginState>();
          Common::EventHook login_hook = manager.login_event.Register([state](int result) {
            {
              std::lock_guard lock{state->mutex};
              state->result = result;
              state->complete = true;
            }
            state->condition.notify_all();
          });
          RunBusyTask("RetroAchievements", "Signing in", [&] {
            manager.Login(password);
            std::unique_lock lock{state->mutex};
            state->condition.wait_for(lock, std::chrono::seconds(15),
                                      [&] { return state->complete; });
          });
          std::fill(password.begin(), password.end(), '\0');
          password.clear();
          save();

          int result = RC_NO_RESPONSE;
          {
            std::lock_guard lock{state->mutex};
            if (state->complete)
              result = state->result;
          }
          if (result == RC_OK)
          {
            Toast("Signed in to RetroAchievements", 1300);
          }
          else
          {
            const std::string reason = result == RC_INVALID_CREDENTIALS ?
                                           "Invalid username or password." :
                                       result == RC_EXPIRED_TOKEN || result == RC_LOGIN_REQUIRED ?
                                           "The account credentials were rejected." :
                                           "The RetroAchievements server could not be reached.";
            RenderMessage("RetroAchievements login failed", std::array<std::string, 1>{reason},
                          true);
          }
          return false;
        }

        if (index == 2)
        {
          const bool enabled = !Config::Get(Config::RA_HARDCORE_ENABLED);
          if (!enabled &&
              !Confirm("Disable Hardcore mode?",
                       std::array<std::string, 2>{"Achievements will continue in Softcore mode.",
                                                  "Leaderboard submissions require Hardcore mode."},
                       true))
          {
            return false;
          }
          Config::SetBase(Config::RA_HARDCORE_ENABLED, enabled);
        }
        else if (index == 3)
        {
          Config::SetBase(Config::RA_UNOFFICIAL_ENABLED,
                          !Config::Get(Config::RA_UNOFFICIAL_ENABLED));
        }
        else if (index == 4)
        {
          Config::SetBase(Config::RA_ENCORE_ENABLED, !Config::Get(Config::RA_ENCORE_ENABLED));
        }
        else if (index == 5)
        {
          Config::SetBase(Config::RA_SPECTATOR_ENABLED, !Config::Get(Config::RA_SPECTATOR_ENABLED));
          if (manager.GetClient())
            manager.SetSpectatorMode();
        }
        else if (index == 6)
        {
          Config::SetBase(Config::RA_LEADERBOARD_TRACKER_ENABLED,
                          !Config::Get(Config::RA_LEADERBOARD_TRACKER_ENABLED));
        }
        else if (index == 7)
        {
          Config::SetBase(Config::RA_CHALLENGE_INDICATORS_ENABLED,
                          !Config::Get(Config::RA_CHALLENGE_INDICATORS_ENABLED));
        }
        else if (index == 8)
        {
          Config::SetBase(Config::RA_PROGRESS_ENABLED, !Config::Get(Config::RA_PROGRESS_ENABLED));
        }
        save();
        return false;
      },
      false,
      [&](int index) {
        switch (index)
        {
        case 0:
          ResetConfigSetting(Config::RA_ENABLED);
          if (Config::Get(Config::RA_ENABLED))
            manager.Init(nullptr);
          else
            manager.Shutdown();
          break;
        case 2:
          ResetConfigSetting(Config::RA_HARDCORE_ENABLED);
          break;
        case 3:
          ResetConfigSetting(Config::RA_UNOFFICIAL_ENABLED);
          break;
        case 4:
          ResetConfigSetting(Config::RA_ENCORE_ENABLED);
          break;
        case 5:
          ResetConfigSetting(Config::RA_SPECTATOR_ENABLED);
          if (manager.GetClient())
            manager.SetSpectatorMode();
          break;
        case 6:
          ResetConfigSetting(Config::RA_LEADERBOARD_TRACKER_ENABLED);
          break;
        case 7:
          ResetConfigSetting(Config::RA_CHALLENGE_INDICATORS_ENABLED);
          break;
        case 8:
          ResetConfigSetting(Config::RA_PROGRESS_ENABLED);
          break;
        default:
          return false;
        }
        save();
        return true;
      });
}

void Launcher::NetworkSettings()
{
  Tools::WiiNetworkStatus wii_status = Tools::GetWiiNetworkStatus();
  RunRows(
      "Online & accounts", "Wii network / GameCube BBA",
      [&]() {
        const auto gc = Config::Get(Config::MAIN_SERIAL_PORT_1);
        return std::vector<Row>{
            {"Wii wired / DHCP profile",
             wii_status.network_config_present ? "Configured" : "Missing", false},
            {"Reset Wii network profile", "Wired · DHCP · automatic DNS", true, false, false},
            {"Wii NAND client certificate",
             wii_status.client_certificate_present && wii_status.client_private_key_present ?
                 "Certificate + private key ready" :
                 "Missing / incomplete",
             false},
            {"Extract certificates from NAND", "Choose personal BootMii backup", true, false,
             false},
            {"GameCube BBA / modem",
             gc == ExpansionInterface::EXIDeviceType::None ? "Disabled" : "Configured", true, false,
             false}};
      },
      [&](int index, int) {
        std::string error;
        if (index == 1)
        {
          Tools::Result result = Tools::Result::IoError;
          RunBusyTask("Configuring Wii networking", "Wired DHCP and automatic DNS",
                      [&] { result = Tools::ResetWiiNetworkConfiguration(&error); });
          if (result == Tools::Result::Success)
            Toast("Wii wired/DHCP profile ready", 1000);
          else
            RenderMessage(
                "Wii network setup failed",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
          wii_status = Tools::GetWiiNetworkStatus();
        }
        else if (index == 3)
        {
          ExtractCertificatesFromNAND();
          wii_status = Tools::GetWiiNetworkStatus();
        }
        else if (index == 4)
          GameCubeNetworkSettings();
        return false;
      });
}

void Launcher::ExtractCertificatesFromNAND()
{
  static constexpr std::array<std::string_view, 1> extensions = {".bin"};
  const std::string nand =
      FileBrowser({}, false, true, false, extensions, "Select BootMii nand.bin");
  if (nand.empty())
    return;

  std::string keys;
  constexpr u64 nand_without_keys_size = 0x21000000ULL;
  constexpr u64 keys_size = 0x400ULL;
  if (File::GetSize(nand) == nand_without_keys_size)
  {
    // BootMii normally stores keys.bin beside nand.bin.
    const std::string adjacent_keys = JoinPath(ParentPath(nand), "keys.bin");
    if (File::GetSize(adjacent_keys) == keys_size)
      keys = adjacent_keys;
    else
      keys = FileBrowser(ParentPath(nand), false, true, false, extensions,
                         "Select the matching keys.bin");
    if (keys.empty())
      return;
  }

  Tools::NANDCertificateSource source;
  source.path = nand;
  source.keys_path = keys;

  std::string error;
  Tools::Result result = Tools::Result::IoError;
  RunBusyTask("Extracting Wii certificates", FileName(nand),
              [&] { result = Tools::ExtractNANDCertificates(source, &error); });
  if (result == Tools::Result::Success)
  {
    Toast("Wii client certificates ready", 1100);
  }
  else
  {
    RenderMessage("Certificate extraction failed",
                  std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
  }
}

bool Launcher::ChooseForwarderIcon(Game* game, std::string* output_path)
{
  if (!game || !output_path)
    return false;
  const std::string base = std::string(DATA_DIRECTORY) + "/forwarders";
  const std::string temporary_directory = base + "/iconpick";
  EnsureDirectory(base);
  EnsureDirectory(temporary_directory);
  if (DIR* directory = ::opendir(temporary_directory.c_str()))
  {
    while (dirent* entry = ::readdir(directory))
    {
      const std::string name = entry->d_name;
      if (name.starts_with("gicon_") && name.ends_with(".png"))
        std::remove(JoinPath(temporary_directory, name).c_str());
    }
    ::closedir(directory);
  }

  std::vector<std::string> paths;
  std::atomic_bool cancel{false};
  const std::string cover_path = CoverPath(*game);
  if (RegularFileExists(cover_path))
    paths.push_back(cover_path);
  paths.emplace_back("romfs:/fwd/dolphin_icon.png");

  const std::string api_key = m_store.Get("Network/SteamGridDBKey");
  if (!api_key.empty())
  {
    RunBusyTask(
        "Fetching icons from SteamGridDB", game->title,
        [&] {
          CoverDownload::RequestOptions options{&cancel};
          std::vector<CoverDownload::GameResult> games;
          if (CoverDownload::SearchGames(api_key, game->title, &games, &options) ==
                  CoverDownload::Result::Ok &&
              !games.empty())
          {
            std::vector<CoverDownload::Artwork> icons;
            if (CoverDownload::FetchIcons(api_key, games.front().id, &icons, &options) ==
                CoverDownload::Result::Ok)
            {
              for (std::size_t index = 0;
                   index < icons.size() && index < 14 && !cancel.load(std::memory_order_acquire);
                   ++index)
              {
                const std::string path =
                    JoinPath(temporary_directory, "gicon_" + std::to_string(index) + ".png");
                if (CoverDownload::DownloadImage(icons[index].url, path, &options) ==
                    CoverDownload::Result::Ok)
                  paths.push_back(path);
              }
            }
          }
        },
        &cancel);
  }
  if (paths.empty())
  {
    Toast("No icon found - download a cover first", 1600);
    return false;
  }

  const int count = static_cast<int>(paths.size());
  const int columns = std::max(1, std::min(count, 5));
  const int rows = (count + columns - 1) / columns;
  constexpr int gap = 18;
  constexpr int top = 150;
  constexpr int bottom = 40;
  const int cell_width = (m_width - 80 - (columns - 1) * gap) / columns;
  const int cell_height = (m_height - top - bottom - (rows - 1) * gap) / rows;
  const int cell = std::clamp(std::min(cell_width, cell_height), 90, 200);
  const int x0 = (m_width - (columns * cell + (columns - 1) * gap)) / 2;
  const int y0 = top;
  std::vector<SDL_Texture*> textures(count, nullptr);
  for (int index = 0; index < count; ++index)
    textures[index] = LoadScaledTexture(paths[index], cell, cell);
  int selection = 0;
  int chosen = -1;
  bool done = false;
  BeginScreenFx();
  while (!done && BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::ScrollUp)
      {
        selection = std::min(count - 1, selection + columns);
        continue;
      }
      if (touch == TouchKind::ScrollDown)
      {
        selection = std::max(0, selection - columns);
        continue;
      }
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 40)
        {
          done = true;
          continue;
        }
        for (int index = 0; index < count; ++index)
        {
          const int row = index / columns;
          const int column = index % columns;
          const int x = x0 + column * (cell + gap);
          const int y = y0 + row * (cell + gap);
          if (touch_x >= x && touch_x < x + cell && touch_y >= y && touch_y < y + cell)
          {
            selection = index;
            chosen = index;
            done = true;
            break;
          }
        }
        continue;
      }
      if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_RIGHT)
          selection = (selection + 1) % count;
        else if (event.key.keysym.sym == SDLK_LEFT)
          selection = (selection + count - 1) % count;
        else if (event.key.keysym.sym == SDLK_DOWN)
          selection = (selection + columns) % count;
        else if (event.key.keysym.sym == SDLK_UP)
          selection = (selection - columns + count) % count;
        else if (event.key.keysym.sym == SDLK_RETURN)
        {
          chosen = selection;
          done = true;
        }
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          done = true;
      }
      if (event.type != SDL_CONTROLLERBUTTONDOWN)
        continue;
      if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        selection = (selection + 1) % count;
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        selection = (selection + count - 1) % count;
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        selection = (selection + columns) % count;
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        selection = (selection - columns + count) % count;
      else if (event.cbutton.button == BUTTON_CONFIRM)
      {
        chosen = selection;
        done = true;
      }
      else if (event.cbutton.button == BUTTON_CANCEL)
        done = true;
    }

    ClearBackground();
    DrawHeader("Choose an icon", game->title);
    for (int index = 0; index < count; ++index)
    {
      const int row = index / columns;
      const int column = index % columns;
      const int x = x0 + column * (cell + gap);
      const int y = y0 + row * (cell + gap);
      if (index == selection)
        FillRect(x - 6, y - 6, cell + 12, cell + 12, m_selection);
      FillRect(x, y, cell, cell, m_card);
      if (textures[index])
      {
        SDL_Rect destination{x, y, cell, cell};
        SDL_RenderCopy(m_renderer, textures[index], nullptr, &destination);
      }
      else
      {
        DrawTextCentered(m_font_small, x + cell / 2, y + cell / 2, "?", m_dim);
      }
    }
    DrawSettingsFooter("A  Select       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  for (SDL_Texture* texture : textures)
  {
    if (texture)
      SDL_DestroyTexture(texture);
  }
  if (chosen < 0 || chosen >= count)
    return false;
  *output_path = paths[chosen];
  return true;
}

void Launcher::CreateHomeShortcut(Game* game)
{
  if (!game)
    return;
  constexpr int icon_x = 110;
  constexpr int icon_y = 176;
  constexpr int icon_size = 280;
  constexpr int name_y = 196;
  constexpr int author_y = 290;
  constexpr int create_y = 406;
  constexpr int field_height = 64;
  constexpr int create_height = 58;
  const int right_x = icon_x + icon_size + 70;
  const int right_width = m_width - right_x - 90;
  std::string name = game->title;
  std::string icon_path = CoverPath(*game);
  if (!RegularFileExists(icon_path))
    icon_path = "romfs:/fwd/dolphin_icon.png";
  SDL_Texture* icon = LoadScaledTexture(icon_path, icon_size, icon_size);
  int selection = 0;
  bool done = false;

  const auto edit = [&](std::string_view title, std::string* value) {
    std::string replacement;
    if (PromptText(title, *value, &replacement, false, false) && !replacement.empty())
      *value = std::move(replacement);
  };
  const auto build = [&] {
    if (icon_path.empty())
    {
      Toast("Pick an icon first", 1200);
      return;
    }
    std::array<char, 512> error{};
    bool created = false;
    std::vector<std::string> legacy_game_paths;
    if (!game->installed_nand)
    {
      const auto identity =
          std::ranges::find(m_library_identities, game->key, &LibraryIdentityRecord::id);
      if (identity != m_library_identities.end())
        legacy_game_paths = identity->previous_paths;
      // The installed shortcut immediately depends on this launcher.ini record. Commit it before
      // making the external HOME Menu mutation so a crash or power loss cannot leave a shortcut
      // referring to a progressive-scan identity which only existed in memory.
      if (m_library_identities_dirty)
        SaveLibraryIdentities();
      FlushPendingSaves();
    }
    RunBusyTask("Creating HOME shortcut", game->title, [&] {
      created = game->installed_nand ?
                    Forwarder::CreateNANDTitle(game->title_id, name, icon_path, game->key,
                                               error.data(), error.size()) :
                    Forwarder::Create(game->path, name, icon_path, game->config_override_path,
                                      game->key, legacy_game_paths, error.data(), error.size());
    });
    if (created)
    {
      Toast("HOME shortcut installed", 1800);
      done = true;
    }
    else
    {
      RenderMessage("Shortcut failed",
                    std::array<std::string, 1>{error[0] ? error.data() : "Unknown error"});
    }
    BeginScreenFx();
  };
  const auto activate = [&] {
    if (selection == 0)
    {
      std::string selected_path;
      if (ChooseForwarderIcon(game, &selected_path))
      {
        icon_path = std::move(selected_path);
        if (icon)
          SDL_DestroyTexture(icon);
        icon = LoadScaledTexture(icon_path, icon_size, icon_size);
      }
      BeginScreenFx();
    }
    else if (selection == 1)
    {
      edit("Shortcut name", &name);
      BeginScreenFx();
    }
    else
    {
      build();
    }
  };

  BeginScreenFx();
  while (!done && BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::Tap)
      {
        if (touch_x >= icon_x && touch_x < icon_x + icon_size && touch_y >= icon_y &&
            touch_y < icon_y + icon_size)
        {
          selection = 0;
          activate();
        }
        else if (touch_y >= name_y - 6 && touch_y < name_y + field_height)
        {
          selection = 1;
          activate();
        }
        else if (touch_y >= create_y - 6 && touch_y < create_y + create_height)
        {
          selection = 2;
          activate();
        }
        else if (touch_y >= m_height - 40)
        {
          done = true;
        }
        continue;
      }
      if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_LEFT)
          selection = 0;
        else if (event.key.keysym.sym == SDLK_RIGHT && selection == 0)
          selection = 1;
        else if (event.key.keysym.sym == SDLK_UP)
          selection = selection == 0 ? 2 : (selection == 1 ? 2 : selection - 1);
        else if (event.key.keysym.sym == SDLK_DOWN)
          selection = selection == 0 ? 1 : (selection == 2 ? 1 : selection + 1);
        else if (event.key.keysym.sym == SDLK_RETURN)
          activate();
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          done = true;
      }
      if (event.type != SDL_CONTROLLERBUTTONDOWN)
        continue;
      if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        selection = 0;
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && selection == 0)
        selection = 1;
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        selection = selection == 0 ? 2 : (selection == 1 ? 2 : selection - 1);
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        selection = selection == 0 ? 1 : (selection == 2 ? 1 : selection + 1);
      else if (event.cbutton.button == BUTTON_CONFIRM)
        activate();
      else if (event.cbutton.button == BUTTON_CANCEL)
        done = true;
    }

    ClearBackground();
    DrawHeader("Create HOME shortcut", game->title);
    if (selection == 0)
      FillRect(icon_x - 6, icon_y - 6, icon_size + 12, icon_size + 12, m_selection);
    FillRect(icon_x, icon_y, icon_size, icon_size, m_card);
    if (icon)
    {
      SDL_Rect destination{icon_x, icon_y, icon_size, icon_size};
      SDL_RenderCopy(m_renderer, icon, nullptr, &destination);
    }
    else
    {
      DrawTextCentered(m_font_small, icon_x + icon_size / 2, icon_y + icon_size / 2,
                       m_localization.Translate("(no icon)"), m_dim);
    }
    DrawTextCentered(m_font_small, icon_x + icon_size / 2, icon_y + icon_size + 20,
                     m_localization.Translate("Icon"), selection == 0 ? m_value : m_dim);
    const auto field = [&](int index, int y, std::string_view label, std::string_view value) {
      const bool current = selection == index;
      if (current)
      {
        FillRect(right_x - 10, y - 6, right_width + 20, field_height, m_focus);
        FillRect(right_x - 10, y - 6, 5, field_height, m_selection);
      }
      DrawText(m_font_small, right_x, y, label, current ? m_value : m_dim);
      DrawScrollingTextLeft(m_font, right_x, y + 26, right_width - 8, value,
                            current ? m_value : m_text);
    };
    field(1, name_y, m_localization.Translate("Name"), name);
    DrawText(m_font_small, right_x, author_y, m_localization.Translate("Author / Version"), m_dim);
    const std::string metadata =
        std::string(DOLPHIN_SWITCH_RELEASE_AUTHOR) + "  |  " + Updater::BuiltReleaseTag();
    DrawScrollingTextLeft(m_font, right_x, author_y + 26, right_width - 8, metadata, m_text);
    const bool create_selected = selection == 2;
    FillRect(right_x - 10, create_y - 6, right_width + 20, create_height,
             create_selected ? SDL_Color{44, 86, 44, 240} : SDL_Color{30, 46, 32, 200});
    if (create_selected)
      FillRect(right_x - 10, create_y - 6, 5, create_height, m_selection);
    DrawTextCentered(m_font, right_x + right_width / 2, create_y + 12,
                     m_localization.Translate("Create shortcut"),
                     create_selected ? m_value : SDL_Color{150, 225, 150, 255});
    DrawSettingsFooter("A  Edit / choose       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  if (icon)
    SDL_DestroyTexture(icon);
}

std::string Launcher::InstalledReleaseTag() const
{
  const std::string built = Updater::BuiltReleaseTag();
  const std::string stored = Trim(m_store.Get("Launcher/InstalledReleaseTag"));
  if (stored.empty())
    return built;
  return Updater::IsNewer(stored, built) ? stored : built;
}

std::string Launcher::UpdateStatusText() const
{
  const Updater::Snapshot snapshot = Updater::GetSnapshot();
  switch (snapshot.state)
  {
  case Updater::State::Checking:
    return std::string(m_localization.Translate("Checking..."));
  case Updater::State::UpdateAvailable:
    return snapshot.release.tag + " " + std::string(m_localization.Translate("available"));
  case Updater::State::UpToDate:
    return std::string(m_localization.Translate("Up to date"));
  case Updater::State::Downloading:
  {
    const std::uint64_t total = snapshot.total ? snapshot.total : snapshot.release.asset_size;
    const std::uint64_t percent =
        total ? std::min<std::uint64_t>(100, snapshot.downloaded * 100 / total) : 0;
    return std::string(m_localization.Translate("Downloading")) + " " + std::to_string(percent) +
           "%";
  }
  case Updater::State::ReadyToInstall:
    return std::string(m_localization.Translate("Ready to install"));
  case Updater::State::Installing:
    return std::string(m_localization.Translate("Installing..."));
  case Updater::State::Installed:
    return std::string(m_localization.Translate("Installed"));
  case Updater::State::Cancelled:
    return std::string(m_localization.Translate("Cancelled"));
  case Updater::State::Error:
    return std::string(m_localization.Translate("Update check failed"));
  case Updater::State::Idle:
    break;
  }
  return std::string(m_localization.Translate("Installed")) + " " + InstalledReleaseTag();
}

std::vector<std::string> Launcher::WrapUpdateNotes(std::string_view text, int max_width)
{
  const auto utf8_boundaries = [](const std::string& value) {
    std::vector<std::size_t> boundaries{0};
    for (std::size_t index = 0; index < value.size();)
    {
      const unsigned char lead = static_cast<unsigned char>(value[index]);
      std::size_t length = lead < 0x80           ? 1 :
                           (lead & 0xe0) == 0xc0 ? 2 :
                           (lead & 0xf0) == 0xe0 ? 3 :
                           (lead & 0xf8) == 0xf0 ? 4 :
                                                   1;
      if (index + length > value.size())
        length = 1;
      for (std::size_t part = 1; part < length; ++part)
      {
        if ((static_cast<unsigned char>(value[index + part]) & 0xc0) != 0x80)
        {
          length = 1;
          break;
        }
      }
      index += length;
      boundaries.push_back(index);
    }
    return boundaries;
  };

  std::vector<std::string> lines;
  const std::string notes(text);
  std::size_t paragraph_start = 0;
  while (paragraph_start <= notes.size())
  {
    std::size_t paragraph_end = notes.find('\n', paragraph_start);
    if (paragraph_end == std::string::npos)
      paragraph_end = notes.size();
    std::string paragraph = notes.substr(paragraph_start, paragraph_end - paragraph_start);
    if (!paragraph.empty() && paragraph.back() == '\r')
      paragraph.pop_back();
    for (char& value : paragraph)
    {
      if (value == '\t' || static_cast<unsigned char>(value) < 0x20)
        value = ' ';
    }
    while (!paragraph.empty() && paragraph.back() == ' ')
      paragraph.pop_back();
    if (paragraph.empty())
    {
      lines.emplace_back();
    }
    else
    {
      bool continuation = false;
      while (!paragraph.empty())
      {
        while (!paragraph.empty() && paragraph.front() == ' ')
          paragraph.erase(paragraph.begin());
        if (paragraph.empty())
          break;
        const std::string prefix =
            continuation && !paragraph.starts_with("- ") ? "  " : std::string{};
        if (TextWidth(m_font_small, prefix + paragraph) <= max_width)
        {
          lines.push_back(prefix + paragraph);
          break;
        }
        const std::vector<std::size_t> boundaries = utf8_boundaries(paragraph);
        std::size_t low = std::min<std::size_t>(1, boundaries.size() - 1);
        std::size_t high = boundaries.size() - 1;
        while (low < high)
        {
          const std::size_t middle = (low + high + 1) / 2;
          if (TextWidth(m_font_small, prefix + paragraph.substr(0, boundaries[middle])) <=
              max_width)
          {
            low = middle;
          }
          else
          {
            high = middle - 1;
          }
        }
        std::size_t split = boundaries[low];
        const std::size_t space = paragraph.rfind(' ', split);
        if (space != std::string::npos && space > 0 && space >= split / 3)
          split = space;
        lines.push_back(prefix + paragraph.substr(0, split));
        paragraph.erase(0, split);
        continuation = true;
      }
    }
    if (paragraph_end == notes.size())
      break;
    paragraph_start = paragraph_end + 1;
  }
  if (lines.empty())
    lines.emplace_back(m_localization.Translate("No release notes were provided."));
  return lines;
}

void Launcher::UpdateScreen()
{
  if (!m_cover_download_ready)
  {
    RenderMessage("Update check unavailable",
                  std::array<std::string, 2>{"The network connection could not be initialized.",
                                             "Check the connection and try again."},
                  true);
    return;
  }

  Updater::Snapshot initial = Updater::GetSnapshot();
  if (initial.state == Updater::State::Idle || initial.state == Updater::State::UpToDate)
    Updater::StartCheck(InstalledReleaseTag());

  int scroll = 0;
  bool cancel_requested = false;
  std::string wrapped_key;
  std::vector<std::string> wrapped_lines;
  BeginScreenFx();
  while (BeginFrame())
  {
    Updater::Snapshot snapshot = Updater::GetSnapshot();
    const int panel_width = m_width * 7 / 8;
    const int panel_height = m_height * 4 / 5;
    const int panel_x = (m_width - panel_width) / 2;
    const int panel_y = (m_height - panel_height) / 2;
    const int body_x = panel_x + 42;
    const int body_y = panel_y + 126;
    const int body_width = panel_width - 84;
    const int body_bottom = panel_y + panel_height - 108;
    const int line_height = TTF_FontHeight(m_font_small) + 8;
    const int visible_lines = std::max(1, (body_bottom - body_y) / line_height);
    const std::string release_text =
        snapshot.release.notes.empty() ?
            std::string(m_localization.Translate("No release notes were provided.")) :
            snapshot.release.notes;
    const std::string next_key = snapshot.release.tag + '\n' + release_text;
    if (wrapped_key != next_key)
    {
      wrapped_key = next_key;
      wrapped_lines = WrapUpdateNotes(release_text, body_width);
      scroll = 0;
    }
    const int max_scroll = std::max(0, static_cast<int>(wrapped_lines.size()) - visible_lines);
    scroll = std::clamp(scroll, 0, max_scroll);

    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::ScrollUp)
        scroll = std::min(max_scroll, scroll + std::max(1, m_touch_scroll_steps));
      else if (touch == TouchKind::ScrollDown)
        scroll = std::max(0, scroll - std::max(1, m_touch_scroll_steps));

      const bool controller = event.type == SDL_CONTROLLERBUTTONDOWN;
      const bool keyboard = event.type == SDL_KEYDOWN;
      if (!controller && !keyboard)
        continue;
      const int button = controller ? event.cbutton.button : -1;
      const SDL_Keycode key = keyboard ? event.key.keysym.sym : SDLK_UNKNOWN;
      if (button == SDL_CONTROLLER_BUTTON_DPAD_UP || key == SDLK_UP)
        scroll = std::max(0, scroll - 1);
      else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN || key == SDLK_DOWN)
        scroll = std::min(max_scroll, scroll + 1);
      else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || key == SDLK_PAGEUP)
        scroll = std::max(0, scroll - visible_lines);
      else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER || key == SDLK_PAGEDOWN)
        scroll = std::min(max_scroll, scroll + visible_lines);
      else if (button == BUTTON_CANCEL || key == SDLK_ESCAPE)
      {
        if (snapshot.state == Updater::State::Downloading)
        {
          Updater::Cancel();
          cancel_requested = true;
        }
        else
        {
          return;
        }
      }
      else if (button == BUTTON_CONFIRM || key == SDLK_RETURN)
      {
        if (snapshot.state == Updater::State::UpdateAvailable)
        {
          if (Updater::StartDownload(m_launcher_path))
            cancel_requested = false;
        }
        else if (snapshot.state == Updater::State::ReadyToInstall)
        {
          FlushPendingSaves();
          Updater::RequestInstallation();
          m_running = false;
          return;
        }
        else if (snapshot.state == Updater::State::Error ||
                 snapshot.state == Updater::State::Cancelled ||
                 snapshot.state == Updater::State::UpToDate)
        {
          cancel_requested = false;
          Updater::StartCheck(InstalledReleaseTag());
        }
      }
    }

    snapshot = Updater::GetSnapshot();
    ClearBackground();
    FillRect(0, 0, m_width, m_height, SDL_Color{0, 0, 0, 105});
    GlassPanel(panel_x, panel_y, panel_width, panel_height);
    Border(panel_x, panel_y, panel_width, panel_height, 3, m_selection);
    DrawTextCentered(m_font_large, m_width / 2, panel_y + 24,
                     m_localization.Translate("Dolphin Update"), m_selection);

    std::string status;
    switch (snapshot.state)
    {
    case Updater::State::Idle:
      status = m_localization.Translate("Ready to check for updates");
      break;
    case Updater::State::Checking:
      status = m_localization.Translate("Checking GitHub for the latest release...");
      break;
    case Updater::State::UpdateAvailable:
      status = std::string(m_localization.Translate("Version")) + " " + snapshot.release.tag + " " +
               std::string(m_localization.Translate("is available")) + "    " +
               std::string(m_localization.Translate("Installed:")) + " " + InstalledReleaseTag();
      break;
    case Updater::State::UpToDate:
      status = std::string(m_localization.Translate("You are up to date")) + "    " +
               std::string(m_localization.Translate("Installed:")) + " " + InstalledReleaseTag();
      break;
    case Updater::State::Downloading:
      status = cancel_requested ? std::string(m_localization.Translate("Cancelling download...")) :
                                  std::string(m_localization.Translate("Downloading")) + " " +
                                      snapshot.release.asset_name;
      break;
    case Updater::State::ReadyToInstall:
      status = m_localization.Translate("Download verified - ready to install and exit Dolphin");
      break;
    case Updater::State::Installing:
      status = m_localization.Translate("Installing update...");
      break;
    case Updater::State::Installed:
      status = m_localization.Translate("Update installed successfully");
      break;
    case Updater::State::Cancelled:
      status = m_localization.Translate("Update cancelled");
      break;
    case Updater::State::Error:
      status = snapshot.error.empty() ? std::string(m_localization.Translate("Update failed")) :
                                        snapshot.error;
      break;
    }
    DrawScrollingTextLeft(m_font_small, body_x, panel_y + 92, body_width, status,
                          snapshot.state == Updater::State::Error ? SDL_Color{235, 125, 115, 255} :
                                                                    m_value);

    SDL_Rect clip{body_x, body_y, body_width, body_bottom - body_y};
    SDL_RenderSetClipRect(m_renderer, &clip);
    for (int row = 0; row < visible_lines && scroll + row < static_cast<int>(wrapped_lines.size());
         ++row)
    {
      DrawText(m_font_small, body_x, body_y + row * line_height, wrapped_lines[scroll + row],
               m_text);
    }
    SDL_RenderSetClipRect(m_renderer, nullptr);
    if (static_cast<int>(wrapped_lines.size()) > visible_lines)
    {
      const int track_x = panel_x + panel_width - 25;
      const int track_height = body_bottom - body_y;
      FillRect(track_x, body_y, 4, track_height, SDL_Color{40, 44, 54, 255});
      const int thumb_height =
          std::max(18, track_height * visible_lines / static_cast<int>(wrapped_lines.size()));
      FillRect(track_x, body_y + (track_height - thumb_height) * scroll / std::max(1, max_scroll),
               4, thumb_height, m_selection);
    }

    if (snapshot.state == Updater::State::Downloading)
    {
      const std::uint64_t total = snapshot.total ? snapshot.total : snapshot.release.asset_size;
      const int percent =
          total ?
              static_cast<int>(std::min<std::uint64_t>(100, snapshot.downloaded * 100 / total)) :
              0;
      const int bar_x = body_x;
      const int bar_y = panel_y + panel_height - 82;
      const int bar_width = body_width;
      constexpr int bar_height = 24;
      Border(bar_x, bar_y, bar_width, bar_height, 2, m_selection);
      FillRect(bar_x + 3, bar_y + 3, (bar_width - 6) * percent / 100, bar_height - 6, m_highlight);
      char progress[96];
      std::snprintf(progress, sizeof(progress), "%d%%    %.1f / %.1f MiB", percent,
                    snapshot.downloaded / (1024.0 * 1024.0), total / (1024.0 * 1024.0));
      DrawTextCentered(m_font_small, m_width / 2, bar_y + 30, progress, m_dim);
      const std::array<std::pair<std::string_view, std::string_view>, 1> controls = {
          std::pair{"B", "Cancel"}};
      DrawFooter(controls, panel_y + panel_height - 25);
    }
    else
    {
      std::vector<std::pair<std::string_view, std::string_view>> controls = {
          {"B", "Back"}, {"Up / Down", "Scroll"}, {"L", "Page"}, {"R", "Page"}};
      if (snapshot.state == Updater::State::UpdateAvailable)
        controls = {{"A", "Download"}, {"B", "Back"}, {"Up / Down", "Scroll"}};
      else if (snapshot.state == Updater::State::ReadyToInstall)
        controls = {{"A", "Install & Exit"}, {"B", "Back"}};
      else if (snapshot.state == Updater::State::Error ||
               snapshot.state == Updater::State::Cancelled ||
               snapshot.state == Updater::State::UpToDate)
        controls = {{"A", "Check again"}, {"B", "Back"}};
      DrawFooter(controls, panel_y + panel_height - 35);
    }
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

void Launcher::PollUpdateNotification()
{
  const Updater::Snapshot snapshot = Updater::GetSnapshot();
  if (snapshot.state == Updater::State::UpdateAvailable && !snapshot.release.tag.empty() &&
      snapshot.release.tag != m_update_notified_tag)
  {
    m_update_notified_tag = snapshot.release.tag;
    m_update_notice_tag = snapshot.release.tag;
    m_update_notice_until = SDL_GetTicks() + 9000;
  }
}

void Launcher::DrawUpdateNotification()
{
  if (m_update_notice_tag.empty() || SDL_TICKS_PASSED(SDL_GetTicks(), m_update_notice_until))
  {
    m_update_notice_tag.clear();
    m_update_notice_until = 0;
    return;
  }
  const int width = std::min(540, m_width - 40);
  constexpr int height = 92;
  const int x = m_width - width - 24;
  const int y = m_height - height - 58;
  GlassPanel(x, y, width, height);
  Border(x, y, width, height, 2, m_selection);
  const std::string title = "Dolphin " + m_update_notice_tag + " " +
                            std::string(m_localization.Translate("is available"));
  DrawText(m_font, x + 22, y + 16, Ellipsize(m_font, title, width - 44), m_value);
  DrawText(m_font_small, x + 22, y + 54,
           m_localization.Translate("Open Settings > Launcher > Check for updates"), m_text);
}

void Launcher::AppearanceSettings()
{
  static constexpr std::array<std::string_view, 5> THEMES = {"XMB (PS3)", "Bubbles", "Glow",
                                                             "Classic", "OLED black"};
  static constexpr std::array<std::string_view, 5> THEME_VALUES = {"xmb", "bubbles", "glow",
                                                                   "classic", "oled"};
  constexpr int option_count = 11;
  constexpr int update_row = option_count;
  constexpr int selection_count = option_count + 1;
  constexpr int row_height = 46;
  constexpr int list_top = 118;
  static int saved_selection = 0;
  static int saved_top = 0;

  const auto rows_provider = [&] {
    int theme_index = 1;
    const auto iterator =
        std::ranges::find(THEME_VALUES, std::string_view(m_store.Get("Launcher/Theme")));
    if (iterator != THEME_VALUES.end())
      theme_index = iterator - THEME_VALUES.begin();
    const bool steamgriddb_key_set = !Trim(m_store.Get("Network/SteamGridDBKey")).empty();
    return std::array<Row, option_count>{
        Row{"Theme", std::string(THEMES[theme_index])},
        Row{"Language", m_localization.GetDisplayName(), true, false, true, true, false},
        Row{"Games per row", std::to_string(m_grid_columns)},
        Row{"Rows per page", std::to_string(m_grid_rows)},
        Row{"Show game titles", m_show_titles ? "On" : "Off"},
        Row{"Show region flags", m_show_region_flags ? "On" : "Off"},
        Row{"Show custom settings badges", m_show_custom_settings_badges ? "On" : "Off"},
        Row{"UI animations", m_animations ? "On" : "Off"},
        Row{"Sound effects", m_store.GetBool("Launcher/Sounds", true) ? "On" : "Off"},
        Row{"Check updates at boot",
            m_store.GetBool("Launcher/CheckUpdatesAtBoot", true) ? "On" : "Off"},
        Row{"SteamGridDB API key", steamgriddb_key_set ? "Configured" : "Not set", true, false,
            false},
    };
  };

  const auto apply_option = [&](int index, int delta) {
    if (index == 0)
    {
      int current = 1;
      const auto iterator =
          std::ranges::find(THEME_VALUES, std::string_view(m_store.Get("Launcher/Theme")));
      if (iterator != THEME_VALUES.end())
        current = iterator - THEME_VALUES.begin();
      current = SelectChoice("Theme", THEMES, current, delta);
      m_store.Set("Launcher/Theme", std::string(THEME_VALUES[current]));
    }
    else if (index == 1)
    {
      const auto languages = Localization::GetLanguages();
      std::vector<std::string_view> names;
      names.reserve(languages.size());
      for (const LauncherLanguage& language : languages)
        names.push_back(language.name);
      int current = Localization::FindLanguage(m_store.Get("Launcher/Language", "system"));
      current = SelectChoice("Language", names, current, delta);
      m_store.Set("Launcher/Language", std::string(languages[current].code));
      m_localization.SetLanguage(languages[current].code);
      if (!LoadFonts())
      {
        m_store.Set("Launcher/Language", "en");
        m_localization.SetLanguage("en");
        (void)LoadFonts();
        Toast("The selected language font could not be loaded", 1600);
      }
    }
    else if (index == 2)
    {
      int value = std::clamp(m_grid_columns, 3, 8);
      if (delta == 0)
      {
        const int selected =
            Dropdown("Games per row", {"3", "4", "5", "6", "7", "8"}, value - 3, true, false);
        value = selected + 3;
      }
      else
      {
        value = std::clamp(value + (delta < 0 ? -1 : 1), 3, 8);
      }
      m_store.SetInt("Launcher/GridColumns", value);
    }
    else if (index == 3)
    {
      int value = std::clamp(m_grid_rows, 1, 3);
      if (delta == 0)
        value = Dropdown("Rows per page", {"1", "2", "3"}, value - 1, true, false) + 1;
      else
        value = std::clamp(value + (delta < 0 ? -1 : 1), 1, 3);
      m_store.SetInt("Launcher/GridRows", value);
    }
    else if (index == 4)
    {
      m_store.SetBool("Launcher/ShowTitles", !m_show_titles);
    }
    else if (index == 5)
    {
      m_store.SetBool("Launcher/ShowRegionFlags", !m_show_region_flags);
    }
    else if (index == 6)
    {
      m_store.SetBool("Launcher/ShowCustomSettingsBadges", !m_show_custom_settings_badges);
    }
    else if (index == 7)
    {
      m_store.SetBool("Launcher/Animations", !m_animations);
    }
    else if (index == 8)
    {
      const bool enabled = !m_store.GetBool("Launcher/Sounds", true);
      m_store.SetBool("Launcher/Sounds", enabled);
      SetUiAudioEnabled(enabled);
    }
    else if (index == 9)
    {
      m_store.SetBool("Launcher/CheckUpdatesAtBoot",
                      !m_store.GetBool("Launcher/CheckUpdatesAtBoot", true));
    }
    else if (index == 10)
    {
      std::string api_key = m_store.Get("Network/SteamGridDBKey");
      if (PromptText("SteamGridDB API key", api_key, &api_key, true, true,
                     "Used for cover and shortcut artwork downloads.",
                     "Leave blank to remove the saved key"))
      {
        api_key = Trim(std::move(api_key));
        m_store.Set("Network/SteamGridDBKey", api_key);
        MarkStoreDirty();
        FlushPendingSaves();
        Toast(api_key.empty() ? "SteamGridDB API key removed" : "SteamGridDB API key updated",
              1100);
      }
      return;
    }
    MarkStoreDirty();
    ApplyAppearance();
  };

  const auto reset_option = [&](int index) {
    switch (index)
    {
    case 0:
      m_store.Set("Launcher/Theme", "bubbles");
      break;
    case 1:
      m_store.Set("Launcher/Language", "system");
      m_localization.SetLanguage("system");
      if (!LoadFonts())
      {
        m_store.Set("Launcher/Language", "en");
        m_localization.SetLanguage("en");
        (void)LoadFonts();
      }
      break;
    case 2:
      m_store.SetInt("Launcher/GridColumns", 5);
      break;
    case 3:
      m_store.SetInt("Launcher/GridRows", 2);
      break;
    case 4:
      m_store.SetBool("Launcher/ShowTitles", true);
      break;
    case 5:
      m_store.SetBool("Launcher/ShowRegionFlags", true);
      break;
    case 6:
      m_store.SetBool("Launcher/ShowCustomSettingsBadges", true);
      break;
    case 7:
      m_store.SetBool("Launcher/Animations", true);
      break;
    case 8:
      m_store.SetBool("Launcher/Sounds", true);
      SetUiAudioEnabled(true);
      break;
    case 9:
      m_store.SetBool("Launcher/CheckUpdatesAtBoot", true);
      break;
    case 10:
      m_store.Set("Network/SteamGridDBKey", "");
      break;
    default:
      return;
    }
    MarkStoreDirty();
    ApplyAppearance();
    Toast("Setting reset to default", 550);
    BeginScreenFx();
  };

  int selection = std::clamp(saved_selection, 0, selection_count - 1);
  int top = std::max(0, saved_top);
  const auto finish = [&] {
    saved_selection = selection;
    saved_top = top;
    FlushPendingSaves();
  };

  BeginScreenFx();
  while (BeginFrame())
  {
    const auto rows = rows_provider();
    const int visible =
        std::min(option_count, std::max(1, (m_height - list_top - 190) / row_height));
    top = std::clamp(top, 0, std::max(0, option_count - visible));
    const int column_width = std::min(980, m_width - 180);
    const int column_x = (m_width - column_width) / 2;
    const int button_width = std::min(500, m_width - 80);
    constexpr int button_height = 58;
    const int button_x = (m_width - button_width) / 2;
    const int button_y =
        std::min(m_height - button_height - 104, list_top + visible * row_height + 24);

    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, option_count, visible))
        continue;
      if ((touch == TouchKind::SwipeLeft || touch == TouchKind::SwipeRight) &&
          selection < option_count && rows[selection].adjustable)
      {
        apply_option(selection, touch == TouchKind::SwipeLeft ? -1 : 1);
        continue;
      }
      if (touch == TouchKind::Tap)
      {
        if (touch_y < (m_width >= 1600 ? 112 : 80) || touch_y >= m_height - 40)
        {
          finish();
          return;
        }
        if (touch_x >= button_x && touch_x < button_x + button_width && touch_y >= button_y &&
            touch_y < button_y + button_height)
        {
          selection = update_row;
          UpdateScreen();
          BeginScreenFx();
          continue;
        }
        if (touch_x >= column_x && touch_x < column_x + column_width && touch_y >= list_top &&
            touch_y < list_top + visible * row_height)
        {
          const int index = top + (touch_y - list_top) / row_height;
          if (index >= 0 && index < option_count)
          {
            selection = index;
            apply_option(index, 0);
          }
        }
        continue;
      }

      const int direction = EventNavigation(event);
      if (direction != 0)
        selection = (selection + direction + selection_count) % selection_count;

      const bool controller = event.type == SDL_CONTROLLERBUTTONDOWN;
      const bool keyboard = event.type == SDL_KEYDOWN;
      const int button = controller ? event.cbutton.button : -1;
      const SDL_Keycode key = keyboard ? event.key.keysym.sym : SDLK_UNKNOWN;
      if ((button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || key == SDLK_LEFT) &&
          selection < option_count && rows[selection].adjustable)
      {
        apply_option(selection, -1);
      }
      else if ((button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT || key == SDLK_RIGHT) &&
               selection < option_count && rows[selection].adjustable)
      {
        apply_option(selection, 1);
      }
      else if ((button == BUTTON_SETTINGS || key == SDLK_x) && selection < option_count)
      {
        const SettingHelpInfo info = SettingHelpFor("Launcher", rows[selection]);
        const std::string_view current = rows[selection].value == ">" ?
                                             std::string_view{} :
                                             std::string_view(rows[selection].value);
        ShowInfoCard("Launcher", rows[selection].label, info.kind, info.description, current,
                     SettingScope("Launcher", {}), rows[selection].localize_label,
                     rows[selection].localize_value);
        BeginScreenFx();
      }
      else if ((button == SDL_CONTROLLER_BUTTON_X || key == SDLK_y || key == SDLK_DELETE) &&
               selection < option_count)
      {
        reset_option(selection);
      }
      else if (button == BUTTON_CONFIRM || key == SDLK_RETURN)
      {
        if (selection == update_row)
        {
          UpdateScreen();
          BeginScreenFx();
        }
        else
        {
          apply_option(selection, 0);
        }
      }
      else if (button == BUTTON_CANCEL || key == SDLK_ESCAPE)
      {
        finish();
        return;
      }
    }

    if (selection < option_count)
    {
      if (selection < top)
        top = selection;
      if (selection >= top + visible)
        top = selection - visible + 1;
    }

    ClearBackground();
    DrawHeader("Launcher", {});
    const int label_x = column_x + 40;
    const int value_x = column_x + column_width - 40;
    GlassPanel(column_x - 12, list_top - 10, column_width + 24, visible * row_height + 18);
    if (selection < option_count)
    {
      const float target_y = static_cast<float>(list_top + (selection - top) * row_height + 1);
      m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                          target_y :
                          m_highlight_y + (target_y - m_highlight_y) * 0.30f;
      FillRect(column_x, static_cast<int>(m_highlight_y), column_width, row_height - 2, m_focus);
      FillRect(column_x, static_cast<int>(m_highlight_y), 5, row_height - 2, m_selection);
    }
    for (int row = 0; row < visible && top + row < option_count; ++row)
    {
      const int index = top + row;
      const int slot_y = list_top + row * row_height;
      const int y = slot_y + (row_height - TTF_FontHeight(m_font)) / 2;
      const bool current = index == selection;
      const std::string_view displayed_label = rows[index].localize_label ?
                                                   m_localization.Translate(rows[index].label) :
                                                   std::string_view(rows[index].label);
      const std::string_view displayed_value = rows[index].localize_value ?
                                                   m_localization.Translate(rows[index].value) :
                                                   std::string_view(rows[index].value);
      DrawText(m_font, label_x, y, Ellipsize(m_font, displayed_label, column_width * 2 / 3),
               current ? m_value : m_text);
      DrawTextRight(
          m_font_small, value_x, y + (TTF_FontHeight(m_font) - TTF_FontHeight(m_font_small)) / 2,
          Ellipsize(m_font_small, displayed_value, column_width / 3), current ? m_value : m_dim);
    }
    if (option_count > visible)
    {
      const int track_height = visible * row_height;
      const int track_x = column_x + column_width + 16;
      const int track_y = list_top - 2;
      FillRect(track_x, track_y, 4, track_height, SDL_Color{40, 44, 54, 255});
      const int thumb_height = std::max(16, track_height * visible / option_count);
      FillRect(track_x,
               track_y + (track_height - thumb_height) * top / std::max(1, option_count - visible),
               4, thumb_height, m_selection);
    }

    const bool update_selected = selection == update_row;
    FillRect(button_x, button_y, button_width, button_height,
             update_selected ? m_focus : SDL_Color{35, 40, 50, 225});
    Border(button_x, button_y, button_width, button_height, 2,
           update_selected ? m_selection : m_dim);
    DrawTextCentered(m_font, m_width / 2, button_y + (button_height - TTF_FontHeight(m_font)) / 2,
                     m_localization.Translate("Check for Updates"),
                     update_selected ? m_value : m_text);
    DrawTextCentered(m_font_small, m_width / 2, button_y + button_height + 8,
                     Ellipsize(m_font_small, UpdateStatusText(), std::min(m_width - 80, 720)),
                     update_selected ? m_value : m_dim);
    DrawSettingsFooter(
        "Left / Right  Change       A  Choose       X  Info       Y  Reset       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  finish();
}

void Launcher::GameSourcesScreen()
{
  int selection = 0;
  int top = 0;
  constexpr int row_height = 50;
  constexpr int list_y = 112;
  const auto identity = [](const std::string& path) { return Lower(NormalizePath(path)); };
  BeginScreenFx();
  while (BeginFrame())
  {
    const int count = 1 + static_cast<int>(m_sources.size());
    const int visible = std::max(1, (m_height - 176) / row_height);
    selection = std::clamp(selection, 0, count - 1);
    if (selection < top)
      top = selection;
    if (selection >= top + visible)
      top = selection - visible + 1;
    bool rebuild = false;
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, count, visible))
        continue;
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 48)
          return;
        for (int row = 0; row < visible && top + row < count; ++row)
        {
          const int y = list_y + row * row_height;
          if (touch_y >= y && touch_y < y + 46)
          {
            selection = top + row;
            SDL_Event press{};
            press.type = SDL_CONTROLLERBUTTONDOWN;
            press.cbutton.button = BUTTON_CONFIRM;
            SDL_PushEvent(&press);
            break;
          }
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      const bool confirm =
          (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CONFIRM) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RETURN);
      const bool cancel =
          (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CANCEL) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE);
      if (cancel)
        return;
      if (!confirm)
        continue;
      if (selection == 0)
      {
        if (m_sources.size() >= 16)
        {
          Toast("Maximum of 16 game folders", 1000);
          continue;
        }
        const std::string selected = FileBrowser({}, true, false, false);
        if (!selected.empty())
        {
          const std::string selected_identity = identity(selected);
          if (std::ranges::any_of(m_sources, [&](const std::string& path) {
                return identity(path) == selected_identity;
              }))
          {
            Toast("Folder already added", 900);
          }
          else
          {
            EnsureSourceMountedAtStartup(selected);
            m_sources.push_back(NormalizePath(selected));
            SaveSources();
            FlushPendingSaves();
            selection = static_cast<int>(m_sources.size());
          }
          rebuild = true;
        }
      }
      else
      {
        std::size_t source_index = static_cast<std::size_t>(selection - 1);
        const int choice =
            Dropdown("Game folder", {"Change folder", "Move up", "Move down", "Remove"}, -1);
        if (choice == 0)
        {
          const std::string selected = FileBrowser(m_sources[source_index], true, false, false);
          if (!selected.empty())
          {
            const std::string selected_identity = identity(selected);
            bool duplicate = false;
            for (std::size_t index = 0; index < m_sources.size(); ++index)
            {
              if (index != source_index && identity(m_sources[index]) == selected_identity)
                duplicate = true;
            }
            if (duplicate)
              Toast("Folder already added", 900);
            else
            {
              EnsureSourceMountedAtStartup(selected);
              m_sources[source_index] = NormalizePath(selected);
              SaveSources();
              FlushPendingSaves();
            }
            rebuild = true;
          }
        }
        else if (choice == 1 && source_index > 0)
        {
          std::swap(m_sources[source_index], m_sources[source_index - 1]);
          --selection;
          SaveSources();
          FlushPendingSaves();
          rebuild = true;
        }
        else if (choice == 2 && source_index + 1 < m_sources.size())
        {
          std::swap(m_sources[source_index], m_sources[source_index + 1]);
          ++selection;
          SaveSources();
          FlushPendingSaves();
          rebuild = true;
        }
        else if (choice == 3 &&
                 Confirm("Remove game folder?",
                         std::array<std::string, 3>{
                             m_sources[source_index], "",
                             std::string(m_localization.Translate("No files will be deleted."))}))
        {
          m_sources.erase(m_sources.begin() + source_index);
          selection = std::max(0, selection - 1);
          SaveSources();
          FlushPendingSaves();
          rebuild = true;
        }
      }
      if (rebuild)
        break;
    }
    if (rebuild)
    {
      BeginScreenFx();
      continue;
    }

    ClearBackground();
    DrawText(m_font_large, 64, 34, m_localization.Translate("Game folders"), m_highlight);
    DrawTextRight(m_font_small, m_width - 64, 52,
                  m_localization.Translate("All folders are scanned recursively by Dolphin"),
                  m_dim);
    for (int row = 0; row < visible && top + row < count; ++row)
    {
      const int index = top + row;
      const int y = list_y + row * row_height;
      const bool current = index == selection;
      if (current)
      {
        FillRect(56, y - 3, m_width - 112, 46, m_focus);
        FillRect(56, y - 3, 5, 46, m_selection);
      }
      const std::string label = index == 0 ?
                                    std::string(m_localization.Translate("[ Add game folder ]")) :
                                    m_sources[index - 1];
      DrawText(m_font, 82, y, Ellipsize(m_font, label, m_width - 170),
               current ? m_value : (index == 0 ? m_highlight : m_text));
    }
    DrawSettingsFooter("A  Select       B  Back");
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

bool Launcher::EditSmbShare(Storage::SmbShare* share, bool creating)
{
  if (!share)
    return false;
  Storage::SmbShare edited = *share;
  constexpr int field_count = 7;
  constexpr int save_row = 7;
  constexpr int total_rows = 8;
  int selection = 0;
  bool done = false;
  bool saved = false;

  const auto clean_server = [&] {
    edited.server = Trim(edited.server);
    if (Lower(edited.server).starts_with("smb://"))
      edited.server.erase(0, 6);
    while (!edited.server.empty() && edited.server.back() == '/')
      edited.server.pop_back();
  };
  const auto clean_share = [&] {
    std::string combined = Trim(edited.share);
    if (!edited.path.empty())
      combined += "/" + edited.path;
    std::ranges::replace(combined, '\\', '/');
    while (!combined.empty() && combined.front() == '/')
      combined.erase(combined.begin());
    while (!combined.empty() && combined.back() == '/')
      combined.pop_back();
    std::string normalized;
    bool slash = false;
    for (const char value : combined)
    {
      if (value == '/')
      {
        if (slash)
          continue;
        slash = true;
      }
      else
      {
        slash = false;
      }
      normalized += value;
    }
    const std::size_t separator = normalized.find('/');
    edited.share = Trim(normalized.substr(0, separator));
    edited.path =
        separator == std::string::npos ? std::string{} : Trim(normalized.substr(separator + 1));
  };
  const auto shared_folder = [&] {
    return edited.path.empty() ? edited.share : edited.share + "/" + edited.path;
  };
  const auto validate = [&] {
    edited.name = Trim(edited.name);
    clean_server();
    clean_share();
    if (edited.name.empty())
    {
      RenderMessage(
          "Display name required",
          std::array<std::string, 1>{"Enter a name used to identify this share in Dolphin."}, true);
      return false;
    }
    if (edited.server.empty() || edited.server.find('/') != std::string::npos ||
        edited.server.find('\\') != std::string::npos)
    {
      RenderMessage("Invalid SMB server",
                    std::array<std::string, 2>{"Enter only a host name or IP address.",
                                               "Example: 192.168.1.20"},
                    true);
      return false;
    }
    bool invalid_path = edited.share.empty() || edited.share.find(':') != std::string::npos;
    std::size_t start = 0;
    while (!invalid_path && start <= edited.path.size())
    {
      const std::size_t slash = edited.path.find('/', start);
      const std::string component = Trim(edited.path.substr(
          start, slash == std::string::npos ? std::string::npos : slash - start));
      if ((!edited.path.empty() && component.empty()) || component == "." || component == ".." ||
          component.find(':') != std::string::npos)
        invalid_path = true;
      if (slash == std::string::npos)
        break;
      start = slash + 1;
    }
    if (invalid_path)
    {
      RenderMessage(
          "Invalid SMB share",
          std::array<std::string, 2>{"Enter a share name, optionally followed by folders.",
                                     "Do not include a drive letter or smb:// prefix."},
          true);
      return false;
    }
    return true;
  };
  const auto edit_field = [&](int index) {
    std::string value;
    bool accepted = false;
    if (index == 0)
      accepted = PromptText("SMB display name", edited.name, &value, false, false,
                            "Friendly name shown in the Dolphin file browser.",
                            "Example: Living room NAS");
    else if (index == 1)
      accepted = PromptText("Server or IP address", edited.server, &value, false, false,
                            "Host only; do not include smb:// or a folder.",
                            "Example: 192.168.1.20 or NAS.local");
    else if (index == 2)
      accepted = PromptText("Shared folder", shared_folder(), &value, false, false,
                            "Enter the share and an optional folder path inside it.",
                            "Nested folders are supported");
    else if (index == 3)
      accepted = PromptText("Username", edited.user, &value, false, true,
                            "Leave blank for guest access.", "Leave blank for guest");
    else if (index == 4)
      accepted = PromptText("Password", edited.password, &value, true, true,
                            "Stored in launcher.ini; leave blank when not required.",
                            "Leave blank when no password is required");
    else if (index == 5)
      accepted =
          PromptText("Workgroup", edited.domain, &value, false, true,
                     "Usually optional on a home network.", "Example: WORKGROUP, or leave blank");
    if (!accepted)
      return;
    if (index == 0)
      edited.name = value;
    else if (index == 1)
    {
      edited.server = value;
      clean_server();
    }
    else if (index == 2)
    {
      edited.share = value;
      edited.path.clear();
      clean_share();
    }
    else if (index == 3)
      edited.user = value;
    else if (index == 4)
      edited.password = value;
    else if (index == 5)
      edited.domain = value;
    BeginScreenFx();
  };
  const auto activate = [&] {
    if (selection < 6)
    {
      edit_field(selection);
    }
    else if (selection == 6)
    {
      edited.auto_mount = !edited.auto_mount;
    }
    else if (validate())
    {
      if (creating || edited.id.empty())
      {
        std::unordered_set<std::string> ids;
        for (const Storage::SmbShare& existing : m_shares)
          ids.insert(existing.id);
        std::uint64_t seed = armGetSystemTick();
        do
        {
          char id[17];
          std::snprintf(id, sizeof(id), "%08llx",
                        static_cast<unsigned long long>(seed & 0xffffffffULL));
          edited.id = id;
          seed = seed * 6364136223846793005ULL + 1;
        } while (ids.contains(edited.id));
      }
      *share = std::move(edited);
      saved = true;
      done = true;
    }
  };

  BeginScreenFx();
  while (!done && BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      const int scale = m_width >= 1600 ? 3 : 2;
      const int row_height = 27 * scale;
      const int y0 = (m_width >= 1600 ? 112 : 80) + 26;
      const int margin = m_width >= 1600 ? 90 : 56;
      const int help_width = m_width >= 1600 ? 570 : 420;
      const int gap = m_width >= 1600 ? 44 : 28;
      const int form_width = m_width - margin * 2 - help_width - gap;
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 42)
        {
          done = true;
          continue;
        }
        for (int index = 0; index < field_count; ++index)
        {
          if (touch_x >= margin && touch_x < margin + form_width &&
              touch_y >= y0 + index * row_height && touch_y < y0 + (index + 1) * row_height)
          {
            selection = index;
            activate();
            break;
          }
        }
        const int button_y = y0 + field_count * row_height + 10;
        if (touch_x >= margin && touch_x < margin + form_width && touch_y >= button_y &&
            touch_y < button_y + row_height)
        {
          selection = save_row;
          activate();
        }
        continue;
      }
      if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_UP)
          selection = (selection + total_rows - 1) % total_rows;
        else if (event.key.keysym.sym == SDLK_DOWN)
          selection = (selection + 1) % total_rows;
        else if ((event.key.keysym.sym == SDLK_LEFT || event.key.keysym.sym == SDLK_RIGHT) &&
                 selection == 6)
          edited.auto_mount = !edited.auto_mount;
        else if (event.key.keysym.sym == SDLK_RETURN)
          activate();
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          done = true;
      }
      if (event.type != SDL_CONTROLLERBUTTONDOWN)
        continue;
      if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        selection = (selection + total_rows - 1) % total_rows;
      else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        selection = (selection + 1) % total_rows;
      else if ((event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
                event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) &&
               selection == 6)
        edited.auto_mount = !edited.auto_mount;
      else if (event.cbutton.button == BUTTON_CONFIRM)
        activate();
      else if (event.cbutton.button == BUTTON_CANCEL)
        done = true;
    }

    ClearBackground();
    DrawHeader(creating ? "Add SMB network share" : "Edit SMB network share", edited.name);
    const int scale = m_width >= 1600 ? 3 : 2;
    const int row_height = 27 * scale;
    const int y0 = (m_width >= 1600 ? 112 : 80) + 26;
    const int margin = m_width >= 1600 ? 90 : 56;
    const int help_width = m_width >= 1600 ? 570 : 420;
    const int gap = m_width >= 1600 ? 44 : 28;
    const int form_width = m_width - margin * 2 - help_width - gap;
    const int help_x = margin + form_width + gap;
    const int panel_height = field_count * row_height + row_height + 30;
    GlassPanel(margin, y0 - 10, form_width, panel_height);
    GlassPanel(help_x, y0 - 10, help_width, panel_height);
    static constexpr std::array<std::string_view, field_count> labels = {
        "Display name", "Server / IP address", "Shared folder",     "Username",
        "Password",     "Workgroup",           "Connect at startup"};
    const std::string password =
        edited.password.empty() ?
            std::string(m_localization.Translate("Not set")) :
            std::string(std::min<std::size_t>(16, edited.password.size()), '*');
    const std::array<std::string, field_count> values = {
        edited.name.empty() ? std::string(m_localization.Translate("Not set")) : edited.name,
        edited.server.empty() ? std::string(m_localization.Translate("Not set")) : edited.server,
        edited.share.empty() ? std::string(m_localization.Translate("Not set")) : shared_folder(),
        edited.user.empty() ? std::string(m_localization.Translate("Guest")) : edited.user,
        password,
        edited.domain.empty() ? std::string(m_localization.Translate("Optional")) : edited.domain,
        std::string(m_localization.Translate(edited.auto_mount ? "On" : "Off"))};
    for (int index = 0; index < field_count; ++index)
    {
      const int y = y0 + index * row_height;
      const bool current = selection == index;
      if (current)
      {
        FillRect(margin + 8, y, form_width - 16, row_height - 2, m_focus);
        FillRect(margin + 8, y, 5, row_height - 2, m_selection);
      }
      DrawText(m_font_small, margin + 30, y + (row_height - TTF_FontHeight(m_font_small)) / 2,
               m_localization.Translate(labels[index]), current ? m_value : m_dim);
      DrawScrollingTextRight(m_font, margin + form_width - 24,
                             y + (row_height - TTF_FontHeight(m_font)) / 2, form_width / 2 - 30,
                             values[index], current ? m_value : m_text);
    }
    const int button_y = y0 + field_count * row_height + 10;
    const bool button_selected = selection == save_row;
    FillRect(margin + 14, button_y, form_width - 28, row_height - 4,
             button_selected ? m_focus : m_card);
    if (button_selected)
      Border(margin + 14, button_y, form_width - 28, row_height - 4, 2, m_selection);
    DrawTextCentered(m_font, margin + form_width / 2,
                     button_y + (row_height - TTF_FontHeight(m_font)) / 2 - 2,
                     m_localization.Translate(creating ? "Connect and save" : "Save changes"),
                     button_selected ? m_value : m_highlight);

    static constexpr std::array<std::string_view, total_rows> help_titles = {
        "Display name", "Server / IP address", "Shared folder",      "Username",
        "Password",     "Workgroup",           "Connect at startup", "Save share"};
    static constexpr std::array<std::string_view, total_rows> help_line_1 = {
        "A friendly name shown only in Dolphin.",
        "The host name or IP of your SMB server.",
        "The share name and optional folder path.",
        "Leave blank when the share allows guests.",
        "The password for the selected account.",
        "Usually optional on home networks.",
        "Reconnect this share when the launcher opens.",
        "Validate the fields and connect to the share."};
    static constexpr std::array<std::string_view, total_rows> help_line_2 = {
        "Example: Living room NAS",
        "Example: 192.168.1.20 or NAS.local",
        "Nested folders are supported.",
        "Use the account configured on your NAS or PC.",
        "The value is masked on this screen.",
        "Example: WORKGROUP",
        "Turn this off for manually connected shares.",
        "Connection errors will be shown after saving."};
    DrawText(m_font_large, help_x + 28, y0 + 22, m_localization.Translate(help_titles[selection]),
             m_highlight);
    const int help_line_height = TTF_FontHeight(m_font_small) + 4;
    DrawWrapped(m_font_small, help_x + 28, y0 + 92, help_width - 56, help_line_height, 2,
                m_localization.Translate(help_line_1[selection]), m_text);
    DrawWrapped(m_font_small, help_x + 28, y0 + 156, help_width - 56, help_line_height, 2,
                m_localization.Translate(help_line_2[selection]), m_dim);
    const std::string address =
        "smb://" + (edited.server.empty() ? std::string("server") : edited.server) + "/" +
        (edited.share.empty() ? std::string("share") : shared_folder());
    DrawText(m_font_small, help_x + 28, y0 + 210, m_localization.Translate("Connection preview"),
             m_dim);
    DrawScrollingTextLeft(m_font, help_x + 28, y0 + 244, help_width - 56, address, m_value);
    DrawButtonHint(help_x + 28, y0 + panel_height - 67, "A", "Edit / toggle");
    DrawButtonHint(help_x + 28, y0 + panel_height - 33, "B", "Cancel");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  return saved;
}

void Launcher::NetworkSharesScreen()
{
  // Do not let an automatic mount worker replace a devoptab registration while this screen is
  // editing it. The worker is joined once here; individual connect operations below remain
  // asynchronous and keep the UI responsive.
  StopAutoMountShares();
  // Stopping the startup worker here used to permanently abandon every share it had not reached
  // yet.  Re-evaluate the saved auto-mount set on every exit path (including touch/back returns),
  // after any edits made on this screen have been committed.
  Common::ScopeGuard restart_auto_mounts([this] {
    if (!m_shutdown)
      StartAutoMountShares();
  });
  int selection = 0;
  int top = 0;
  constexpr int list_y = 112;
  constexpr int row_height = 60;
  BeginScreenFx();
  while (BeginFrame())
  {
    const int count = 1 + static_cast<int>(m_shares.size());
    const int visible = std::max(1, (m_height - list_y - 58) / row_height);
    selection = std::clamp(selection, 0, count - 1);
    if (selection < top)
      top = selection;
    if (selection >= top + visible)
      top = selection - visible + 1;
    bool rebuild = false;
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, count, visible))
        continue;
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 48)
          return;
        for (int row = 0; row < visible && top + row < count; ++row)
        {
          const int y = list_y + row * row_height;
          if (touch_y >= y && touch_y < y + row_height - 4)
          {
            selection = top + row;
            SDL_Event press{};
            press.type = SDL_CONTROLLERBUTTONDOWN;
            press.cbutton.button = BUTTON_CONFIRM;
            SDL_PushEvent(&press);
            break;
          }
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      const bool confirm =
          (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CONFIRM) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RETURN);
      const bool cancel =
          (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CANCEL) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE);
      if (cancel)
        return;
      if (!confirm)
        continue;
      if (selection == 0)
      {
        if (m_shares.size() >= 8)
        {
          Toast("Maximum of 8 SMB shares", 1000);
          continue;
        }
        Storage::SmbShare new_share;
        if (EditSmbShare(&new_share, true))
        {
          m_shares.push_back(new_share);
          SaveShares();
          FlushPendingSaves();
          std::string error;
          std::atomic_bool cancel_mount{false};
          bool mounted = false;
          RunBusyTask(
              "Connecting SMB share", new_share.name,
              [&] { mounted = Storage::MountSmb(new_share, &error, &cancel_mount); },
              &cancel_mount);
          if (mounted)
          {
            const std::string root = Storage::SmbRootPath(new_share.id);
            for (const std::string& source : m_sources)
              if (PathAtOrBelow(source, root))
                m_pending_scan_sources.push_back(source);
          }
          if (!mounted && !cancel_mount.load(std::memory_order_acquire))
            RenderMessage("SMB connection failed", std::array<std::string, 1>{error});
          selection = static_cast<int>(m_shares.size());
          rebuild = true;
        }
      }
      else
      {
        const int share_index = selection - 1;
        Storage::SmbShare& selected_share = m_shares[share_index];
        const bool mounted = Storage::IsSmbMounted(selected_share.id);
        const int choice = Dropdown(
            selected_share.name.empty() ? selected_share.share : selected_share.name,
            {mounted ? "Disconnect" : "Connect", "Edit", "Toggle connect at startup", "Remove"}, -1,
            false, true);
        if (choice == 0)
        {
          if (mounted)
          {
            StopGameScan();
            Storage::UnmountSmb(selected_share.id);
            m_library_refresh_requested = true;
          }
          else
          {
            std::string error;
            std::atomic_bool cancel_mount{false};
            bool connected = false;
            RunBusyTask(
                "Connecting SMB share", selected_share.name,
                [&] { connected = Storage::MountSmb(selected_share, &error, &cancel_mount); },
                &cancel_mount);
            if (connected)
            {
              const std::string root = Storage::SmbRootPath(selected_share.id);
              for (const std::string& source : m_sources)
                if (PathAtOrBelow(source, root))
                  m_pending_scan_sources.push_back(source);
            }
            if (!connected && !cancel_mount.load(std::memory_order_acquire))
              RenderMessage("SMB connection failed", std::array<std::string, 1>{error});
          }
          rebuild = true;
        }
        else if (choice == 1)
        {
          Storage::SmbShare edited_share = selected_share;
          if (EditSmbShare(&edited_share, false))
          {
            const bool reconnect = mounted || edited_share.auto_mount;
            StopGameScan();
            Storage::UnmountSmb(selected_share.id);
            m_library_refresh_requested = true;
            selected_share = std::move(edited_share);
            SaveShares();
            FlushPendingSaves();
            if (reconnect)
            {
              std::string error;
              std::atomic_bool cancel_mount{false};
              bool connected = false;
              RunBusyTask(
                  "Reconnecting SMB share", selected_share.name,
                  [&] { connected = Storage::MountSmb(selected_share, &error, &cancel_mount); },
                  &cancel_mount);
              if (connected)
              {
                const std::string root = Storage::SmbRootPath(selected_share.id);
                for (const std::string& source : m_sources)
                  if (PathAtOrBelow(source, root))
                    m_pending_scan_sources.push_back(source);
              }
              if (!connected && !cancel_mount.load(std::memory_order_acquire))
                RenderMessage("SMB connection failed", std::array<std::string, 1>{error});
            }
            rebuild = true;
          }
        }
        else if (choice == 2)
        {
          selected_share.auto_mount = !selected_share.auto_mount;
          SaveShares();
          FlushPendingSaves();
          rebuild = true;
        }
        else if (choice == 3 &&
                 Confirm("Remove SMB share?",
                         std::array<std::string, 3>{
                             selected_share.name, "",
                             std::string(m_localization.Translate(
                                 "Saved folders on this share will also be removed."))}))
        {
          const std::string root = Storage::SmbRootPath(selected_share.id);
          StopGameScan();
          Storage::UnmountSmb(selected_share.id);
          m_library_refresh_requested = true;
          m_shares.erase(m_shares.begin() + share_index);
          SaveShares();
          FlushPendingSaves();
          RemoveSavedPathsBelow(root);
          selection = std::max(0, selection - 1);
          rebuild = true;
        }
      }
      if (rebuild)
        break;
    }
    if (rebuild)
    {
      BeginScreenFx();
      continue;
    }

    ClearBackground();
    const std::string summary = std::to_string(m_shares.size()) + " " +
                                std::string(m_localization.Translate(
                                    m_shares.size() == 1 ? "saved share" : "saved shares"));
    DrawHeader("SMB network shares", summary);
    for (int row = 0; row < visible && top + row < count; ++row)
    {
      const int index = top + row;
      const int y = list_y + row * row_height;
      const bool current = index == selection;
      if (current)
      {
        FillRect(56, y - 3, m_width - 112, row_height - 4, m_focus);
        FillRect(56, y - 3, 5, row_height - 4, m_selection);
      }
      if (index == 0)
      {
        DrawText(m_font, 82, y + (row_height - TTF_FontHeight(m_font)) / 2 - 2,
                 m_localization.Translate("[ Add SMB share ]"), current ? m_value : m_highlight);
      }
      else
      {
        const Storage::SmbShare& item = m_shares[index - 1];
        const bool mounted = Storage::IsSmbMounted(item.id);
        DrawText(m_font, 82, y, item.name, current ? m_value : m_text);
        const Storage::SmbConnectionState connection_state =
            Storage::GetSmbConnectionState(item.id);
        const std::string status =
            connection_state == Storage::SmbConnectionState::Connecting   ? "Connecting..." :
            connection_state == Storage::SmbConnectionState::Reconnecting ? "Reconnecting..." :
            connection_state == Storage::SmbConnectionState::Failed       ? "Connection failed" :
            mounted                                                       ? "Connected" :
                      (item.auto_mount ? "Disconnected - auto" : "Disconnected");
        DrawTextRight(m_font_small, m_width - 82, y + 4, m_localization.Translate(status),
                      mounted ? SDL_Color{120, 220, 120, 255} : m_dim);
        const std::string address = "smb://" + item.server + "/" + item.share +
                                    (item.path.empty() ? std::string{} : "/" + item.path);
        DrawText(m_font_small, 82, y + 31, Ellipsize(m_font_small, address, m_width - 340), m_dim);
      }
    }
    DrawSettingsFooter("A  Select       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

bool Launcher::DeleteTree(const std::string& path, const std::atomic_bool* cancel)
{
  if (cancel && cancel->load(std::memory_order_relaxed))
    return false;
  if (IsFilesystemRoot(path))
    return false;
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0)
    return errno == ENOENT;
  if (!S_ISDIR(info.st_mode))
    return std::remove(path.c_str()) == 0;
  DIR* directory = ::opendir(path.c_str());
  if (!directory)
    return false;
  bool ok = true;
  while (dirent* entry = ::readdir(directory))
  {
    if (cancel && cancel->load(std::memory_order_relaxed))
    {
      ok = false;
      break;
    }
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
      continue;
    if (!DeleteTree(JoinPath(path, entry->d_name), cancel))
      ok = false;
  }
  if (::closedir(directory) != 0)
    ok = false;
  return ok && ::rmdir(path.c_str()) == 0;
}

bool Launcher::MeasureTree(const std::string& path, TransferState* state)
{
  if (!state || state->cancelled.load(std::memory_order_relaxed))
    return false;
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0)
  {
    SetTransferDetail(state, {}, "Source is no longer available");
    return false;
  }
  if (S_ISREG(info.st_mode))
  {
    state->total.fetch_add(static_cast<std::uint64_t>(info.st_size), std::memory_order_relaxed);
    return true;
  }
  if (!S_ISDIR(info.st_mode))
  {
    SetTransferDetail(state, {}, "Unsupported file type");
    return false;
  }
  DIR* directory = ::opendir(path.c_str());
  if (!directory)
  {
    SetTransferDetail(state, {}, "Could not open a source folder");
    return false;
  }
  bool ok = true;
  while (ok && !state->cancelled.load(std::memory_order_relaxed))
  {
    dirent* entry = ::readdir(directory);
    if (!entry)
      break;
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
      continue;
    ok = MeasureTree(JoinPath(path, entry->d_name), state);
  }
  if (::closedir(directory) != 0 && ok)
  {
    SetTransferDetail(state, {}, "Could not close a source folder");
    ok = false;
  }
  return ok;
}

bool Launcher::CopyTree(const std::string& source, const std::string& destination,
                        TransferState* state)
{
  struct stat info{};
  if (::lstat(source.c_str(), &info) != 0)
  {
    SetTransferDetail(state, {}, "Source is no longer available");
    return false;
  }
  if (S_ISDIR(info.st_mode))
  {
    if (::mkdir(destination.c_str(), 0777) != 0)
    {
      SetTransferDetail(state, {}, "Could not create a destination folder");
      return false;
    }
    state->destination_created.store(true, std::memory_order_relaxed);
    DIR* directory = ::opendir(source.c_str());
    if (!directory)
    {
      SetTransferDetail(state, {}, "Could not open a source folder");
      return false;
    }
    bool ok = true;
    while (ok && !state->cancelled.load(std::memory_order_relaxed))
    {
      dirent* entry = ::readdir(directory);
      if (!entry)
        break;
      if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
        continue;
      ok = CopyTree(JoinPath(source, entry->d_name), JoinPath(destination, entry->d_name), state);
    }
    if (::closedir(directory) != 0 && ok)
    {
      SetTransferDetail(state, {}, "Could not close a source folder");
      ok = false;
    }
    return ok && !state->cancelled.load(std::memory_order_relaxed);
  }
  if (!S_ISREG(info.st_mode))
  {
    SetTransferDetail(state, {}, "Unsupported file type");
    return false;
  }

  SetTransferDetail(state, FileName(source));
  const std::string partial = destination + ".dolphin-part";
  const std::string backup = destination + ".dolphin-old";
  std::remove(partial.c_str());
  FILE* input = std::fopen(source.c_str(), "rb");
  if (!input)
  {
    SetTransferDetail(state, {}, "Could not open the source file");
    return false;
  }
  FILE* output = std::fopen(partial.c_str(), "wb");
  if (!output)
  {
    std::fclose(input);
    SetTransferDetail(state, {}, "Could not create the destination file");
    return false;
  }
  bool ok = true;
  while (ok && !state->cancelled.load(std::memory_order_relaxed))
  {
    const std::size_t count = std::fread(state->buffer.data(), 1, state->buffer.size(), input);
    if (count != 0)
    {
      if (std::fwrite(state->buffer.data(), 1, count, output) != count)
      {
        SetTransferDetail(state, {}, "Write failed; check free space and permissions");
        ok = false;
        break;
      }
      state->done.fetch_add(count, std::memory_order_relaxed);
    }
    if (count < state->buffer.size())
    {
      if (std::ferror(input))
      {
        SetTransferDetail(state, {}, "Read failed");
        ok = false;
      }
      break;
    }
  }
  if (state->cancelled.load(std::memory_order_relaxed))
    ok = false;
  if (ok && std::fflush(output) != 0)
  {
    SetTransferDetail(state, {}, "Could not flush the destination file");
    ok = false;
  }
  if (ok && ::fsync(::fileno(output)) != 0)
  {
    SetTransferDetail(state, {}, "Could not commit the destination file");
    ok = false;
  }
  if (std::fclose(input) != 0 && ok)
  {
    SetTransferDetail(state, {}, "Could not close the source file");
    ok = false;
  }
  if (std::fclose(output) != 0 && ok)
  {
    SetTransferDetail(state, {}, "Could not close the destination file");
    ok = false;
  }
  if (!ok)
  {
    std::remove(partial.c_str());
    return false;
  }

  struct stat destination_info{};
  const bool existed = ::lstat(destination.c_str(), &destination_info) == 0;
  if (existed)
  {
    struct stat backup_info{};
    if (::lstat(backup.c_str(), &backup_info) == 0)
    {
      SetTransferDetail(state, {}, "A previous backup file blocks this operation");
      std::remove(partial.c_str());
      return false;
    }
    if (std::rename(destination.c_str(), backup.c_str()) != 0)
    {
      SetTransferDetail(state, {}, "Could not preserve the existing destination");
      std::remove(partial.c_str());
      return false;
    }
  }
  if (std::rename(partial.c_str(), destination.c_str()) != 0)
  {
    if (existed)
      std::rename(backup.c_str(), destination.c_str());
    SetTransferDetail(state, {}, "Could not finalize the copied file");
    std::remove(partial.c_str());
    return false;
  }
  if (existed)
    std::remove(backup.c_str());
  return true;
}

bool Launcher::RenderTransfer(TransferState* state)
{
  if (!BeginFrame())
    state->cancelled.store(true, std::memory_order_relaxed);
  SDL_Event event{};
  while (PollEvent(&event))
  {
    int touch_x = 0;
    int touch_y = 0;
    const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
    if ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CANCEL) ||
        (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) ||
        (touch == TouchKind::Tap && touch_y >= m_height - 100))
      state->cancelled.store(true, std::memory_order_relaxed);
  }
  std::string current;
  {
    std::lock_guard lock(state->detail_mutex);
    current = state->current;
  }
  ClearBackground();
  DrawHeader("File transfer", current);
  const int bar_width = m_width * 2 / 3;
  const int bar_x = (m_width - bar_width) / 2;
  const int bar_y = m_height / 2 - 24;
  const int bar_height = m_height >= 1080 ? 50 : 38;
  Border(bar_x, bar_y, bar_width, bar_height, 2, m_selection);
  const std::uint64_t done = state->done.load(std::memory_order_relaxed);
  const std::uint64_t total = state->total.load(std::memory_order_relaxed);
  const std::uint64_t progress = total ? std::min(done, total) : 0;
  const int fill =
      total ? static_cast<int>((bar_width - 6) * (static_cast<long double>(progress) / total)) : 0;
  FillRect(bar_x + 3, bar_y + 3, fill, bar_height - 6, m_highlight);
  char text[128];
  const int percent = total ? static_cast<int>(progress * 100 / total) : 0;
  std::snprintf(text, sizeof(text), "%d%%  ·  %.1f / %.1f MiB", percent, done / 1048576.0,
                total / 1048576.0);
  DrawTextCentered(m_font, m_width / 2, bar_y + bar_height + 28, text, m_text);
  if (state->cancelled.load(std::memory_order_relaxed))
  {
    DrawTextCentered(m_font_small, m_width / 2, m_height - (m_height >= 1080 ? 78 : 58),
                     m_localization.Translate("Cancelling..."), m_value);
  }
  else
  {
    const std::array<std::pair<std::string_view, std::string_view>, 1> controls = {
        std::pair{"B", "Cancel"}};
    DrawFooter(controls, m_height - (m_height >= 1080 ? 64 : 44));
  }
  SDL_RenderPresent(m_renderer);
  return !state->cancelled.load(std::memory_order_relaxed);
}

void Launcher::RunBusyTask(std::string_view title, std::string_view detail,
                           const std::function<void()>& task, std::atomic_bool* cancel)
{
  std::atomic<bool> complete{false};
  const std::string owned_title{title};
  const std::string owned_detail{detail};
  (void)appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  BusyTaskThreadContext context{&task, &complete, cancel};
  Thread worker{};
  const Result create_result =
      threadCreate(&worker, BusyTaskThreadEntry, &context, nullptr, BUSY_TASK_STACK_SIZE, 0x2C, -2);
  const bool worker_created = R_SUCCEEDED(create_result);
  const Result start_result = worker_created ? threadStart(&worker) : create_result;
  const bool worker_started = worker_created && R_SUCCEEDED(start_result);
  if (!worker_started)
  {
    if (worker_created)
      (void)threadClose(&worker);
    task();
    complete.store(true, std::memory_order_release);
  }

  while (!complete.load(std::memory_order_acquire))
  {
    const bool can_render = BeginFrame();
    if (!can_render && cancel)
      cancel->store(true, std::memory_order_release);
    if (can_render)
    {
      SDL_Event event{};
      while (PollEvent(&event))
      {
        if (cancel &&
            ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CANCEL) ||
             (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)))
        {
          cancel->store(true, std::memory_order_release);
        }
      }
      ClearBackground();
      DrawHeader(owned_title, owned_detail);
      const int panel_width = std::min(920, m_width - 180);
      const int panel_height = m_height >= 1080 ? 260 : 196;
      const int panel_x = (m_width - panel_width) / 2;
      const int panel_y = (m_height - panel_height) / 2;
      GlassPanel(panel_x, panel_y, panel_width, panel_height);
      const int phase = static_cast<int>((SDL_GetTicks() / 220) % 4);
      std::string message(m_localization.Translate("Working"));
      message.append(static_cast<std::size_t>(phase), '.');
      DrawTextCentered(m_font_large, m_width / 2, panel_y + (m_height >= 1080 ? 62 : 44), message,
                       m_value);
      DrawTextCentered(
          m_font_small, m_width / 2, panel_y + (m_height >= 1080 ? 148 : 112),
          m_localization.Translate(cancel && cancel->load(std::memory_order_acquire) ?
                                       "Cancelling at the next safe point..." :
                                       "Do not remove the active storage device or close Dolphin."),
          m_dim);
      if (cancel && !cancel->load(std::memory_order_acquire))
      {
        const std::array<std::pair<std::string_view, std::string_view>, 1> hints = {
            std::pair{"B", "Cancel"}};
        DrawFooter(hints);
      }
      SDL_RenderPresent(m_renderer);
    }
    WaitForNextFrame();
  }
  if (worker_started)
  {
    (void)threadWaitForExit(&worker);
    (void)threadClose(&worker);
  }
  (void)appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
}

bool Launcher::ExecutePaste(const std::string& folder)
{
  if (m_clipboard_path.empty())
    return false;
  const std::string source_path = m_clipboard_path;
  struct stat source_info{};
  if (::lstat(m_clipboard_path.c_str(), &source_info) != 0)
  {
    RenderMessage("Paste failed",
                  std::array<std::string, 1>{"The copied item is no longer available."}, true);
    m_clipboard_path.clear();
    m_clipboard_move = false;
    return false;
  }
  const std::string destination = JoinPath(folder, FileName(m_clipboard_path));
  if (NormalizePath(destination) == NormalizePath(m_clipboard_path) ||
      (S_ISDIR(source_info.st_mode) && PathAtOrBelow(destination, m_clipboard_path)))
  {
    RenderMessage("Paste failed",
                  std::array<std::string, 1>{"The destination cannot be inside the source."}, true);
    return false;
  }

  struct stat destination_info{};
  const bool destination_exists = ::lstat(destination.c_str(), &destination_info) == 0;
  if (destination_exists && S_ISDIR(source_info.st_mode))
  {
    RenderMessage(
        "Folder already exists",
        std::array<std::string, 2>{std::string(m_localization.Translate(
                                       "Choose another destination or rename the folder.")),
                                   destination});
    return false;
  }
  if (destination_exists && !S_ISREG(destination_info.st_mode))
  {
    RenderMessage("Paste failed",
                  std::array<std::string, 1>{"The destination is not a regular file."}, true);
    return false;
  }
  if (destination_exists &&
      !Confirm("Replace existing file?",
               std::array<std::string, 2>{
                   FileName(destination),
                   std::string(m_localization.Translate("The existing file will be replaced."))}))
    return false;

  if (m_clipboard_move && DeviceName(m_clipboard_path) == DeviceName(destination))
  {
    const std::string backup = destination + ".dolphin-old";
    bool preserved = false;
    if (destination_exists)
    {
      struct stat backup_info{};
      if (::lstat(backup.c_str(), &backup_info) == 0 ||
          std::rename(destination.c_str(), backup.c_str()) != 0)
      {
        RenderMessage("Move failed",
                      std::array<std::string, 1>{"Could not preserve the existing destination."},
                      true);
        return false;
      }
      preserved = true;
    }
    if (std::rename(m_clipboard_path.c_str(), destination.c_str()) == 0)
    {
      if (preserved)
        std::remove(backup.c_str());
      ReplaceSavedPathPrefix(source_path, destination);
      m_clipboard_path.clear();
      m_clipboard_move = false;
      Toast("Move complete", 800);
      return true;
    }
    if (preserved)
      std::rename(backup.c_str(), destination.c_str());
  }

  TransferState state;
  SetTransferDetail(&state, {}, "Measuring source...");
  bool ok = false;
  bool enough_space = true;
  bool cleanup_ok = true;
  bool move_commit_started = false;
  std::atomic<bool> complete{false};
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  std::thread worker([&] {
    ok = MeasureTree(m_clipboard_path, &state);
    if (ok && !state.cancelled.load(std::memory_order_relaxed))
    {
      struct statvfs free_space{};
      const std::uint64_t total = state.total.load(std::memory_order_relaxed);
      if (::statvfs(folder.c_str(), &free_space) == 0 && free_space.f_frsize != 0 &&
          total > static_cast<std::uint64_t>(free_space.f_bavail) * free_space.f_frsize)
      {
        enough_space = false;
        ok = false;
        SetTransferDetail(&state, {}, "The destination does not have enough available space");
      }
    }
    if (ok && !state.cancelled.load(std::memory_order_relaxed))
    {
      SetTransferDetail(&state, FileName(m_clipboard_path));
      ok = CopyTree(m_clipboard_path, destination, &state);
    }
    // A directory destination did not exist before the transfer, so always remove every partial
    // result after failure or cancellation. Do not let the user's cancellation stop cleanup.
    if (!ok && S_ISDIR(source_info.st_mode) &&
        state.destination_created.load(std::memory_order_relaxed))
      cleanup_ok = DeleteTree(destination);
    if (ok && m_clipboard_move)
    {
      // Once the destination is committed, deleting the source is the non-cancellable commit phase
      // of a cross-device move. Stopping halfway would leave an ambiguous partial move.
      move_commit_started = true;
      SetTransferDetail(&state, FileName(m_clipboard_path), "Removing the original...");
      cleanup_ok = DeleteTree(m_clipboard_path);
      if (!cleanup_ok)
        ok = false;
    }
    complete.store(true, std::memory_order_release);
  });
  while (!complete.load(std::memory_order_acquire))
  {
    RenderTransfer(&state);
    WaitForNextFrame();
  }
  worker.join();
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);

  if (!enough_space)
  {
    RenderMessage(
        "Not enough free space",
        std::array<std::string, 1>{"The destination does not have enough available space."}, true);
    return false;
  }

  if (ok && m_clipboard_move)
  {
    ReplaceSavedPathPrefix(source_path, destination);
    m_clipboard_path.clear();
    m_clipboard_move = false;
  }
  else if (m_clipboard_move && move_commit_started && !cleanup_ok)
  {
    RenderMessage(
        "Move incomplete",
        std::array<std::string, 2>{"The copy completed, but the original could not be removed.",
                                   "Review both locations before trying again."},
        true);
  }
  if (ok)
    Toast("Transfer complete", 800);
  else if (state.cancelled.load(std::memory_order_relaxed))
    Toast("Transfer cancelled", 800);
  else
  {
    const std::string error = TransferError(&state);
    RenderMessage("Transfer failed",
                  std::array<std::string, 1>{error.empty() ?
                                                 std::string(m_localization.Translate(
                                                     "The file transfer could not be completed.")) :
                                                 std::string(m_localization.Translate(error))});
  }
  return ok;
}

bool Launcher::RenamePath(const std::string& path)
{
  std::string name;
  if (!PromptText("Rename", FileName(path), &name, false, false))
    return false;
  name = Trim(std::move(name));
  if (!ValidEntryName(name))
  {
    RenderMessage(
        "Invalid name",
        std::array<std::string, 1>{"Names cannot contain /, \\, :, or control characters."}, true);
    return false;
  }
  const std::string destination = JoinPath(ParentPath(path), name);
  struct stat destination_info{};
  if (::lstat(destination.c_str(), &destination_info) == 0)
  {
    RenderMessage("Rename failed",
                  std::array<std::string, 1>{"An item with that name already exists."}, true);
    return false;
  }
  if (std::rename(path.c_str(), destination.c_str()) != 0)
  {
    RenderMessage("Rename failed", std::array<std::string, 1>{std::strerror(errno)});
    return false;
  }
  ReplaceSavedPathPrefix(path, destination);
  Toast("Renamed", 700);
  return true;
}

void Launcher::FileActions(const std::string& path)
{
  const int choice = Dropdown("File options", {"Copy", "Move", "Rename"}, -1);
  if (choice == 0 || choice == 1)
  {
    m_clipboard_path = path;
    m_clipboard_move = choice == 1;
    Toast(choice == 1 ? "Move queued" : "Copied to clipboard", 700);
  }
  else if (choice == 2)
    RenamePath(path);
}

std::string Launcher::GameLocationLabel(const Game& game) const
{
  if (game.installed_nand)
  {
    char location[40];
    std::snprintf(location, sizeof(location), "Wii NAND: %016llx",
                  static_cast<unsigned long long>(game.title_id));
    return location;
  }
  const std::string path = NormalizePath(game.path);
  for (const Storage::SmbShare& share : m_shares)
  {
    const std::string root = NormalizePath(Storage::SmbRootPath(share.id));
    if (root.empty() || !PathAtOrBelow(path, root))
      continue;
    std::string relative = path.substr(std::min(path.size(), root.size()));
    while (!relative.empty() && relative.front() == '/')
      relative.erase(relative.begin());
    std::string address = "SMB: smb://" + share.server + "/" + share.share;
    if (!relative.empty())
      address += "/" + relative;
    return address;
  }
  if (path.starts_with("sdmc:"))
    return "SD: " + path;
  if (Lower(path).starts_with("ums"))
    return "USB: " + path;
  return path;
}

std::string Launcher::FileBrowser(const std::string& start, bool select_folder, bool select_game,
                                  bool manage, std::span<const std::string_view> extensions,
                                  std::string_view selection_title)
{
  const auto ui = [&](std::string_view text) {
    return std::string(m_localization.Translate(text));
  };
  enum class Kind
  {
    UseFolder,
    Parent,
    Paste,
    Directory,
    File,
    Location,
    Smb,
    ManageSmb,
  };
  struct Entry
  {
    std::string label;
    std::string path;
    Kind kind = Kind::File;
    std::string value;
    // Present only for USB roots on the Locations page.  This is the physical/volume identity,
    // never the mutable umsN: alias shown in path.
    std::string usb_id;
  };
  const auto game_extension = [&](std::string_view name) {
    const std::string lower = Lower(std::string(name));
    if (!extensions.empty())
    {
      return std::ranges::any_of(extensions, [&](std::string_view extension) {
        return lower.ends_with(Lower(std::string(extension)));
      });
    }
    static constexpr std::array<std::string_view, 13> EXTENSIONS = {
        ".gcm", ".tgc", ".bin", ".iso", ".ciso", ".gcz", ".wbfs",
        ".wia", ".rvz", ".nfs", ".wad", ".dol",  ".elf"};
    return std::ranges::any_of(
        EXTENSIONS, [&](std::string_view extension) { return lower.ends_with(extension); });
  };
  std::string current = NormalizePath(start);
  if (!current.empty())
  {
    for (const Storage::SmbShare& share : m_shares)
    {
      if (!PathAtOrBelow(current, Storage::SmbRootPath(share.id)) ||
          Storage::IsSmbMounted(share.id))
        continue;
      std::string error;
      std::atomic_bool cancel_mount{false};
      bool mounted = false;
      RunBusyTask(
          "Connecting SMB share", share.name,
          [&] { mounted = Storage::MountSmb(share, &error, &cancel_mount); }, &cancel_mount);
      if (!mounted)
      {
        if (!cancel_mount.load(std::memory_order_acquire))
          RenderMessage("SMB connection failed", std::array<std::string, 2>{share.name, error});
        current.clear();
      }
      break;
    }
  }
  int selection = 0;
  int top = 0;
  constexpr int row_height = 46;
  constexpr int list_top = 112;
  const int visible = std::max(1, (m_height - 178) / row_height);
  std::vector<Entry> entries;
  std::string entries_path;
  std::uint64_t locations_generation = Storage::UsbStatusGeneration();
  bool refresh_entries = true;

  while (BeginFrame())
  {
    const std::uint64_t current_generation = Storage::UsbStatusGeneration();
    if (current != entries_path || (current.empty() && current_generation != locations_generation))
      refresh_entries = true;
    if (refresh_entries)
    {
      entries.clear();
      bool folder_opened = true;
      if (current.empty())
      {
        entries.push_back({ui("SD card"), "sdmc:/", Kind::Location, ui("Internal SD storage")});
        for (const Storage::Location& location : Storage::ListUsbLocations())
        {
          entries.push_back(
              {location.label, location.path, Kind::Location, ui("USB mass storage"), location.id});
        }
        for (const Storage::SmbShare& share : m_shares)
        {
          const bool mounted = Storage::IsSmbMounted(share.id);
          entries.push_back({ui("SMB") + " - " + (share.name.empty() ? share.share : share.name) +
                                 (mounted ? "" : " (" + ui("disconnected") + ")"),
                             Storage::SmbBrowsePath(share), Kind::Smb,
                             ui(mounted ? "SMB · Connected" : "SMB · Connect")});
        }
        entries.push_back(
            {ui("Manage SMB shares"), {}, Kind::ManageSmb, ui("Add / edit / connect")});
      }
      else
      {
        if (select_folder)
          entries.push_back({ui("[ Use this folder ]"), current, Kind::UseFolder, current});
        if (manage && !m_clipboard_path.empty())
          entries.push_back(
              {"[ " + ui("Paste") + " " + ui(m_clipboard_move ? "moved" : "copied") + " " +
                   ui("item here") + " ]",
               current, Kind::Paste,
               ui(m_clipboard_move ? "Move" : "Copy") + " " + FileName(m_clipboard_path)});
        entries.push_back({ui("[ .. locations / parent ]"), ParentPath(current), Kind::Parent,
                           ui("Parent folder")});
        const std::size_t fixed = entries.size();
        DIR* directory = ::opendir(current.c_str());
        if (directory)
        {
          while (dirent* item = ::readdir(directory))
          {
            if (item->d_name[0] == '.' &&
                (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0))
              continue;
            const std::string path = JoinPath(current, item->d_name);
            struct stat info{};
            if (::stat(path.c_str(), &info) != 0)
              continue;
            if (S_ISDIR(info.st_mode))
              entries.push_back(
                  {std::string(item->d_name) + "/", path, Kind::Directory, ui("Folder")});
            else if (!select_folder && (!select_game || game_extension(item->d_name)))
              entries.push_back({item->d_name, path, Kind::File, HumanBytes(info.st_size)});
          }
          ::closedir(directory);
        }
        else
        {
          folder_opened = false;
        }
        std::ranges::sort(entries.begin() + std::min(fixed, entries.size()), entries.end(),
                          [](const Entry& left, const Entry& right) {
                            if ((left.kind == Kind::Directory) != (right.kind == Kind::Directory))
                              return left.kind == Kind::Directory;
                            return Lower(left.label) < Lower(right.label);
                          });
      }
      if (!current.empty() && !folder_opened)
      {
        RenderMessage("Folder unavailable",
                      std::array<std::string, 3>{current, "",
                                                 std::string(m_localization.Translate(
                                                     "The device may be disconnected."))});
        current.clear();
        selection = top = 0;
        entries_path.clear();
        refresh_entries = true;
        continue;
      }
      if (entries.empty())
        entries.push_back({ui("No accessible locations"), {}, Kind::File, {}});
      entries_path = current;
      locations_generation = current_generation;
      refresh_entries = false;
    }
    selection = std::clamp(selection, 0, static_cast<int>(entries.size()) - 1);
    bool rebuild = false;
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, static_cast<int>(entries.size()), visible))
        continue;
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 48)
        {
          if (current.empty())
            return {};
          current = ParentPath(current);
          selection = top = 0;
          rebuild = true;
          break;
        }
        for (int row = 0; row < visible && top + row < static_cast<int>(entries.size()); ++row)
        {
          const int y = list_top + row * row_height;
          if (touch_y >= y && touch_y < y + 42)
          {
            selection = top + row;
            SDL_Event confirm{};
            confirm.type = SDL_CONTROLLERBUTTONDOWN;
            confirm.cbutton.button = BUTTON_CONFIRM;
            SDL_PushEvent(&confirm);
            break;
          }
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + entries.size()) % entries.size();
      if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_KEYDOWN)
      {
        const bool confirm = event.type == SDL_CONTROLLERBUTTONDOWN ?
                                 event.cbutton.button == BUTTON_CONFIRM :
                                 event.key.keysym.sym == SDLK_RETURN;
        const bool cancel = event.type == SDL_CONTROLLERBUTTONDOWN ?
                                event.cbutton.button == BUTTON_CANCEL :
                                event.key.keysym.sym == SDLK_ESCAPE;
        const bool options =
            event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_SETTINGS;
        const bool paste = event.type == SDL_CONTROLLERBUTTONDOWN &&
                           event.cbutton.button == SDL_CONTROLLER_BUTTON_X;
        const bool eject = event.type == SDL_CONTROLLERBUTTONDOWN ?
                               event.cbutton.button == SDL_CONTROLLER_BUTTON_START :
                               event.key.keysym.sym == SDLK_DELETE;
        if (cancel)
        {
          if (!current.empty())
          {
            current = ParentPath(current);
            selection = top = 0;
            rebuild = true;
          }
          else
          {
            return {};
          }
        }
        else if (eject && manage && current.empty() && entries[selection].kind == Kind::Location &&
                 !entries[selection].usb_id.empty())
        {
          if (EjectUsbLocation(entries[selection].usb_id))
          {
            selection = top = 0;
            rebuild = true;
          }
        }
        else if (options && (entries[selection].kind == Kind::Directory ||
                             entries[selection].kind == Kind::File ||
                             entries[selection].kind == Kind::UseFolder))
        {
          if (manage)
            FileActions(entries[selection].path);
          rebuild = true;
        }
        else if (paste && manage && !current.empty() && !m_clipboard_path.empty())
        {
          ExecutePaste(current);
          rebuild = true;
        }
        else if (confirm)
        {
          Entry entry = entries[selection];
          if (entry.kind == Kind::UseFolder)
            return NormalizePath(entry.path);
          if (entry.kind == Kind::Parent)
          {
            current = entry.path;
            selection = top = 0;
            rebuild = true;
          }
          else if (entry.kind == Kind::Directory || entry.kind == Kind::Location)
          {
            current = NormalizePath(entry.path);
            selection = top = 0;
            rebuild = true;
          }
          else if (entry.kind == Kind::Smb)
          {
            const auto iterator =
                std::ranges::find_if(m_shares, [&](const Storage::SmbShare& share) {
                  return Storage::SmbBrowsePath(share) == entry.path;
                });
            if (iterator != m_shares.end() && !Storage::IsSmbMounted(iterator->id))
            {
              std::string error;
              std::atomic_bool cancel_mount{false};
              bool mounted = false;
              RunBusyTask(
                  "Connecting SMB share", iterator->name,
                  [&] { mounted = Storage::MountSmb(*iterator, &error, &cancel_mount); },
                  &cancel_mount);
              if (!mounted)
              {
                if (!cancel_mount.load(std::memory_order_acquire))
                  RenderMessage("SMB connection failed", std::array<std::string, 1>{error});
                rebuild = true;
                continue;
              }
            }
            current = NormalizePath(entry.path);
            selection = top = 0;
            rebuild = true;
          }
          else if (entry.kind == Kind::ManageSmb)
          {
            NetworkSharesScreen();
            rebuild = true;
          }
          else if (entry.kind == Kind::File)
          {
            if (select_game)
              return entry.path;
          }
          else if (entry.kind == Kind::Paste)
          {
            ExecutePaste(current);
            rebuild = true;
          }
        }
      }
      if (rebuild)
        break;
    }
    if (rebuild)
    {
      refresh_entries = true;
      continue;
    }
    if (selection < top)
      top = selection;
    if (selection >= top + visible)
      top = selection - visible + 1;
    ClearBackground();
    const std::string_view screen_title = !selection_title.empty() ? selection_title :
                                          manage                   ? "File manager" :
                                          select_game              ? "Select game" :
                                                                     "Select game folder";
    DrawText(m_font_large, 64, 30, m_localization.Translate(screen_title), m_highlight);
    DrawTextRight(m_font_small, m_width - 64, 48,
                  current.empty() ? std::string(m_localization.Translate("Locations")) :
                                    Ellipsize(m_font_small, current, m_width / 2),
                  m_dim);
    constexpr int x = 54;
    const int width = m_width - 108;
    for (int row = 0; row < visible && top + row < static_cast<int>(entries.size()); ++row)
    {
      const int index = top + row;
      const int y = list_top + row * row_height;
      if (index == selection)
      {
        FillRect(x, y - 3, width, 42, m_focus);
        FillRect(x, y - 3, 5, 42, m_selection);
      }
      const bool action_row =
          entries[index].kind == Kind::UseFolder || entries[index].kind == Kind::Paste;
      const SDL_Color color = action_row                        ? m_highlight :
                              entries[index].kind == Kind::File ? SDL_Color{120, 220, 120, 255} :
                                                                  m_text;
      DrawText(m_font, 80, y, Ellipsize(m_font, entries[index].label, m_width - 180),
               index == selection ? m_value : color);
    }
    const bool usb_root_selected = manage && current.empty() && selection >= 0 &&
                                   selection < static_cast<int>(entries.size()) &&
                                   entries[selection].kind == Kind::Location &&
                                   !entries[selection].usb_id.empty();
    if (manage)
    {
      if (usb_root_selected)
      {
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> hints = {
            std::pair{"A", "Open"}, std::pair{"+", "Safely eject"}, std::pair{"B", "Back"}};
        DrawFooter(hints);
      }
      else if (current.empty())
      {
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> hints = {
            std::pair{"A", "Open"}, std::pair{"B", "Back"}};
        DrawFooter(hints);
      }
      else
      {
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 4> hints = {
            std::pair{"A", "Open"}, std::pair{"X", "Actions"}, std::pair{"Y", "Paste"},
            std::pair{"B", "Back"}};
        DrawFooter(hints);
      }
    }
    else
    {
      DrawSettingsFooter("A  Open / Select       B  Back");
    }
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  return {};
}

void Launcher::FileManager()
{
  FileBrowser({}, false, false, true);
}

void Launcher::LibraryFilterMenu()
{
  std::vector<std::string> choices;
  choices.emplace_back(m_localization.Translate("All games"));
  choices.emplace_back(m_localization.Translate("Favorites"));
  for (const Collection& collection : m_collections)
    choices.push_back(collection.name);
  const int manage_index = static_cast<int>(choices.size());
  choices.emplace_back(m_localization.Translate("Manage collections..."));
  const int search_index = static_cast<int>(choices.size());
  choices.emplace_back(m_localization.Translate("Search..."));
  const int clear_search_index = static_cast<int>(choices.size());
  if (!m_search_query.empty())
    choices.emplace_back(m_localization.Translate("Clear search"));

  int current = 0;
  if (m_active_collection == "favorites")
    current = 1;
  else if (!m_active_collection.empty())
  {
    const auto found = std::ranges::find(m_collections, m_active_collection, &Collection::name);
    if (found != m_collections.end())
      current = 2 + static_cast<int>(found - m_collections.begin());
  }
  const int selected = Dropdown("Library view", choices, current, true, false);
  if (selected < 0)
    return;
  if (selected == 0)
    m_active_collection.clear();
  else if (selected == 1)
    m_active_collection = "favorites";
  else if (selected >= 2 && selected < manage_index)
    m_active_collection = m_collections[selected - 2].name;
  else if (selected == manage_index)
  {
    ManageCollections();
  }
  else if (selected == search_index)
  {
    std::string query;
    if (PromptText("Search games", m_search_query, &query, false, true,
                   "Search title, Game ID, platform, or path"))
      m_search_query = Trim(std::move(query));
  }
  else if (selected == clear_search_index)
  {
    m_search_query.clear();
  }
  RebuildVisibleGames();
}

void Launcher::ManageCollections()
{
  const auto normalize_name = [&](std::string name, std::string_view previous = {}) {
    name = Trim(std::move(name));
    if (name.size() > 64)
      name.resize(64);
    const bool invalid = name.empty() || Lower(name) == "favorites" ||
                         std::ranges::any_of(name, [](unsigned char character) {
                           return character < ' ' || character == ',' || character == '=';
                         });
    const bool duplicate = std::ranges::any_of(m_collections, [&](const Collection& collection) {
      return collection.name != previous && Lower(collection.name) == Lower(name);
    });
    return invalid || duplicate ? std::string{} : name;
  };

  RunRows(
      "Manage collections", {},
      [&] {
        std::vector<Row> rows{{"Create collection...", ">", true, false, false}};
        rows.reserve(m_collections.size() + 1);
        for (const Collection& collection : m_collections)
        {
          rows.push_back({collection.name,
                          std::to_string(collection.members.size()) +
                              (collection.members.size() == 1 ? " game" : " games"),
                          true, false, false, false, false});
        }
        return rows;
      },
      [&](int index, int) {
        if (index == 0)
        {
          std::string entered;
          if (!PromptText("Collection name", {}, &entered, false, false))
            return false;
          entered = normalize_name(std::move(entered));
          if (entered.empty())
          {
            Toast("Invalid or duplicate collection name", 1000);
            return false;
          }
          m_collections.push_back({std::move(entered), {}});
          SaveCollections();
          return false;
        }

        const std::size_t collection_index = static_cast<std::size_t>(index - 1);
        if (collection_index >= m_collections.size())
          return false;
        Collection& collection = m_collections[collection_index];
        const int choice =
            Dropdown(collection.name, {"View collection", "Rename", "Delete"}, -1, false, true);
        if (choice == 0)
        {
          m_active_collection = collection.name;
          return true;
        }
        if (choice == 1)
        {
          const std::string old_name = collection.name;
          std::string entered;
          if (!PromptText("Rename collection", old_name, &entered, false, false))
            return false;
          entered = normalize_name(std::move(entered), old_name);
          if (entered.empty())
          {
            Toast("Invalid or duplicate collection name", 1000);
            return false;
          }
          collection.name = entered;
          if (m_active_collection == old_name)
            m_active_collection = entered;
          SaveCollections();
          return false;
        }
        if (choice == 2 &&
            Confirm("Delete collection?",
                    std::array<std::string, 2>{collection.name,
                                               "Games and save data will not be deleted."}))
        {
          if (m_active_collection == collection.name)
            m_active_collection.clear();
          m_collections.erase(m_collections.begin() + collection_index);
          SaveCollections();
        }
        return false;
      },
      true);
  RebuildVisibleGames();
}

void Launcher::EditGameOrganization(Game* game)
{
  if (!game)
    return;
  RunRows(
      "Favorites & collections", game->title,
      [&] {
        std::vector<Row> rows;
        rows.push_back({"Favorite", m_favorites.contains(game->key) ? "Yes" : "No"});
        rows.push_back({"Create collection...", ">", true, false, false});
        for (const Collection& collection : m_collections)
        {
          rows.push_back({collection.name,
                          collection.members.contains(game->key) ?
                              std::string(m_localization.Translate("Added")) :
                              std::string(m_localization.Translate("Not added")),
                          true, false, true, false});
        }
        return rows;
      },
      [&](int index, int) {
        if (index == 0)
        {
          if (!m_favorites.erase(game->key))
            m_favorites.insert(game->key);
        }
        else if (index == 1)
        {
          std::string name;
          if (!PromptText("Collection name", {}, &name, false, false))
            return false;
          name = Trim(std::move(name));
          if (name.size() > 64)
            name.resize(64);
          if (name.empty() || name == "favorites" ||
              std::ranges::any_of(name, [](unsigned char character) {
                return character < ' ' || character == ',' || character == '=';
              }))
          {
            Toast("Invalid collection name", 1000);
            return false;
          }
          const auto existing = std::ranges::find(m_collections, name, &Collection::name);
          if (existing == m_collections.end())
          {
            Collection collection;
            collection.name = name;
            collection.members.insert(game->key);
            m_collections.emplace_back(std::move(collection));
          }
          else
          {
            existing->members.insert(game->key);
          }
        }
        else
        {
          Collection& collection = m_collections[index - 2];
          if (!collection.members.erase(game->key))
            collection.members.insert(game->key);
        }
        SaveCollections();
        RebuildVisibleGames();
        return false;
      },
      true);
}

void Launcher::DownloadCovers()
{
  const std::string api_key = Trim(m_store.Get("Network/SteamGridDBKey"));
  if (!m_cover_download_ready || api_key.empty())
  {
    RenderMessage("Cover download unavailable",
                  std::array<std::string, 2>{"A SteamGridDB API key is required.",
                                             "Configure it in Settings > Launcher."},
                  true);
    return;
  }
  struct MissingCover
  {
    std::string key;
    std::string title;
    std::string path;
  };
  std::vector<MissingCover> missing;
  for (const Game& game : m_games)
  {
    const std::string path = CoverPath(game);
    if (!RegularFileExists(path))
      missing.push_back({game.key, game.title, path});
  }
  if (missing.empty())
  {
    Toast("Every game already has a cover", 1000);
    return;
  }
  if (!Confirm("Download covers?",
               std::array<std::string, 2>{std::to_string(missing.size()) + " " +
                                              std::string(m_localization.Translate("games")),
                                          std::string(m_localization.Translate(
                                              "Press B while downloading to cancel safely."))}))
    return;

  std::atomic_bool cancel{false};
  std::atomic<int> downloaded{0};
  std::atomic<int> failed{0};
  std::vector<std::string> downloaded_keys;
  downloaded_keys.reserve(missing.size());
  const auto task = [&] {
    CoverDownload::RequestOptions options;
    options.cancel = &cancel;
    for (const MissingCover& item : missing)
    {
      if (cancel.load(std::memory_order_acquire))
        break;
      const CoverDownload::Result result =
          CoverDownload::DownloadBestCover(api_key, item.title, item.path, nullptr, &options);
      if (result == CoverDownload::Result::Ok)
      {
        downloaded.fetch_add(1, std::memory_order_relaxed);
        downloaded_keys.emplace_back(item.key);
      }
      else if (result != CoverDownload::Result::Cancelled)
        failed.fetch_add(1, std::memory_order_relaxed);
    }
  };
  RunBusyTask("Downloading covers", std::to_string(missing.size()) + " games queued", task,
              &cancel);
  const std::unordered_set<std::string> downloaded_set(downloaded_keys.begin(),
                                                       downloaded_keys.end());
  for (Game& game : m_games)
  {
    if (!downloaded_set.contains(game.key))
      continue;
    if (game.cover)
      SDL_DestroyTexture(game.cover);
    game.cover = nullptr;
    game.cover_use = 0;
    game.cover_loaded_at = 0;
    game.cover_attempted = false;
  }
  if (cancel.load(std::memory_order_acquire))
    Toast("Cover download cancelled", 1000);
  else
    RenderMessage("Cover download complete",
                  std::array<std::string, 2>{
                      std::to_string(downloaded.load()) + " " +
                          std::string(m_localization.Translate("downloaded")),
                      std::to_string(failed.load()) + " " +
                          std::string(m_localization.Translate("not found or failed"))});
}

bool Launcher::EjectUsbLocation(std::string_view stable_id)
{
  const std::vector<Storage::Location> locations = Storage::GetUsbSnapshot().locations;
  const auto found = std::ranges::find(locations, stable_id, &Storage::Location::id);
  if (found == locations.end())
  {
    Toast("USB drive is no longer connected", 900);
    return false;
  }
  const Storage::Location location = *found;
  if (!Confirm(
          "Safely eject USB drive?",
          std::array<std::string, 3>{location.label, location.mount_alias,
                                     std::string(m_localization.Translate(
                                         "All partitions on this physical drive will unmount."))}))
    return false;
  StopGameScan();
  std::string error;
  bool ejected = false;
  RunBusyTask("Safely ejecting USB storage", location.label,
              [&] { ejected = Storage::SafelyEjectUsb(location.id, &error); });
  if (ejected)
  {
    m_library_refresh_requested = true;
    Toast("USB drive can now be removed", 1400);
    return true;
  }
  RenderMessage("USB eject failed", std::array<std::string, 1>{error});
  return false;
}

void Launcher::LibrarySettings()
{
  constexpr int row_count = 7;
  constexpr int row_height = 56;
  constexpr int start_y = 110;
  auto& saved = m_row_positions["Library & storage\n"];
  int selection = std::clamp(saved.first, 0, row_count - 1);
  std::size_t installed_count = Tools::ListInstalledWiiTitles().size();
  const auto open_row = [&] {
    if (selection == 0)
      GameSourcesScreen();
    else if (selection == 1)
      FileManager();
    else if (selection == 2)
      NetworkSharesScreen();
    else if (selection == 3)
      DownloadCovers();
    else if (selection == 4)
      SaveDataSettings();
    else if (selection == 5)
      InstalledContentManager();
    else
      InstallWAD();
    installed_count = Tools::ListInstalledWiiTitles().size();
    BeginScreenFx();
  };
  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::Tap)
      {
        if (touch_y < (m_width >= 1600 ? 112 : 80) || touch_y >= m_height - 40)
        {
          saved.first = selection;
          return;
        }
        for (int row = 0; row < row_count; ++row)
        {
          const int y = start_y + row * row_height;
          if (touch_y >= y && touch_y < y + row_height)
          {
            selection = row;
            open_row();
            if (m_pending_launch)
              return;
            break;
          }
        }
        continue;
      }
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + row_count) % row_count;
      if ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CONFIRM) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RETURN))
      {
        open_row();
        if (m_pending_launch)
          return;
      }
      else if ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CANCEL) ||
               (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
      {
        saved.first = selection;
        return;
      }
    }

    ClearBackground();
    DrawHeader("Library & storage");
    const int column_width = std::min(980, m_width - 180);
    const int column_x = (m_width - column_width) / 2;
    const int label_x = column_x + 40;
    const int value_x = column_x + column_width - 40;
    GlassPanel(column_x - 12, start_y - 10, column_width + 24, row_count * row_height + 18);
    const float target = static_cast<float>(start_y + selection * row_height + 2);
    m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                        target :
                        m_highlight_y + (target - m_highlight_y) * 0.30f;
    FillRect(column_x, static_cast<int>(m_highlight_y), column_width, row_height - 4, m_focus);
    FillRect(column_x, static_cast<int>(m_highlight_y), 5, row_height - 4, m_selection);

    const int connected = std::ranges::count_if(
        m_shares, [](const Storage::SmbShare& share) { return Storage::IsSmbMounted(share.id); });
    const std::string folder_value =
        std::to_string(m_sources.size()) + " " +
        std::string(m_localization.Translate(m_sources.size() == 1 ? "folder" : "folders"));
    const std::string save_value(m_localization.Translate("Manage GC/Wii saves"));
    const std::string smb_value = std::to_string(connected) + " / " +
                                  std::to_string(m_shares.size()) + " " +
                                  std::string(m_localization.Translate("connected"));
    const std::string installed_value =
        std::to_string(installed_count) + " " +
        std::string(m_localization.Translate(installed_count == 1 ? "title" : "titles"));
    const std::array<std::string_view, row_count> labels = {
        "Game folders", "File manager",      "SMB network shares", "Download covers",
        "Save data",    "Installed content", "Install WAD"};
    const std::array<std::string, row_count> values = {
        folder_value,
        std::string(m_localization.Translate("SD / USB / SMB")),
        smb_value,
        std::string(m_localization.Translate("SteamGridDB batch")),
        save_value,
        installed_value,
        std::string(m_localization.Translate("Select package or file"))};
    const int font_height = TTF_FontHeight(m_font);
    const int small_height = TTF_FontHeight(m_font_small);
    for (int row = 0; row < row_count; ++row)
    {
      const int slot = start_y + row * row_height;
      const int y = slot + (row_height - font_height) / 2;
      const bool current = row == selection;
      DrawText(m_font, label_x, y, m_localization.Translate(labels[row]),
               current ? m_value : m_text);
      DrawTextRight(m_font_small, value_x, slot + (row_height - small_height) / 2, values[row],
                    current ? m_value : m_dim);
    }
    DrawSettingsFooter("A  Open       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  saved.first = selection;
}

int Launcher::ChooseCoverArtwork(const std::vector<CoverDownload::Artwork>& artwork,
                                 std::string_view game_name)
{
  if (artwork.empty())
    return -1;
  constexpr int list_x = 56;
  const int list_width = m_width / 2 - 78;
  constexpr int row_height = 52;
  constexpr int start_y = 116;
  const int preview_x = m_width / 2 + 28;
  const int preview_area_width = m_width - preview_x - 56;
  const int preview_height = std::min(m_height - 210, m_width >= 1600 ? 720 : 510);
  const int preview_width = preview_height * 2 / 3;
  const int visible = std::max(1, (m_height - start_y - 72) / row_height);
  const std::string temporary = std::string(COVER_DIRECTORY) + "/.sgdb-preview.img";
  int selection = 0;
  int top = 0;
  int loaded = -1;
  SDL_Texture* preview = nullptr;
  bool preview_failed = false;
  const auto release_preview = [&] {
    if (preview)
      SDL_DestroyTexture(preview);
    preview = nullptr;
    std::remove(temporary.c_str());
  };
  const auto load_preview = [&](int index) {
    release_preview();
    loaded = index;
    preview_failed = false;
    ClearBackground();
    DrawHeader("Choose cover artwork", game_name);
    DrawTextCentered(m_font, preview_x + preview_area_width / 2, m_height / 2 - 18,
                     m_localization.Translate("Loading preview..."), m_dim);
    SDL_RenderPresent(m_renderer);
    const std::string& url =
        artwork[index].thumbnail_url.empty() ? artwork[index].url : artwork[index].thumbnail_url;
    std::atomic_bool cancel{false};
    CoverDownload::Result result = CoverDownload::Result::Error;
    CoverDownload::RequestOptions options{&cancel};
    RunBusyTask(
        "Loading cover preview", std::string(game_name),
        [&] { result = CoverDownload::DownloadImage(url, temporary, &options); }, &cancel);
    if (result == CoverDownload::Result::Ok)
      preview = LoadScaledTexture(temporary, preview_width, preview_height);
    preview_failed = preview == nullptr;
    std::remove(temporary.c_str());
    BeginScreenFx();
  };

  EnsureDirectory(COVER_DIRECTORY);
  load_preview(0);
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      const int previous_touch_selection = selection;
      if (TouchScrollList(touch, &selection, &top, static_cast<int>(artwork.size()), visible))
      {
        if (selection != previous_touch_selection)
          load_preview(selection);
        continue;
      }
      if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 48)
        {
          release_preview();
          return -1;
        }
        if (touch_x >= list_x && touch_x < list_x + list_width)
        {
          for (int row = 0; row < visible && top + row < static_cast<int>(artwork.size()); ++row)
          {
            const int y = start_y + row * row_height;
            if (touch_y >= y && touch_y < y + row_height)
            {
              selection = top + row;
              if (loaded != selection)
                load_preview(selection);
              break;
            }
          }
        }
        continue;
      }
      const int previous = selection;
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + static_cast<int>(artwork.size())) % artwork.size();
      if ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CONFIRM) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RETURN))
      {
        release_preview();
        return selection;
      }
      if ((event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == BUTTON_CANCEL) ||
          (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
      {
        release_preview();
        return -1;
      }
      if (selection < top)
        top = selection;
      if (selection >= top + visible)
        top = selection - visible + 1;
      if (selection != previous)
        load_preview(selection);
    }

    ClearBackground();
    DrawHeader("Choose cover artwork", game_name);
    GlassPanel(list_x - 10, start_y - 10, list_width + 20,
               std::min(visible, static_cast<int>(artwork.size())) * row_height + 18);
    for (int row = 0; row < visible && top + row < static_cast<int>(artwork.size()); ++row)
    {
      const int index = top + row;
      const int y = start_y + row * row_height;
      const int text_y = y + (row_height - TTF_FontHeight(m_font)) / 2;
      const bool current = index == selection;
      if (current)
      {
        FillRect(list_x, y, list_width, row_height - 3, m_focus);
        FillRect(list_x, y, 5, row_height - 3, m_selection);
      }
      DrawText(m_font, list_x + 26, text_y,
               std::string(m_localization.Translate("Artwork")) + " " + std::to_string(index + 1),
               current ? m_value : m_text);
      if (artwork[index].width > 0 && artwork[index].height > 0)
      {
        const std::string dimensions =
            std::to_string(artwork[index].width) + "x" + std::to_string(artwork[index].height);
        DrawTextRight(m_font_small, list_x + list_width - 20,
                      text_y + (TTF_FontHeight(m_font) - TTF_FontHeight(m_font_small)) / 2,
                      dimensions, current ? m_value : m_dim);
      }
    }
    const int image_x = preview_x + (preview_area_width - preview_width) / 2;
    const int image_y = start_y;
    FillRect(image_x, image_y, preview_width, preview_height, m_card);
    if (loaded == selection && preview)
    {
      SDL_Rect destination{image_x, image_y, preview_width, preview_height};
      SDL_RenderCopy(m_renderer, preview, nullptr, &destination);
    }
    else if (loaded == selection && preview_failed)
    {
      DrawTextCentered(m_font_small, image_x + preview_width / 2, image_y + preview_height / 2,
                       m_localization.Translate("Preview unavailable"), m_dim);
    }
    Border(image_x, image_y, preview_width, preview_height, 2,
           loaded == selection ? m_selection : m_dim);
    DrawSettingsFooter("A  Use artwork       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  release_preview();
  return -1;
}

void Launcher::DownloadCover(Game* game)
{
  if (!game)
    return;
  if (!m_cover_download_ready)
  {
    RenderMessage("Cover downloads unavailable",
                  std::array<std::string, 2>{"The network or HTTP client could not be initialized.",
                                             "Check the Switch network connection and try again."},
                  true);
    return;
  }
  const auto save_api_key = [&](std::string api_key) {
    api_key = Trim(std::move(api_key));
    m_store.Set("Network/SteamGridDBKey", api_key);
    MarkStoreDirty();
    FlushPendingSaves();
    return api_key;
  };
  std::string api_key = Trim(m_store.Get("Network/SteamGridDBKey"));
  if (api_key.empty())
  {
    if (!PromptText("Enter your free SteamGridDB API key", {}, &api_key, true, false,
                    "Create a free API key at steamgriddb.com/profile/preferences/api"))
    {
      Toast("A SteamGridDB API key is required", 1200);
      return;
    }
    api_key = save_api_key(std::move(api_key));
  }
  std::string query = game->title;
  CoverDownload::GameResult selected_game;
  while (true)
  {
    std::vector<CoverDownload::GameResult> matches;
    std::atomic_bool cancel{false};
    CoverDownload::Result result = CoverDownload::Result::Error;
    CoverDownload::RequestOptions options{&cancel};
    RunBusyTask(
        "Searching SteamGridDB", query,
        [&] { result = CoverDownload::SearchGames(api_key, query, &matches, &options); }, &cancel);
    if (result == CoverDownload::Result::Cancelled)
      return;
    if (result == CoverDownload::Result::NoKey)
    {
      std::string replacement = api_key;
      if (!PromptText("SteamGridDB API key rejected", replacement, &replacement, true, false,
                      "Enter a valid key to retry the cover search.",
                      "steamgriddb.com/profile/preferences/api"))
        return;
      api_key = save_api_key(std::move(replacement));
      continue;
    }
    if (result != CoverDownload::Result::Ok && result != CoverDownload::Result::NotFound)
    {
      RenderMessage("Cover search failed",
                    std::array<std::string, 1>{CoverDownload::ResultMessage(result)});
      return;
    }
    std::vector<std::string> names{std::string(m_localization.Translate("Custom search..."))};
    names.reserve(matches.size() + 1);
    for (const auto& match : matches)
      names.push_back(match.name);
    const int match_index = Dropdown("Choose matching title", names, -1, true, false);
    if (match_index < 0)
      return;
    if (match_index == 0)
    {
      std::string custom;
      if (!PromptText("Custom SteamGridDB search", query, &custom, false, false))
        continue;
      custom = Trim(std::move(custom));
      if (!custom.empty())
        query = std::move(custom);
      continue;
    }
    if (match_index > static_cast<int>(matches.size()))
      return;
    selected_game = matches[match_index - 1];
    break;
  }

  std::vector<CoverDownload::Artwork> artwork;
  std::atomic_bool artwork_cancel{false};
  CoverDownload::Result artwork_result = CoverDownload::Result::Error;
  CoverDownload::RequestOptions artwork_options{&artwork_cancel};
  RunBusyTask(
      "Loading available artwork", selected_game.name,
      [&] {
        artwork_result =
            CoverDownload::FetchArtwork(api_key, selected_game.id, &artwork, &artwork_options);
      },
      &artwork_cancel);
  if (artwork_result == CoverDownload::Result::Cancelled)
    return;
  if (artwork_result != CoverDownload::Result::Ok)
  {
    RenderMessage("Artwork search failed",
                  std::array<std::string, 1>{CoverDownload::ResultMessage(artwork_result)});
    return;
  }
  const int artwork_index = ChooseCoverArtwork(artwork, selected_game.name);
  if (artwork_index < 0 || artwork_index >= static_cast<int>(artwork.size()))
    return;
  std::atomic_bool download_cancel{false};
  CoverDownload::Result download = CoverDownload::Result::Error;
  CoverDownload::RequestOptions download_options{&download_cancel};
  RunBusyTask(
      "Downloading selected cover", selected_game.name,
      [&] {
        download = CoverDownload::DownloadImage(artwork[artwork_index].url, CoverPath(*game),
                                                &download_options);
      },
      &download_cancel);
  if (download == CoverDownload::Result::Cancelled)
    return;
  if (download == CoverDownload::Result::Ok)
  {
    ReloadCover(game);
    Toast("Cover downloaded", 1200);
  }
  else
  {
    RenderMessage("Cover download failed",
                  std::array<std::string, 1>{CoverDownload::ResultMessage(download)});
  }
}

void Launcher::ImportCoverFromFile(Game* game)
{
  if (!game)
    return;

  static constexpr std::array<std::string_view, 5> IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg",
                                                                       ".webp", ".bmp"};
  const std::string selected = FileBrowser(ParentPath(game->path), false, true, false,
                                           IMAGE_EXTENSIONS, "Select local cover");
  if (selected.empty())
    return;

  const std::string destination = CoverPath(*game);
  const std::string temporary = destination + ".tmp";
  const std::string backup = destination + ".old";
  std::atomic_bool cancel{false};
  bool imported = false;
  std::string reason;
  std::string detail;
  RunBusyTask(
      "Importing local cover", FileName(selected),
      [&] {
        const auto fail = [&](std::string_view message, std::string_view technical = {}) {
          reason = std::string(message);
          detail = std::string(technical);
          std::remove(temporary.c_str());
        };
        const auto was_cancelled = [&] {
          if (!cancel.load(std::memory_order_acquire))
            return false;
          std::remove(temporary.c_str());
          return true;
        };

        if (was_cancelled())
          return;
        struct stat source_info{};
        constexpr std::uint64_t MAXIMUM_FILE_SIZE = 32ULL * 1024 * 1024;
        if (::stat(selected.c_str(), &source_info) != 0)
        {
          fail("The selected cover file is unavailable.", std::strerror(errno));
          return;
        }
        if (!S_ISREG(source_info.st_mode))
        {
          fail("The selected cover file is unavailable.");
          return;
        }
        if (source_info.st_size < 1 ||
            static_cast<std::uint64_t>(source_info.st_size) > MAXIMUM_FILE_SIZE)
        {
          fail("The selected cover file is too large.");
          return;
        }
        if (!RecoverAtomicFile(destination))
        {
          fail("Dolphin could not prepare the cover file safely.", std::strerror(errno));
          return;
        }

        using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
        Surface source{IMG_Load(selected.c_str()), SDL_FreeSurface};
        if (!source)
        {
          fail("The selected file is not a supported image.", IMG_GetError());
          return;
        }
        constexpr std::uint64_t MAXIMUM_PIXELS = 16ULL * 1024 * 1024;
        if (source->w <= 0 || source->h <= 0 || source->w > 8192 || source->h > 8192 ||
            static_cast<std::uint64_t>(source->w) * static_cast<std::uint64_t>(source->h) >
                MAXIMUM_PIXELS)
        {
          fail("The selected image dimensions are too large.");
          return;
        }
        if (was_cancelled())
          return;

        Surface converted{SDL_ConvertSurfaceFormat(source.get(), SDL_PIXELFORMAT_RGBA32, 0),
                          SDL_FreeSurface};
        source.reset();
        if (!converted)
        {
          fail("Dolphin could not convert the selected image to PNG.", SDL_GetError());
          return;
        }
        if (was_cancelled())
          return;
        if (IMG_SavePNG(converted.get(), temporary.c_str()) != 0)
        {
          fail("Dolphin could not convert the selected image to PNG.", IMG_GetError());
          return;
        }
        converted.reset();
        if (was_cancelled())
          return;

        // Re-open the generated PNG before replacing the active cover. This catches truncated or
        // unsupported output while the previous cover is still untouched.
        Surface verification{IMG_Load(temporary.c_str()), SDL_FreeSurface};
        if (!verification || verification->w <= 0 || verification->h <= 0)
        {
          fail("Dolphin could not verify the converted cover.", IMG_GetError());
          return;
        }
        verification.reset();
        FILE* saved_file = std::fopen(temporary.c_str(), "rb+");
        if (!saved_file)
        {
          fail("Dolphin could not save the converted cover.", std::strerror(errno));
          return;
        }
        const bool synced = ::fsync(::fileno(saved_file)) == 0;
        const bool closed = std::fclose(saved_file) == 0;
        if (!synced || !closed)
        {
          fail("Dolphin could not save the converted cover.", std::strerror(errno));
          return;
        }
        if (was_cancelled())
          return;

        const bool had_current = RegularFileExists(destination);
        if (had_current && std::rename(destination.c_str(), backup.c_str()) != 0)
        {
          fail("Dolphin could not replace the current cover safely.", std::strerror(errno));
          return;
        }
        if (std::rename(temporary.c_str(), destination.c_str()) != 0)
        {
          const int saved_errno = errno;
          if (had_current)
            std::rename(backup.c_str(), destination.c_str());
          fsdevCommitDevice("sdmc");
          fail("Dolphin could not replace the current cover safely.", std::strerror(saved_errno));
          return;
        }
        fsdevCommitDevice("sdmc");
        if (had_current && std::remove(backup.c_str()) == 0)
          fsdevCommitDevice("sdmc");
        imported = true;
      },
      &cancel);

  if (imported)
  {
    ReloadCover(game);
    Toast("Cover imported", 1200);
    return;
  }
  if (cancel.load(std::memory_order_acquire))
    return;
  std::vector<std::string> lines;
  lines.emplace_back(m_localization.Translate(
      reason.empty() ? "The selected cover could not be imported safely." : reason));
  if (!detail.empty())
    lines.emplace_back(std::move(detail));
  RenderMessage("Cover import failed", lines);
}

void Launcher::CoverSettings(Game* game)
{
  if (!game)
    return;

  const std::string cover_path = CoverPath(*game);
  (void)RecoverAtomicFile(cover_path);
  const std::array<Row, 2> actions = {
      Row{"Download from SteamGridDB", "Online artwork", true, false, false},
      Row{"Import cover from file", "Local image", true, false, false},
  };
  int selection = 0;

  const int header_height = m_width >= 1600 ? 112 : 80;
  const int content_top = header_height + (m_height >= 1080 ? 92 : 58);
  const int content_bottom = m_height - (m_height >= 1080 ? 112 : 82);
  const int cards_width =
      std::min(m_width - (m_width >= 1600 ? 240 : 120), m_width >= 1600 ? 1420 : 1080);
  const int card_gap = m_width >= 1600 ? 36 : 24;
  const int card_width = (cards_width - card_gap) / 2;
  const int maximum_card_height = m_height >= 1080 ? 540 : 390;
  const int card_height = std::min(maximum_card_height, content_bottom - content_top);
  const int cards_x = (m_width - cards_width) / 2;
  const int cards_y = content_top + std::max(0, content_bottom - content_top - card_height) / 2;
  const std::array<SDL_Rect, 2> cards = {
      SDL_Rect{cards_x, cards_y, card_width, card_height},
      SDL_Rect{cards_x + card_width + card_gap, cards_y, card_width, card_height},
  };
  const auto contains = [](const SDL_Rect& rectangle, int x, int y) {
    return x >= rectangle.x && x < rectangle.x + rectangle.w && y >= rectangle.y &&
           y < rectangle.y + rectangle.h;
  };
  const auto show_info = [&] {
    const Row& action = actions[selection];
    const SettingHelpInfo info = SettingHelpFor("Cover settings", action);
    ShowInfoCard("Cover settings", action.label, info.kind, info.description, action.value,
                 "Per-game setting", true, true);
    BeginScreenFx();
  };
  const auto activate = [&] {
    if (selection == 0)
      DownloadCover(game);
    else
      ImportCoverFromFile(game);
    BeginScreenFx();
  };
  const auto remove_custom_cover = [&] {
    if (!RegularFileExists(cover_path) ||
        !Confirm("Remove custom cover?",
                 std::array<std::string, 2>{
                     "The downloaded or imported cover will be deleted.",
                     "Dolphin will use the game's embedded artwork when available."},
                 true))
    {
      BeginScreenFx();
      return;
    }

    int removal_error = 0;
    if (!RecoverAtomicFile(cover_path))
      removal_error = errno != 0 ? errno : EIO;
    else if (std::remove(cover_path.c_str()) != 0 && errno != ENOENT)
      removal_error = errno != 0 ? errno : EIO;
    if (removal_error != 0)
    {
      RenderMessage("Cover removal failed",
                    std::array<std::string, 1>{std::strerror(removal_error)});
    }
    else
    {
      fsdevCommitDevice("sdmc");
      ReloadCover(game);
      Toast("Custom cover removed", 1200);
    }
    BeginScreenFx();
  };

  m_footer_hit_count = 0;
  BeginScreenFx();
  while (BeginFrame())
  {
    const bool has_custom_cover = RegularFileExists(cover_path);
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      bool choose = false;
      bool info = false;
      bool remove = false;
      bool back = false;

      if (touch == TouchKind::Tap)
      {
        if (contains(cards[0], touch_x, touch_y))
        {
          selection = 0;
          choose = true;
        }
        else if (contains(cards[1], touch_x, touch_y))
        {
          selection = 1;
          choose = true;
        }
        else
        {
          const int footer = FooterHitTest(touch_x, touch_y);
          if (footer == 0)
            choose = true;
          else if (footer == 1)
            info = true;
          else if (has_custom_cover && footer == 2)
            remove = true;
          else if (footer == (has_custom_cover ? 3 : 2))
            back = true;
        }
      }

      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
          selection = 0;
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
          selection = 1;
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
                 event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
          selection = 1 - selection;
        else if (event.cbutton.button == BUTTON_CONFIRM)
          choose = true;
        else if (event.cbutton.button == BUTTON_SETTINGS)
          info = true;
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_X && has_custom_cover)
          remove = true;
        else if (event.cbutton.button == BUTTON_CANCEL)
          back = true;
      }
      else if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_LEFT)
          selection = 0;
        else if (event.key.keysym.sym == SDLK_RIGHT)
          selection = 1;
        else if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_DOWN)
          selection = 1 - selection;
        else if (event.key.keysym.sym == SDLK_RETURN)
          choose = true;
        else if (event.key.keysym.sym == SDLK_x)
          info = true;
        else if ((event.key.keysym.sym == SDLK_y || event.key.keysym.sym == SDLK_DELETE) &&
                 has_custom_cover)
          remove = true;
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          back = true;
      }

      if (back)
        return;
      if (info)
        show_info();
      else if (remove)
        remove_custom_cover();
      else if (choose)
        activate();
    }

    ClearBackground();
    DrawHeader("Cover settings", game->title);
    for (int index = 0; index < static_cast<int>(cards.size()); ++index)
    {
      const SDL_Rect& card = cards[index];
      const bool selected = index == selection;
      FillRect(card.x + 5, card.y + 7, card.w, card.h, SDL_Color{0, 0, 0, 62});
      FillRect(card.x, card.y, card.w, card.h, selected ? m_focus : m_card);
      Border(card.x, card.y, card.w, card.h, selected ? 4 : 2, selected ? m_selection : m_dim);
      if (selected)
        FillRect(card.x, card.y, 8, card.h, m_selection);

      const std::string_view title = m_localization.Translate(actions[index].label);
      TTF_Font* const title_font =
          TextWidth(m_font_large, title) <= card.w - 64 ? m_font_large : m_font;
      const int title_line_height = TTF_FontHeight(title_font) + 8;
      DrawWrappedCentered(title_font, card.x + card.w / 2, card.y + 44, card.w - 64,
                          title_line_height, 2, title, selected ? m_value : m_text);

      const SettingHelpInfo info = SettingHelpFor("Cover settings", actions[index]);
      DrawTextCentered(m_font, card.x + card.w / 2, card.y + (m_height >= 1080 ? 190 : 142),
                       m_localization.Translate(info.kind), selected ? m_highlight : m_dim);
      const int description_y = card.y + (m_height >= 1080 ? 264 : 202);
      const int description_lines = m_height >= 1080 ? 5 : 4;
      DrawWrappedCentered(m_font_small, card.x + card.w / 2, description_y, card.w - 76,
                          TTF_FontHeight(m_font_small) + 8, description_lines,
                          m_localization.Translate(info.description), selected ? m_text : m_dim);

      const std::string_view value = m_localization.Translate(actions[index].value);
      DrawTextCentered(m_font_small, card.x + card.w / 2,
                       card.y + card.h - TTF_FontHeight(m_font_small) - 30, value,
                       selected ? m_value : m_dim);
    }

    if (has_custom_cover)
    {
      static constexpr std::array<std::pair<std::string_view, std::string_view>, 4> hints = {
          std::pair{"A", "Choose"}, std::pair{"X", "Info"}, std::pair{"Y", "Remove custom cover"},
          std::pair{"B", "Back"}};
      DrawFooter(hints);
    }
    else
    {
      static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> hints = {
          std::pair{"A", "Choose"}, std::pair{"X", "Info"}, std::pair{"B", "Back"}};
      DrawFooter(hints);
    }
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

void Launcher::SettingsRoot()
{
  constexpr int count = 10;
  constexpr int launcher_row = 0;
  constexpr int library_row = 1;
  constexpr int achievements_row = 2;
  constexpr int frame_generation_row = 3;
  constexpr int online_row = 9;
  constexpr int section_start = 4;
  int selection = 0;
  int top = 0;
  constexpr int row_height = 54;
  constexpr int y0 = 92;
  constexpr int section_gap = 34;
  static constexpr std::array<std::string_view, count> labels = {"Launcher",
                                                                 "Library & storage",
                                                                 "RetroAchievements",
                                                                 "Frame Generation",
                                                                 "CPU / Emulation",
                                                                 "Graphics",
                                                                 "Audio",
                                                                 "GameCube & Wii",
                                                                 "Controller / Input",
                                                                 "Online & accounts"};
  const int visible = std::max(1, (m_height - y0 - 42 - section_gap) / row_height);
  const auto row_y = [&](int index) {
    return y0 + (index - top) * row_height + (index >= section_start ? section_gap : 0);
  };
  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &top, count, visible))
        continue;
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      bool activate = false;
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == BUTTON_SETTINGS)
        {
          const Row row{std::string(labels[selection]), ">", true, false, false};
          const SettingHelpInfo info = SettingHelpFor("Settings", row);
          ShowInfoCard("Settings", row.label, info.kind, info.description, {}, "Global settings");
          BeginScreenFx();
          continue;
        }
        activate = event.cbutton.button == BUTTON_CONFIRM;
        if (event.cbutton.button == BUTTON_CANCEL)
          return;
      }
      else if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_x)
        {
          const Row row{std::string(labels[selection]), ">", true, false, false};
          const SettingHelpInfo info = SettingHelpFor("Settings", row);
          ShowInfoCard("Settings", row.label, info.kind, info.description, {}, "Global settings");
          BeginScreenFx();
          continue;
        }
        activate = event.key.keysym.sym == SDLK_RETURN;
        if (event.key.keysym.sym == SDLK_ESCAPE)
          return;
      }
      else if (touch == TouchKind::Tap)
      {
        if (touch_y < (m_width >= 1600 ? 112 : 80) || touch_y >= m_height - 40)
          return;
        for (int row = 0; row < visible && top + row < count; ++row)
        {
          const int index = top + row;
          if (touch_y >= row_y(index) && touch_y < row_y(index) + row_height)
          {
            selection = index;
            activate = true;
            break;
          }
        }
      }
      if (activate)
      {
        if (selection == launcher_row)
          AppearanceSettings();
        else if (selection == library_row)
          LibrarySettings();
        else if (selection == achievements_row)
          AchievementSettings();
        else if (selection == frame_generation_row)
          FrameGenerationSettings(false);
        else if (selection == 4)
          EmulationSettings(false);
        else if (selection == 5)
          GraphicsSettings(false);
        else if (selection == 6)
          AudioSettings(false);
        else if (selection == 7)
          ConsoleSettings(false);
        else if (selection == 8)
          ControllerSettings(false);
        else if (selection == online_row)
          NetworkSettings();
        if (m_pending_launch)
          return;
        BeginScreenFx();
      }
      if (selection < top)
        top = selection;
      if (selection >= top + visible)
        top = selection - visible + 1;
    }

    ClearBackground();
    DrawHeader("Settings");
    const int column_width = std::min(980, m_width - 180);
    const int column_x = (m_width - column_width) / 2;
    const int label_x = column_x + 40;
    const int value_x = column_x + column_width - 40;
    const auto draw_visible_section = [&](int begin, int end) {
      const int first = std::max(begin, top);
      const int last = std::min(end, top + visible);
      if (first >= last)
        return;

      const int panel_y = row_y(first) - 10;
      const int panel_bottom = row_y(last - 1) + row_height + 8;
      GlassPanel(column_x - 12, panel_y, column_width + 24, panel_bottom - panel_y);
    };
    draw_visible_section(0, section_start);
    draw_visible_section(section_start, count);
    const float target = static_cast<float>(row_y(selection) + 2);
    m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                        target :
                        m_highlight_y + (target - m_highlight_y) * 0.30f;
    FillRect(column_x, static_cast<int>(m_highlight_y), column_width, row_height - 4, m_focus);
    FillRect(column_x, static_cast<int>(m_highlight_y), 5, row_height - 4, m_selection);
    for (int row = 0; row < visible && top + row < count; ++row)
    {
      const int index = top + row;
      const int slot = row_y(index);
      const int y = slot + (row_height - TTF_FontHeight(m_font)) / 2;
      const bool current = index == selection;
      DrawText(m_font, label_x, y, m_localization.Translate(labels[index]),
               current ? m_value : m_text);
      std::string value;
      if (index == launcher_row)
      {
        const std::string theme = Lower(m_store.Get("Launcher/Theme", "bubbles"));
        value = theme == "xmb"     ? "XMB (PS3)" :
                theme == "glow"    ? "Glow" :
                theme == "classic" ? "Classic" :
                theme == "oled"    ? "OLED black" :
                                     "Bubbles";
      }
      else if (index == library_row)
        value = "game folders / saves / WAD / networking";
      else if (index == achievements_row)
      {
        const bool signed_in = AchievementManager::GetInstance().HasAPIToken();
        value = !signed_in                      ? "Not signed in" :
                Config::Get(Config::RA_ENABLED) ? Config::Get(Config::RA_USERNAME) :
                                                  "Off";
      }
      else if (index == online_row)
        value = "Wii network / BBA";
      else
        value = ">";
      const bool value_is_user_text = index == achievements_row &&
                                      AchievementManager::GetInstance().HasAPIToken() &&
                                      Config::Get(Config::RA_ENABLED);
      const std::string_view displayed_value =
          value_is_user_text ? std::string_view(value) : m_localization.Translate(value);
      DrawTextRight(
          index < section_start ? m_font_small : m_font, value_x,
          slot + (row_height - TTF_FontHeight(index < section_start ? m_font_small : m_font)) / 2,
          displayed_value, current ? m_value : m_dim);
    }
    DrawSettingsFooter("A  Choose       X  Info       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

void Launcher::PerGameSettingsRoot(Game* game)
{
  if (!game)
    return;
  constexpr int count = 7;
  constexpr int row_height = 58;
  constexpr int y0 = 92;
  static constexpr std::array<std::string_view, count> labels = {
      "CPU / Emulation",
      "Graphics",
      "Frame Generation",
      "Audio",
      "GameCube & Wii",
      "Controller / Input",
      "Patches / AR / Gecko / Riivolution"};
  int selection = 0;
  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      bool activate = false;
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == BUTTON_SETTINGS)
        {
          const Row row{std::string(labels[selection]), ">", true, false, false};
          const SettingHelpInfo info = SettingHelpFor("Game settings", row);
          ShowInfoCard("Game settings", row.label, info.kind, info.description, {},
                       "Per-game settings");
          BeginScreenFx();
          continue;
        }
        activate = event.cbutton.button == BUTTON_CONFIRM;
        if (event.cbutton.button == BUTTON_CANCEL)
          return;
      }
      else if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_x)
        {
          const Row row{std::string(labels[selection]), ">", true, false, false};
          const SettingHelpInfo info = SettingHelpFor("Game settings", row);
          ShowInfoCard("Game settings", row.label, info.kind, info.description, {},
                       "Per-game settings");
          BeginScreenFx();
          continue;
        }
        activate = event.key.keysym.sym == SDLK_RETURN;
        if (event.key.keysym.sym == SDLK_ESCAPE)
          return;
      }
      else if (touch == TouchKind::Tap)
      {
        if (touch_y < (m_width >= 1600 ? 112 : 80) || touch_y >= m_height - 40)
          return;
        const int index = (touch_y - y0) / row_height;
        if (index >= 0 && index < count)
        {
          selection = index;
          activate = true;
        }
      }
      if (activate)
      {
        if (selection == 0)
          EmulationSettings(true, game);
        else if (selection == 1)
          GraphicsSettings(true, game);
        else if (selection == 2)
          FrameGenerationSettings(true, game);
        else if (selection == 3)
          AudioSettings(true, game);
        else if (selection == 4)
          ConsoleSettings(true, game);
        else if (selection == 5)
          ControllerSettings(true, game);
        else
          GameModsSettings(game);
        if (m_pending_launch)
          return;
        BeginScreenFx();
      }
    }

    ClearBackground();
    DrawHeader("Game settings", game->title);
    const int column_width = std::min(980, m_width - 180);
    const int column_x = (m_width - column_width) / 2;
    const int label_x = column_x + 40;
    const int value_x = column_x + column_width - 40;
    GlassPanel(column_x - 12, y0 - 10, column_width + 24, count * row_height + 18);
    const float target = static_cast<float>(y0 + selection * row_height + 2);
    m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                        target :
                        m_highlight_y + (target - m_highlight_y) * 0.30f;
    FillRect(column_x, static_cast<int>(m_highlight_y), column_width, row_height - 4, m_focus);
    FillRect(column_x, static_cast<int>(m_highlight_y), 5, row_height - 4, m_selection);
    for (int index = 0; index < count; ++index)
    {
      const int y = y0 + index * row_height + (row_height - TTF_FontHeight(m_font)) / 2;
      const bool current = index == selection;
      DrawText(m_font, label_x, y, m_localization.Translate(labels[index]),
               current ? m_value : m_text);
      DrawTextRight(m_font, value_x, y, ">", current ? m_value : m_dim);
    }
    DrawSettingsFooter("A  Choose       X  Info       B  Back");
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

void Launcher::PerGameMenu(Game* game, bool* launch, bool* rescan)
{
  if (!game || !launch || !rescan)
    return;
  constexpr int count = 10;
  // Leave a full text line between the Game ID/platform summary and the first action.  The old
  // 158px origin placed Launch directly on top of the summary at 720p with several system fonts.
  constexpr int menu_y = 190;
  constexpr int menu_step = 48;
  constexpr int menu_height = 43;
  int selection = 0;
  int touch_top = 0;
  BeginScreenFx();
  while (BeginFrame())
  {
    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (TouchScrollList(touch, &selection, &touch_top, count, count))
        continue;
      const int direction = EventNavigation(event);
      if (direction)
        selection = (selection + direction + count) % count;
      bool activate = false;
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        activate = event.cbutton.button == BUTTON_CONFIRM;
        if (event.cbutton.button == BUTTON_CANCEL)
          return;
      }
      else if (event.type == SDL_KEYDOWN)
      {
        activate = event.key.keysym.sym == SDLK_RETURN;
        if (event.key.keysym.sym == SDLK_ESCAPE)
          return;
      }
      else if (touch == TouchKind::Tap)
      {
        if (touch_y >= m_height - 40)
          return;
        for (int index = 0; index < count; ++index)
        {
          const int row_y = menu_y + index * menu_step - 5;
          if (touch_y >= row_y && touch_y < row_y + menu_height)
          {
            selection = index;
            activate = true;
            break;
          }
        }
      }
      if (!activate)
        continue;

      if (selection == 0)
      {
        *launch = true;
        return;
      }
      if (selection == 1)
      {
        PerGameSettingsRoot(game);
        if (m_pending_launch)
        {
          *launch = true;
          return;
        }
      }
      else if (selection == 2)
      {
        std::string title;
        if (PromptText("Rename game", game->title, &title, false, false))
        {
          game->title = title;
          game->has_custom_title = true;
          if (!game->installed_nand)
            game->config_override_path = EntryGameIniPath(*game);
          m_store.Set("Alias/" + game->key, title);
          MarkStoreDirty();
          game->has_game_config = RegularFileExists(GameIniPath(*game));
        }
      }
      else if (selection == 3)
      {
        EditGameOrganization(game);
      }
      else if (selection == 4)
      {
        CoverSettings(game);
      }
      else if (selection == 5)
      {
        CreateHomeShortcut(game);
      }
      else if (selection == 6)
      {
        InstalledContentManager();
        if (m_pending_launch)
          return;
        if (m_library_refresh_requested)
        {
          *rescan = true;
          return;
        }
      }
      else if (selection == 7)
      {
        const std::string shader_directory = File::GetUserPath(D_SHADERCACHE_IDX);
        int removed = 0;
        std::error_code error;
        if (!game->game_id.empty() && std::filesystem::is_directory(shader_directory, error))
        {
          const std::string marker = "-" + game->game_id;
          for (const auto& entry : std::filesystem::directory_iterator(shader_directory, error))
          {
            if (error)
              break;
            if (entry.is_regular_file(error) &&
                entry.path().filename().string().find(marker) != std::string::npos &&
                std::filesystem::remove(entry.path(), error))
              ++removed;
          }
          const std::string uid_cache =
              File::GetUserPath(D_CACHE_IDX) + game->game_id + ".uidcache";
          if (std::remove(uid_cache.c_str()) == 0)
            ++removed;
        }
        Toast(removed ? "Shader caches cleared" : "No shader caches found", 900);
      }
      else if (selection == 8)
      {
        if (game->has_game_config)
        {
          std::remove(GameIniPath(*game).c_str());
          InvalidateGameSettingCache(*game);
          game->has_game_config = false;
          Toast("Game settings cleared", 700);
        }
        else
        {
          Toast("No game settings found", 700);
        }
      }
      else
      {
        if (game->installed_nand)
        {
          if (Confirm("Uninstall Wii title?",
                      std::array<std::string, 3>{
                          game->title,
                          std::string(m_localization.Translate("Save data is not deleted.")),
                          std::string(m_localization.Translate(
                              "The channel can be reinstalled from its WAD."))}))
          {
            std::string error;
            Tools::Result result = Tools::Result::IoError;
            RunBusyTask("Uninstalling Wii content", game->title,
                        [&] { result = Tools::UninstallWiiTitle(game->title_id, &error); });
            if (result == Tools::Result::Success)
            {
              m_library_refresh_requested = true;
              *rescan = true;
              Toast("Wii title uninstalled", 900);
              return;
            }
            RenderMessage(
                "Uninstall failed",
                std::array<std::string, 1>{error.empty() ? Tools::ResultMessage(result) : error});
          }
        }
        else
        {
          struct stat info{};
          if (::stat(game->path.c_str(), &info) != 0)
          {
            RenderMessage("Delete failed", std::array<std::string, 1>{"The game no longer exists."},
                          true);
          }
          else if (S_ISDIR(info.st_mode))
          {
            RenderMessage(
                "Folder deletion disabled",
                std::array<std::string, 3>{
                    std::string(m_localization.Translate(
                        "Extracted game folders are not deleted automatically.")),
                    std::string(m_localization.Translate(
                        "Remove this folder manually to avoid deleting unrelated files:")),
                    game->path});
          }
          else if (Confirm("Delete game?",
                           std::array<std::string, 4>{
                               game->title, "",
                               std::string(m_localization.Translate(
                                   "This permanently deletes the game file.")),
                               std::string(m_localization.Translate("This cannot be undone."))}))
          {
            if (std::remove(game->path.c_str()) == 0)
            {
              std::remove(CoverPath(*game).c_str());
              std::remove(GameIniPath(*game).c_str());
              *rescan = true;
              Toast("Game deleted", 800);
              return;
            }
            RenderMessage("Delete failed",
                          std::array<std::string, 1>{"The game file could not be removed."}, true);
          }
        }
      }
      BeginScreenFx();
    }

    ClearBackground();
    m_cover_decode_budget = 1;
    EnsureCover(game);
    constexpr int cover_width = 300;
    constexpr int cover_height = 450;
    constexpr int cover_x = 90;
    const int cover_y = (m_height - cover_height) / 2;
    FillRect(cover_x + 5, cover_y + 7, cover_width, cover_height, SDL_Color{0, 0, 0, 60});
    FillRect(cover_x + 2, cover_y + 3, cover_width, cover_height, SDL_Color{0, 0, 0, 75});
    if (game->cover)
    {
      SDL_SetTextureAlphaMod(game->cover, 255);
      SDL_SetTextureColorMod(game->cover, 255, 255, 255);
      SDL_Rect destination{cover_x, cover_y, cover_width, cover_height};
      SDL_RenderCopy(m_renderer, game->cover, nullptr, &destination);
      Border(cover_x, cover_y, cover_width, cover_height, 2, m_dim);
    }
    else
    {
      FillRect(cover_x, cover_y, cover_width, cover_height, SDL_Color{40, 44, 54, 255});
      Border(cover_x, cover_y, cover_width, cover_height, 2, m_dim);
      const std::string_view no_cover = m_localization.Translate("NO COVER");
      const int text_width = cover_width - 32;
      const int line_height = TTF_FontHeight(m_font) + 6;
      const int center_y = cover_y + cover_height / 2;
      if (TextWidth(m_font, no_cover) <= text_width)
        DrawTextCentered(m_font, cover_x + cover_width / 2, center_y - TTF_FontHeight(m_font) / 2,
                         no_cover, m_dim);
      else
        DrawWrappedCentered(m_font, cover_x + cover_width / 2, center_y - line_height, text_width,
                            line_height, 2, no_cover, m_dim);
    }
    DrawScrollingTextLeft(m_font_large, cover_x + cover_width + 70, 104,
                          m_width - (cover_x + cover_width + 70) - 50, game->title, m_text);
    const std::string game_id = game->game_id.empty() ?
                                    std::string(m_localization.Translate("Game ID unavailable")) :
                                    std::string(m_localization.Translate("Game ID")) + "  " +
                                        game->game_id + "    " + game->platform;
    DrawText(m_font_small, cover_x + cover_width + 70, 154, game_id,
             game->game_id.empty() ? SDL_Color{230, 130, 130, 255} : m_dim);

    const int menu_x = cover_x + cover_width + 64;
    const int menu_width = m_width - menu_x - 70;
    const float target = static_cast<float>(menu_y + selection * menu_step - 5);
    m_highlight_y = (!m_animations || m_highlight_y < 0.0f) ?
                        target :
                        m_highlight_y + (target - m_highlight_y) * 0.30f;
    FillRect(menu_x, static_cast<int>(m_highlight_y), menu_width, menu_height, m_focus);
    FillRect(menu_x, static_cast<int>(m_highlight_y), 5, menu_height, m_selection);
    const std::array<std::string, count> items = {
        "Launch",
        "Game settings",
        "Rename game",
        m_favorites.contains(game->key) ? "Favorite / collections  ★" : "Favorite / collections",
        "Cover settings",
        "Create HOME shortcut",
        "Manage installed content",
        "Clear shader caches",
        "Clear game settings",
        game->installed_nand ? "Uninstall WAD (keep save)" : "Delete game (remove from storage)"};
    for (int index = 0; index < count; ++index)
    {
      const int slot = menu_y + index * menu_step - 5;
      const int y = slot + (menu_height - TTF_FontHeight(m_font)) / 2;
      const bool current = index == selection;
      const SDL_Color row_color = index == count - 1 ? SDL_Color{228, 120, 120, 255} : m_text;
      DrawText(m_font, cover_x + cover_width + 94, y, m_localization.Translate(items[index]),
               current ? m_value : row_color);
    }
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
}

bool Launcher::RunAppletInstaller()
{
  if (!Initialize(true))
  {
    if (m_sdl_ready)
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dolphin Installer",
                               "The SDL installer could not be initialized.", m_window);
    return false;
  }
  enum class InstallState
  {
    Ready,
    Installed,
    Failed,
  };
  InstallState state = InstallState::Ready;
  std::string error_message;
  const auto install = [&] {
    std::array<char, 512> error{};
    bool installed = false;
    RunBusyTask("Installing HOME Menu shortcut...", "Dolphin",
                [&] { installed = Forwarder::CreateLauncher(error.data(), error.size()); });
    if (installed)
    {
      state = InstallState::Installed;
      error_message.clear();
    }
    else
    {
      state = InstallState::Failed;
      error_message = error[0] ? error.data() : "Unknown installation error";
    }
    BeginScreenFx();
  };

  BeginScreenFx();
  while (BeginFrame())
  {
    const int panel_width = std::min(980, m_width - 120);
    const int panel_height = std::min(m_height >= 1080 ? 560 : 450, m_height - 150);
    const int panel_x = (m_width - panel_width) / 2;
    const int panel_y = (m_height - panel_height) / 2 + 20;
    const int button_width = std::min(700, panel_width - 100);
    const int button_height = m_height >= 1080 ? 112 : 86;
    const int button_x = (m_width - button_width) / 2;
    const int button_y = panel_y + panel_height - button_height - (m_height >= 1080 ? 72 : 55);

    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::Tap)
      {
        if (touch_x >= button_x && touch_x < button_x + button_width && touch_y >= button_y &&
            touch_y < button_y + button_height && state != InstallState::Installed)
        {
          install();
        }
        else if (touch_y >= m_height - 54)
        {
          return true;
        }
        continue;
      }
      if (event.type == SDL_CONTROLLERBUTTONDOWN)
      {
        if (event.cbutton.button == BUTTON_CONFIRM && state != InstallState::Installed)
          install();
        else if (event.cbutton.button == BUTTON_CANCEL)
          return true;
      }
      else if (event.type == SDL_KEYDOWN)
      {
        if (event.key.keysym.sym == SDLK_RETURN && state != InstallState::Installed)
          install();
        else if (event.key.keysym.sym == SDLK_ESCAPE)
          return true;
      }
    }

    ClearBackground();
    DrawHeader("Applet mode installer");
    GlassPanel(panel_x, panel_y, panel_width, panel_height);
    Border(panel_x, panel_y, panel_width, panel_height, 2, m_selection);
    const int text_width = panel_width - (m_height >= 1080 ? 160 : 100);
    const int normal_line_height = m_height >= 1080 ? 42 : 32;
    const int small_line_height = m_height >= 1080 ? 34 : 27;

    if (state == InstallState::Installed)
    {
      DrawWrappedCentered(m_font, m_width / 2, panel_y + 52, text_width, normal_line_height, 2,
                          m_localization.Translate("Dolphin was installed on the HOME Menu."),
                          m_value);
      DrawWrappedCentered(
          m_font_small, m_width / 2, panel_y + 128, text_width, small_line_height, 2,
          m_localization.Translate("You can close this installer and launch Dolphin from HOME."),
          m_text);
    }
    else if (state == InstallState::Failed)
    {
      DrawWrappedCentered(m_font, m_width / 2, panel_y + 50, text_width, normal_line_height, 2,
                          m_localization.Translate("Installation failed"),
                          SDL_Color{255, 155, 155, 255});
      DrawWrappedCentered(
          m_font_small, m_width / 2, panel_y + 112, text_width, small_line_height, 6,
          error_message.empty() ? m_localization.Translate("Unknown installation error") :
                                  std::string_view(error_message),
          m_text);
    }
    else
    {
      DrawWrappedCentered(m_font, m_width / 2, panel_y + 42, text_width, normal_line_height, 2,
                          m_localization.Translate("Dolphin is running in applet mode."), m_value);
      DrawWrappedCentered(m_font_small, m_width / 2, panel_y + 104, text_width, small_line_height,
                          2,
                          m_localization.Translate(
                              "Applet mode has limited memory and is not suitable for emulation."),
                          m_text);
      DrawWrappedCentered(m_font_small, m_width / 2, panel_y + 166, text_width, small_line_height,
                          3,
                          m_localization.Translate("Install a HOME Menu shortcut to run Dolphin "
                                                   "with full memory and normal performance."),
                          m_dim);
    }

    const bool failed = state == InstallState::Failed;
    const bool installed = state == InstallState::Installed;
    FillRect(button_x, button_y, button_width, button_height,
             installed ? SDL_Color{30, 92, 58, 240} :
             failed    ? SDL_Color{105, 48, 48, 240} :
                         m_focus);
    Border(button_x, button_y, button_width, button_height, 3,
           installed ? SDL_Color{100, 225, 145, 255} :
           failed    ? SDL_Color{235, 125, 125, 255} :
                       m_selection);
    const std::string_view button_label =
        m_localization.Translate(installed ? "Installed" :
                                 failed    ? "Try again" :
                                             "Install Dolphin to HOME Menu");
    TTF_Font* const button_font =
        TextWidth(m_font_large, button_label) <= button_width - 48 ? m_font_large : m_font;
    DrawTextCentered(button_font, m_width / 2,
                     button_y + (button_height - TTF_FontHeight(button_font)) / 2,
                     Ellipsize(button_font, button_label, button_width - 48),
                     installed ? SDL_Color{190, 255, 215, 255} : m_value);

    if (installed)
    {
      static constexpr std::array<std::pair<std::string_view, std::string_view>, 1> hints = {
          std::pair{"B", "Exit"}};
      DrawFooter(hints);
    }
    else
    {
      const std::array<std::pair<std::string_view, std::string_view>, 2> hints = {
          std::pair{"A", failed ? "Try again" : "Install"}, std::pair{"B", "Exit"}};
      DrawFooter(hints);
    }
    DrawFadeIn();
    SDL_RenderPresent(m_renderer);
    WaitForNextFrame();
  }
  return true;
}

std::optional<LaunchRequest> Launcher::Run()
{
  if (!Initialize(false))
  {
    if (m_sdl_ready)
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dolphin Launcher",
                               "The full SDL launcher could not be initialized.", m_window);
    return std::nullopt;
  }
  ScanGames();
  BeginScreenFx();
  RenderGrid(0);
  StartUsbInitialization();
  if (m_cover_download_ready && m_store.GetBool("Launcher/CheckUpdatesAtBoot", true))
    Updater::StartCheck(InstalledReleaseTag());
  if (!m_startup_message.empty())
  {
    RenderMessage("Dolphin", std::array<std::string, 1>{m_startup_message});
    m_startup_message.clear();
    BeginScreenFx();
  }
  int selection = 0;
  bool launch = false;
  // A fresh launcher must stay on page 1 while progressive scan batches are sorted in.  Without
  // this guard, preserving the initially selected game's key can move the selection (and visible
  // page) as games which sort before it arrive.  Stop pinning only after deliberate library
  // navigation; normal in-session selection preservation then resumes.
  bool pin_first_library_page = true;
  std::string desired_selection_key;
  const auto select_key = [&](std::string_view key) {
    if (key.empty())
      return false;
    for (std::size_t index = 0; index < m_visible_games.size(); ++index)
    {
      if (m_games[m_visible_games[index]].key == key)
      {
        selection = static_cast<int>(index);
        return true;
      }
    }
    return false;
  };
  const auto rescan_library = [&] {
    const Game* selected = VisibleGame(selection);
    desired_selection_key = selected ? selected->key : std::string{};
    ScanGames();
    selection = 0;
  };

  while (BeginFrame() && !launch && !m_pending_launch)
  {
    const Game* before_pump = pin_first_library_page ? nullptr : VisibleGame(selection);
    const std::string selected_key =
        pin_first_library_page ? std::string{} :
                                 (before_pump ? before_pump->key : desired_selection_key);
    PumpGameScan();
    PumpUsbInitialization();
    PumpAutoMountShares();
    if (pin_first_library_page)
    {
      selection = 0;
      desired_selection_key.clear();
    }
    else
    {
      if (!selected_key.empty() && select_key(selected_key))
        desired_selection_key.clear();
      selection = m_visible_games.empty() ?
                      0 :
                      std::clamp(selection, 0, static_cast<int>(m_visible_games.size()) - 1);
    }

    if (!m_library_scan && (!m_pending_scan_sources.empty() || m_pending_nand_reconciliation))
    {
      std::vector<std::string> pending = std::move(m_pending_scan_sources);
      m_pending_scan_sources.clear();
      m_pending_nand_reconciliation = false;
      StartGameScan(std::move(pending), false);
    }

    const Uint32 now = SDL_GetTicks();
    const Storage::UsbSnapshot usb_snapshot = Storage::GetUsbSnapshot();
    if (usb_snapshot.generation != m_usb_generation)
    {
      m_usb_generation = usb_snapshot.generation;
      m_usb_refresh_at = now + 350;
    }
    if (m_usb_refresh_at && SDL_TICKS_PASSED(now, m_usb_refresh_at))
    {
      m_usb_refresh_at = 0;
      const Storage::UsbSnapshot current = Storage::GetUsbSnapshot();
      std::unordered_map<std::string, std::string> old_roots;
      std::unordered_map<std::string, std::string> new_roots;
      for (const Storage::Location& location : m_usb_locations)
        old_roots.emplace(location.id, location.path);
      for (const Storage::Location& location : current.locations)
        new_roots.emplace(location.id, location.path);

      std::unordered_set<std::string> changed_ids;
      for (const auto& [id, root] : new_roots)
      {
        const auto old = old_roots.find(id);
        if (old == old_roots.end() ||
            Lower(NormalizePath(old->second)) != Lower(NormalizePath(root)))
          changed_ids.insert(id);
      }
      std::unordered_set<std::string> removed_ids;
      for (const auto& [id, root] : old_roots)
      {
        if (!new_roots.contains(id))
          removed_ids.insert(id);
      }
      for (const auto& [id, root] : new_roots)
        m_unavailable_usb_ids.erase(id);
      for (const std::string& id : removed_ids)
        m_unavailable_usb_ids.insert(id);
      m_usb_locations = current.locations;
      RefreshConfiguredUsbSources();
      // A disconnect/reconnect can return with the same ID and alias. The callback generation
      // still proves device activity, so refresh USB sources even when the topology looks equal.
      if (changed_ids.empty() && removed_ids.empty())
      {
        for (const auto& [source, binding] : m_usb_source_bindings)
          changed_ids.insert(binding.first);
      }
      for (const std::string& id : changed_ids)
        m_unavailable_usb_ids.insert(id);
      if (!removed_ids.empty() || !changed_ids.empty())
      {
        std::erase_if(m_games, [&](Game& game) {
          if (!game.storage_id.starts_with("usb:"))
            return false;
          const std::string id = game.storage_id.substr(4);
          if (!removed_ids.contains(id) && !changed_ids.contains(id))
            return false;
          if (game.cover)
            SDL_DestroyTexture(game.cover);
          return true;
        });
        // A removed volume may have produced a WAD candidate which is still in the worker's ready
        // queue and therefore absent from m_games. Always reconcile NAND after physical removal;
        // this also covers that progressive-scan race without requiring a visible WAD entry.
        m_pending_nand_reconciliation |= !removed_ids.empty();
      }
      for (const std::string& source : m_sources)
      {
        const auto binding = m_usb_source_bindings.find(Lower(source));
        if (binding != m_usb_source_bindings.end() && changed_ids.contains(binding->second.first))
          m_pending_scan_sources.push_back(source);
      }
      std::ranges::sort(m_pending_scan_sources);
      m_pending_scan_sources.erase(
          std::unique(m_pending_scan_sources.begin(), m_pending_scan_sources.end()),
          m_pending_scan_sources.end());
      RebuildVisibleGames();
      selection = pin_first_library_page || m_visible_games.empty() ?
                      0 :
                      std::clamp(selection, 0, static_cast<int>(m_visible_games.size()) - 1);
    }

    SDL_Event event{};
    while (PollEvent(&event))
    {
      int touch_x = 0;
      int touch_y = 0;
      const TouchKind touch = FeedTouch(event, &touch_x, &touch_y);
      if (touch == TouchKind::SwipeLeft || touch == TouchKind::SwipeRight)
      {
        pin_first_library_page = false;
        selection = GridPage(selection, touch == TouchKind::SwipeLeft ? 1 : -1);
        continue;
      }
      if (touch == TouchKind::Tap)
      {
        const int footer = FooterHitTest(touch_x, touch_y);
        if (footer < 0)
        {
          const int hit = GridHitTest(
              touch_x, touch_y,
              m_visible_games.empty() ? 0 : selection / GridPageSize() * GridPageSize());
          if (hit >= 0)
          {
            pin_first_library_page = false;
            if (hit == selection)
              launch = true;
            else
              selection = hit;
          }
        }
        else if (footer <= 3)
        {
          SDL_Event press{};
          press.type = SDL_CONTROLLERBUTTONDOWN;
          press.cbutton.button = footer == 0 ? BUTTON_CONFIRM :
                                 footer == 1 ? SDL_CONTROLLER_BUTTON_X :
                                 footer == 2 ? BUTTON_SETTINGS :
                                               SDL_CONTROLLER_BUTTON_START;
          SDL_PushEvent(&press);
        }
        else if (footer == 4)
        {
          SDL_Event press{};
          press.type = SDL_CONTROLLERBUTTONDOWN;
          press.cbutton.button = SDL_CONTROLLER_BUTTON_BACK;
          SDL_PushEvent(&press);
        }
        else if (footer == 5)
        {
          pin_first_library_page = false;
          selection = GridPage(selection, -1);
        }
        else if (footer == 6)
        {
          pin_first_library_page = false;
          selection = GridPage(selection, 1);
        }
        else if (footer == 7)
        {
          if (ConfirmApplicationExit())
            break;
        }
        continue;
      }
      if (event.type != SDL_CONTROLLERBUTTONDOWN && event.type != SDL_KEYDOWN)
        continue;
      const int button = event.type == SDL_CONTROLLERBUTTONDOWN ? event.cbutton.button : -1;
      const SDL_Keycode key = event.type == SDL_KEYDOWN ? event.key.keysym.sym : SDLK_UNKNOWN;
      if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || key == SDLK_LEFT)
      {
        pin_first_library_page = false;
        selection = GridNavigate(selection, -1, 0);
      }
      else if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT || key == SDLK_RIGHT)
      {
        pin_first_library_page = false;
        selection = GridNavigate(selection, 1, 0);
      }
      else if (button == SDL_CONTROLLER_BUTTON_DPAD_UP || key == SDLK_UP)
      {
        pin_first_library_page = false;
        selection = GridNavigate(selection, 0, -1);
      }
      else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN || key == SDLK_DOWN)
      {
        pin_first_library_page = false;
        selection = GridNavigate(selection, 0, 1);
      }
      else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || key == SDLK_PAGEUP)
      {
        pin_first_library_page = false;
        selection = GridPage(selection, -1);
      }
      else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER || key == SDLK_PAGEDOWN)
      {
        pin_first_library_page = false;
        selection = GridPage(selection, 1);
      }
      else if ((button == BUTTON_CONFIRM || key == SDLK_RETURN) && !m_visible_games.empty())
        launch = true;
      else if ((button == SDL_CONTROLLER_BUTTON_X || key == SDLK_s) && !m_visible_games.empty())
      {
        pin_first_library_page = false;
        Game* const selected_game = VisibleGame(selection);
        if (!selected_game)
          continue;
        const std::string sort_selected_key = selected_game->key;
        m_sort_mode = static_cast<SortMode>((static_cast<int>(m_sort_mode) + 1) % 3);
        m_store.SetInt("Launcher/SortMode", static_cast<int>(m_sort_mode));
        MarkStoreDirty();
        SortGames();
        selection = 0;
        select_key(sort_selected_key);
      }
      else if (button == BUTTON_SETTINGS || key == SDLK_F1)
      {
        const std::vector<std::string> old_sources = m_sources;
        SettingsRoot();
        FlushPendingSaves();
        if (!m_pending_launch && (old_sources != m_sources || m_library_refresh_requested))
          rescan_library();
      }
      else if (button == SDL_CONTROLLER_BUTTON_BACK || key == SDLK_f)
      {
        pin_first_library_page = false;
        const Game* selected = VisibleGame(selection);
        const std::string keep = selected ? selected->key : std::string{};
        LibraryFilterMenu();
        RebuildVisibleGames();
        selection = 0;
        select_key(keep);
        BeginScreenFx();
      }
      else if ((button == SDL_CONTROLLER_BUTTON_START || key == SDLK_SPACE) &&
               !m_visible_games.empty())
      {
        pin_first_library_page = false;
        bool rescan = false;
        PerGameMenu(VisibleGame(selection), &launch, &rescan);
        if (rescan)
          rescan_library();
      }
      else if (button == BUTTON_CANCEL || key == SDLK_ESCAPE)
      {
        if (ConfirmApplicationExit())
          break;
      }
    }
    if (!m_running)
      break;
    PollUpdateNotification();
    RenderGrid(selection);
    WaitForNextFrame();
  }

  if (m_user_exit_requested)
  {
    PrepareApplicationExit();
    return std::nullopt;
  }

  if (m_pending_launch)
  {
    if (m_library_identities_dirty)
      SaveLibraryIdentities();
    FlushPendingSaves();
    return std::exchange(m_pending_launch, std::nullopt);
  }
  if (!launch || m_visible_games.empty())
  {
    return std::nullopt;
  }
  Game* const selected_game = VisibleGame(selection);
  if (!selected_game)
    return std::nullopt;
  Game& game = *selected_game;
  game.played = static_cast<std::int64_t>(std::time(nullptr));
  m_store.Set("Recent/" + game.key, std::to_string(game.played));
  MarkStoreDirty();
  if (m_library_identities_dirty)
    SaveLibraryIdentities();
  FlushPendingSaves();
  if (game.installed_nand)
    return LaunchRequest{{}, game.game_id, game.revision, game.title_id};
  LaunchRequest request{game.path, game.game_id, game.revision};
  request.game_config_path = game.config_override_path;
  return request;
}

}  // namespace

std::optional<LaunchRequest> RunLauncher(std::string startup_message, std::string launcher_path)
{
  std::optional<LaunchRequest> request;
  {
    Launcher launcher(std::move(startup_message), std::move(launcher_path));
    request = launcher.Run();
  }
  return request;
}

bool RunAppletInstaller(std::string launcher_path)
{
  if (!launcher_path.empty())
    Forwarder::SetSelfPath(std::move(launcher_path));
  Launcher launcher({}, {});
  return launcher.RunAppletInstaller();
}

bool RecordInstalledReleaseTag(std::string_view tag)
{
  const std::string normalized = Trim(std::string(tag));
  if (normalized.empty())
    return false;

  Store store;
  if (!store.Load(std::string(CONFIG_PATH)))
    return false;
  store.Set("Launcher/InstalledReleaseTag", normalized);
  return store.Save(std::string(CONFIG_PATH));
}

bool PrepareLaunchStorage(const std::string& path, std::string* resolved_path)
{
  const std::string normalized = NormalizePath(path);
  if (resolved_path)
    *resolved_path = normalized;
  const std::string device = DeviceName(normalized);
  if (device.starts_with("ums"))
  {
    if (!Storage::InitializeUsb())
      return false;
    const auto path_exists = [](const std::string& candidate) {
      struct stat info{};
      return ::stat(candidate.c_str(), &info) == 0 &&
             (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode));
    };
    const std::size_t colon = normalized.find(':');
    std::string relative =
        colon == std::string::npos ? std::string{} : normalized.substr(colon + 1);
    while (!relative.empty() && relative.front() == '/')
      relative.erase(relative.begin());
    const auto resolve_usb_path = [&]() -> std::string {
      if (path_exists(normalized))
        return normalized;
      std::vector<std::string> matches;
      for (const Storage::Location& location : Storage::ListUsbLocations())
      {
        const std::string candidate = NormalizePath(location.path + relative);
        if (path_exists(candidate))
          matches.push_back(candidate);
      }
      return matches.size() == 1 ? matches.front() : std::string{};
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    std::string available_path;
    while ((available_path = resolve_usb_path()).empty() &&
           std::chrono::steady_clock::now() < deadline)
    {
      if (!appletMainLoop())
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (available_path.empty())
      available_path = resolve_usb_path();
    if (available_path.empty())
      return false;
    if (resolved_path)
      *resolved_path = available_path;
    return true;
  }
  if (!device.starts_with("dsmb_"))
  {
    return true;
  }

  const std::string id = device.substr(5);
  if (Storage::IsSmbMounted(id))
  {
    return true;
  }
  Store store;
  if (!store.Load(std::string(CONFIG_PATH)))
    return false;
  const int share_count = std::clamp(store.GetInt("Storage/SmbCount", 0), 0, 8);
  for (int index = 0; index < share_count; ++index)
  {
    const std::string prefix = "Storage/Smb" + std::to_string(index);
    Storage::SmbShare share;
    share.id = store.Get(prefix + "Id");
    if (Lower(share.id) != id)
      continue;
    share.name = store.Get(prefix + "Name");
    share.server = store.Get(prefix + "Server");
    share.share = store.Get(prefix + "Share");
    share.path = store.Get(prefix + "Path");
    share.user = store.Get(prefix + "User");
    share.password = store.Get(prefix + "Password");
    share.domain = store.Get(prefix + "Domain");
    share.auto_mount = store.GetBool(prefix + "AutoMount", true);
    if (!Storage::MountSmb(share))
      return false;
    struct stat mounted_info{};
    const bool available = ::stat(normalized.c_str(), &mounted_info) == 0 &&
                           (S_ISREG(mounted_info.st_mode) || S_ISDIR(mounted_info.st_mode));
    return available;
  }
  return false;
}

bool ResolveLibraryLaunchPath(std::string_view library_id, const std::string& fallback_path,
                              std::string* resolved_path)
{
  const auto path_exists = [](const std::string& candidate) {
    struct stat info{};
    return ::stat(candidate.c_str(), &info) == 0 &&
           (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode));
  };
  const auto try_path = [&](const std::string& candidate) {
    if (candidate.empty())
      return false;
    std::string prepared;
    if (!PrepareLaunchStorage(candidate, &prepared) || !path_exists(prepared))
      return false;
    if (resolved_path)
      *resolved_path = std::move(prepared);
    return true;
  };

  const bool valid_id = !library_id.empty() && library_id.size() <= 96 &&
                        std::ranges::all_of(library_id, [](unsigned char character) {
                          return std::isalnum(character) || character == '-' || character == '_';
                        });
  Store store;
  std::string canonical_path;
  std::string current_path;
  bool retired = false;
  bool record_found = false;
  if (valid_id && store.Load(std::string(CONFIG_PATH)))
  {
    const int count = std::clamp(store.GetInt("Library/IdentityCount", 0), 0, 16384);
    for (int index = 0; index < count; ++index)
    {
      const std::string prefix = "Library/Identity" + std::to_string(index);
      if (store.Get(prefix + "Id") != library_id)
        continue;
      canonical_path = NormalizePath(store.Get(prefix + "Path"));
      current_path = NormalizePath(store.Get(prefix + "CurrentPath"));
      retired = store.GetBool(prefix + "Retired", false);
      record_found = true;
      break;
    }
  }

  // A stable shortcut must never fall back to its embedded mutable path when the ID is invalid,
  // missing, or retired. In particular, a stale umsN: alias may now belong to another drive.
  if (!library_id.empty() && (!valid_id || !record_found || retired))
    return false;

  if (canonical_path.starts_with("usb:"))
  {
    const std::size_t slash = canonical_path.find('/', 4);
    const std::string volume_id =
        canonical_path.substr(4, slash == std::string::npos ? std::string::npos : slash - 4);
    std::string relative =
        slash == std::string::npos ? std::string{} : canonical_path.substr(slash + 1);

    // CurrentPath preserves case, whereas the canonical identity deliberately does not.  Reuse
    // its path below the old umsN: root when it still describes the same relative name.
    const std::size_t current_colon = current_path.find(':');
    if (current_colon != std::string::npos)
    {
      std::string current_relative = current_path.substr(current_colon + 1);
      while (!current_relative.empty() && current_relative.front() == '/')
        current_relative.erase(current_relative.begin());
      if (Lower(current_relative) == Lower(relative))
        relative = std::move(current_relative);
    }

    // Do not fall back to the mutable embedded umsN: alias when a stable volume is known: another
    // disk can later inherit that alias and contain an identically named file.
    if (volume_id.empty() || relative.starts_with('/') ||
        relative.find("../") != std::string::npos || relative == ".." || !Storage::InitializeUsb())
      return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    do
    {
      const std::string root = Storage::ResolveUsbPath(volume_id);
      if (!root.empty())
      {
        const std::string candidate = NormalizePath(JoinPath(root, relative));
        if (PathAtOrBelow(candidate, root) && path_exists(candidate))
        {
          if (resolved_path)
            *resolved_path = candidate;
          return true;
        }
      }
      if (!appletMainLoop())
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  if (canonical_path.starts_with("smb:"))
  {
    const std::size_t slash = canonical_path.find('/', 4);
    const std::string share_id =
        canonical_path.substr(4, slash == std::string::npos ? std::string::npos : slash - 4);
    std::string relative =
        slash == std::string::npos ? std::string{} : canonical_path.substr(slash + 1);
    const std::string root = Storage::SmbRootPath(share_id);
    if (root.empty() || relative.starts_with('/') || relative.find("../") != std::string::npos ||
        relative == "..")
      return false;

    // Preserve case from CurrentPath when possible.  The dsmb_<id>: mount name itself is stable.
    const std::size_t current_colon = current_path.find(':');
    if (current_colon != std::string::npos &&
        Lower(DeviceName(current_path)) == Lower(DeviceName(root)))
    {
      std::string current_relative = current_path.substr(current_colon + 1);
      while (!current_relative.empty() && current_relative.front() == '/')
        current_relative.erase(current_relative.begin());
      if (Lower(current_relative) == Lower(relative))
        relative = std::move(current_relative);
    }
    const std::string candidate = NormalizePath(JoinPath(root, relative));
    return PathAtOrBelow(candidate, root) && try_path(candidate);
  }

  // SD paths are stable but can be case-sensitive on non-FAT devoptabs.  Prefer the exact current
  // path, then support launcher.ini records written before CurrentPath was introduced.
  if (try_path(current_path) || try_path(canonical_path))
    return true;
  return try_path(fallback_path);
}

void ShutdownLauncherStorage()
{
  Storage::Shutdown();
}
}  // namespace DolphinSwitch
