// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/EmbeddedRuntime.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/IniFile.h"
#include "DolphinSwitch/Updater.h"
#include "DolphinSwitch/Deko3DName.h"

namespace DolphinSwitch::EmbeddedRuntime
{
namespace
{
#ifdef __SWITCH2__
constexpr char SOURCE[] = "romfs:/runtime/dolphin-drako3d.nro";
constexpr char SOURCE_ID[] = "romfs:/runtime/dolphin-drako3d.sha256";
constexpr char DESTINATION[] = "sdmc:/switch/dolphin/.emu/dolphin-drako3d.nro";
constexpr char CACHE_STAMP[] = "sdmc:/switch/dolphin/.emu/dolphin-drako3d.ini";
#else
constexpr char SOURCE[] = "romfs:/runtime/dolphin-deko3d.nro";
constexpr char SOURCE_ID[] = "romfs:/runtime/dolphin-deko3d.sha256";
constexpr char DESTINATION[] = "sdmc:/switch/dolphin/.emu/dolphin-deko3d.nro";
constexpr char CACHE_STAMP[] = "sdmc:/switch/dolphin/.emu/dolphin-deko3d.ini";
#endif
constexpr char HANDOFF[] = "sdmc:/switch/dolphin/.emu/launch.ini";
constexpr char RETURN_FILE[] = "sdmc:/switch/dolphin/.emu/return.ini";
std::string Encode(std::string_view value)
{
  constexpr char digits[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char c : value)
  {
    encoded += digits[c >> 4];
    encoded += digits[c & 15];
  }
  return encoded;
}

bool ReadString(const Common::IniFile::Section& section, const char* key, std::string* output)
{
  std::string encoded;
  if (!section.Get(key, &encoded) || encoded.size() > 128 * 1024 || encoded.size() % 2 != 0)
    return false;
  const auto nibble = [](char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
  };
  output->clear();
  for (size_t i = 0; i < encoded.size(); i += 2)
  {
    const int high = nibble(encoded[i]);
    const int low = nibble(encoded[i + 1]);
    if (high < 0 || low < 0 || (high == 0 && low == 0))
      return false;
    *output += static_cast<char>((high << 4) | low);
  }
  return true;
}

bool ValidNro(File::IOFile& file)
{
  std::array<u8, 0x20> header{};
  if (!file || file.GetSize() < 0x80 || file.GetSize() > 512ULL * 1024 * 1024 ||
      !file.Seek(0, File::SeekOrigin::Begin) || !file.ReadBytes(header.data(), header.size()) ||
      std::memcmp(header.data() + 0x10, "NRO0", 4) != 0)
  {
    return false;
  }
  u32 image_size = 0;
  std::memcpy(&image_size, header.data() + 0x18, sizeof(image_size));
  return image_size >= 0x80 && image_size <= file.GetSize() &&
         file.Seek(0, File::SeekOrigin::Begin);
}

bool SameFile(File::IOFile& source, File::IOFile& destination, const ExtractProgress& progress = {})
{
  if (!destination || source.GetSize() != destination.GetSize() ||
      !source.Seek(0, File::SeekOrigin::Begin) || !destination.Seek(0, File::SeekOrigin::Begin))
    return false;

  std::array<u8, 64 * 1024> input{};
  std::array<u8, 64 * 1024> cached{};
  u64 remaining = source.GetSize();
  while (remaining != 0)
  {
    const size_t count = static_cast<size_t>(std::min<u64>(remaining, input.size()));
    if (!source.ReadBytes(input.data(), count) || !destination.ReadBytes(cached.data(), count) ||
        std::memcmp(input.data(), cached.data(), count) != 0)
      return false;
    remaining -= count;
    if (progress)
      progress(source.GetSize() - remaining, source.GetSize());
  }
  return true;
}

bool SaveAtomic(Common::IniFile& ini, const char* path)
{
  const std::string temporary = std::string(path) + ".tmp";
  if (!File::CreateFullPath(temporary) || !ini.Save(temporary))
    return false;
  File::IOFile file(temporary, "rb+");
  const bool synced = file && fsync(fileno(file.GetHandle())) == 0;
  file.Close();
  Common::IniFile read_back;
  bool complete = synced && read_back.Load(temporary);
  if (complete)
  {
    for (const auto& section : ini.GetSections())
    {
      const auto* saved = read_back.GetSection(section.GetName());
      if (!saved || saved->GetValues() != section.GetValues())
      {
        complete = false;
        break;
      }
    }
  }
  if (complete && File::Rename(temporary, path))
    return true;
  File::Delete(temporary);
  return false;
}

std::string ReadRuntimeId()
{
  File::IOFile file(SOURCE_ID, "rb");
  std::string id(64, '\0');
  if (!file || file.GetSize() != 65 || !file.ReadBytes(id.data(), id.size()) ||
      !std::all_of(id.begin(), id.end(),
                   [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
  {
    return {};
  }
  return id;
}

std::optional<s64> GetModifiedTime(File::IOFile& file)
{
  struct stat info{};
  if (!file || fstat(fileno(file.GetHandle()), &info) != 0)
    return std::nullopt;
  return static_cast<s64>(info.st_mtime);
}

bool CacheMatches(File::IOFile& source, File::IOFile& cached, const std::string& id)
{
  if (id.empty() || !ValidNro(cached) || source.GetSize() != cached.GetSize())
    return false;

  Common::IniFile ini;
  if (!ini.Load(CACHE_STAMP))
    return false;
  const auto* section = ini.GetSection("Runtime");
  std::string saved_id;
  u64 size = 0;
  s64 modified = 0;
  const auto current_modified = GetModifiedTime(cached);
  return section && section->Get("Digest", &saved_id) && saved_id == id &&
         section->Get("Size", &size) && size == cached.GetSize() &&
         section->Get("Modified", &modified) && current_modified == modified;
}

bool SaveCacheStamp(File::IOFile& cached, const std::string& id)
{
  const auto modified = GetModifiedTime(cached);
  if (!modified)
    return false;
  Common::IniFile ini;
  auto* section = ini.GetOrCreateSection("Runtime");
  section->Set("Digest", id);
  section->Set("Size", cached.GetSize());
  section->Set("Modified", *modified);
  return SaveAtomic(ini, CACHE_STAMP);
}

bool ExtractRuntime(std::string* error, const ExtractProgress& progress)
{
  File::IOFile source(SOURCE, "rb");
  const std::string id = ReadRuntimeId();
  if (!ValidNro(source) || id.empty())
  {
    *error = "The embedded " DEKO3D_DISPLAY_NAME
             " runtime is missing or invalid. Reinstall " DEKO3D_LAUNCHER_NRO ".";
    return false;
  }
  {
    File::IOFile cached(DESTINATION, "rb");
    if (CacheMatches(source, cached, id))
      return true;
    if (SameFile(source, cached, progress) && SaveCacheStamp(cached, id))
      return true;
  }

  const std::string temporary = std::string(DESTINATION) + ".tmp";
  if (!File::CreateFullPath(temporary) || !source.Seek(0, File::SeekOrigin::Begin))
  {
    *error = "Could not create Dolphin's .emu folder on the SD card.";
    return false;
  }
  bool success = false;
  const u64 total = source.GetSize();
  if (progress)
    progress(0, total * 2);
  {
    File::IOFile output(temporary, "wb");
    std::array<u8, 64 * 1024> buffer{};
    u64 remaining = source.GetSize();
    success = static_cast<bool>(output);
    while (success && remaining != 0)
    {
      const size_t count = static_cast<size_t>(std::min<u64>(remaining, buffer.size()));
      success = source.ReadBytes(buffer.data(), count) && output.WriteBytes(buffer.data(), count);
      remaining -= count;
      if (progress)
        progress(total - remaining, total * 2);
    }
    success = success && output.Flush() && fsync(fileno(output.GetHandle())) == 0;
  }
  if (success)
  {
    File::IOFile output(temporary, "rb");
    success = ValidNro(output) && SameFile(source, output, [&](u64 done, u64) {
                if (progress)
                  progress(total + done, total * 2);
              });
  }
  if (!success || !File::Rename(temporary, DESTINATION))
  {
    File::Delete(temporary);
    *error = "Could not extract the " DEKO3D_DISPLAY_NAME
             " runtime. Check the SD card's free space.";
    return false;
  }
  File::IOFile cached(DESTINATION, "rb");
  if (!SaveCacheStamp(cached, id))
  {
    *error = "Could not save the " DEKO3D_DISPLAY_NAME " runtime cache information to the SD card.";
    return false;
  }
  return true;
}
}

bool IsCached()
{
  File::IOFile source(SOURCE, "rb");
  File::IOFile cached(DESTINATION, "rb");
  return ValidNro(source) && CacheMatches(source, cached, ReadRuntimeId());
}

bool Launch(const std::string& launcher_path, const LaunchRequest& game, std::string* error,
            const ExtractProgress& progress)
{
  if (!envHasNextLoad())
  {
    *error = "The homebrew loader does not support launching the " DEKO3D_DISPLAY_NAME " runtime.";
    return false;
  }
  if (!ExtractRuntime(error, progress))
    return false;

  Common::IniFile ini;
  auto* section = ini.GetOrCreateSection("Runtime");
  section->Set("Version", 1);
  section->Set("Launcher", Encode(Updater::ResolveLauncherPath(launcher_path)));
  section->Set("Path", Encode(game.path));
  section->Set("GameID", Encode(game.game_id));
  section->Set("Revision", game.revision);
  section->Set("GameConfig", Encode(game.game_config_path));
  section->Set("LibraryID", Encode(game.library_id));
  section->Set("GameShare", game.gameshare);
  if (game.nand_title)
    section->Set("NandTitle", fmt::format("0x{:016x}", *game.nand_title));
  if (!SaveAtomic(ini, HANDOFF))
  {
    *error = "Could not save the " DEKO3D_DISPLAY_NAME " launch request to the SD card.";
    return false;
  }
  const std::string arguments = std::string(DESTINATION) + " " + std::string(LAUNCH_ARGUMENT);
  const Result result = envSetNextLoad(DESTINATION, arguments.c_str());
  if (R_FAILED(result))
  {
    File::Delete(HANDOFF);
    *error = fmt::format("The homebrew loader rejected the " DEKO3D_DISPLAY_NAME " runtime ({:08x}).",
                         result);
    return false;
  }
  return true;
}

std::optional<Request> ReadRequest()
{
  Common::IniFile ini;
  if (!ini.Load(HANDOFF))
    return std::nullopt;
  if (!File::Delete(HANDOFF))
    return std::nullopt;
  const auto* section = ini.GetSection("Runtime");
  int version = 0;
  Request request;
  if (!section || !section->Get("Version", &version) || version != 1 ||
      !ReadString(*section, "Launcher", &request.launcher_path))
    return std::nullopt;
  request.launcher_path = Updater::ResolveLauncherPath(request.launcher_path);
  if (request.launcher_path.find("/.emu/") != std::string::npos)
    return std::nullopt;
  LaunchRequest game;
  if (!ReadString(*section, "Path", &game.path) || !ReadString(*section, "GameID", &game.game_id) ||
      !ReadString(*section, "GameConfig", &game.game_config_path) ||
      !ReadString(*section, "LibraryID", &game.library_id))
    return std::nullopt;
  section->Get("Revision", &game.revision, 0u);
  section->Get("GameShare", &game.gameshare, false);
  u64 title = 0;
  if (section->Get("NandTitle", &title) && title != 0)
    game.nand_title = title;
  if (game.path.empty() && !game.nand_title)
    return std::nullopt;
  request.game = std::move(game);
  return request;
}

bool ReturnToLauncher(const std::string& launcher_path, const std::string& message)
{
  const std::string path = Updater::ResolveLauncherPath(launcher_path);
  if (!envHasNextLoad() || path.find("/.emu/") != std::string::npos)
    return false;
  Common::IniFile ini;
  ini.GetOrCreateSection("Return")->Set("Message", Encode(message));
  if (!SaveAtomic(ini, RETURN_FILE))
  {
    File::Delete(RETURN_FILE);
    std::fprintf(stderr, "[Dolphin Switch] Could not save the runtime return message\n");
  }
  const std::string arguments = fmt::format("\"{}\" {}", path, RETURN_ARGUMENT);
  return R_SUCCEEDED(envSetNextLoad(path.c_str(), arguments.c_str()));
}

std::string ReadReturnMessage()
{
  Common::IniFile ini;
  std::string message;
  if (ini.Load(RETURN_FILE))
  {
    if (const auto* section = ini.GetSection("Return"))
      (void)ReadString(*section, "Message", &message);
    File::Delete(RETURN_FILE);
  }
  return message;
}

}
