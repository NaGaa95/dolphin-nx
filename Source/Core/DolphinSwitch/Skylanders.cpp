// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/Skylanders.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <utility>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/IniFile.h"
#include "Core/Config/MainSettings.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/IOS/USB/Emulated/Skylanders/SkylanderFigure.h"
#include "Core/System.h"

namespace DolphinSwitch::Skylanders
{
namespace
{
namespace USB = IOS::HLE::USB;
static_assert(SLOT_COUNT == MAX_SKYLANDERS);

struct PortalSlot
{
  Slot display;
  std::uint32_t serial = 0;
  bool valid = false;
  std::uint8_t portal_slot = 0xff;
};

std::mutex s_mutex;
std::array<PortalSlot, SLOT_COUNT> s_slots{};
bool s_session = false;

std::string StatePath()
{
  return File::GetUserPath(D_CONFIG_IDX) + "Skylanders.ini";
}

std::string FileName(const std::string& path)
{
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string ReadFigure(const std::string& path, PortalSlot* slot,
                       std::unique_ptr<USB::SkylanderFigure>* figure = nullptr)
{
  File::IOFile file(path, figure ? "r+b" : "rb");
  if (!file)
    return "Could not open figure: " + FileName(path);
  if (file.GetSize() != USB::FIGURE_SIZE)
    return "A Skylanders figure must be a 1024-byte dump: " + FileName(path);
  std::array<u8, USB::FIGURE_SIZE> data{};
  if (!file.ReadBytes(data.data(), data.size()))
    return "Could not read figure: " + FileName(path);
  const auto ids = Core::System::GetInstance().GetSkylanderPortal().CalculateIDs(data);
  const auto found = USB::list_skylanders.find(ids);
  slot->valid = true;
  slot->display.path = path;
  slot->display.name = found == USB::list_skylanders.end() ? FileName(path) : found->second.name;
  slot->serial = std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) |
                 (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
  if (figure)
    *figure = std::make_unique<USB::SkylanderFigure>(std::move(file));
  return {};
}

bool SaveSelections()
{
  const std::string path = StatePath();
  Common::IniFile ini;
  if ((File::Exists(path) && !ini.Load(path)) || !File::CreateFullPath(path))
    return false;
  auto* section = ini.GetOrCreateSection("Portal");
  for (std::size_t slot = 0; slot < SLOT_COUNT; ++slot)
    section->Set("Slot" + std::to_string(slot + 1), s_slots[slot].display.path);
  return ini.Save(path);
}

void RemoveLiveFigure(PortalSlot& slot)
{
  if (slot.portal_slot != 0xff)
    Core::System::GetInstance().GetSkylanderPortal().RemoveSkylander(slot.portal_slot);
  slot.portal_slot = 0xff;
  slot.display.loaded = false;
}

std::string ReadSelections()
{
  Common::IniFile ini;
  const auto state_path = StatePath();
  if (File::Exists(state_path) && !ini.Load(state_path))
    return "Could not read the Skylanders portal selections.";
  const auto* section = ini.GetSection("Portal");
  s_slots = {};
  if (!section)
    return {};
  for (std::size_t slot = 0; slot < SLOT_COUNT; ++slot)
  {
    std::string path;
    section->Get("Slot" + std::to_string(slot + 1), &path);
    if (path.empty())
      continue;
    if (!ReadFigure(path, &s_slots[slot]).empty())
      s_slots[slot].display = {path, FileName(path) + " (unavailable)", false};
  }
  return {};
}

std::string LoadLiveFigure(std::size_t index)
{
  PortalSlot input;
  std::unique_ptr<USB::SkylanderFigure> figure;
  if (auto error = ReadFigure(s_slots[index].display.path, &input, &figure); !error.empty())
    return error;
  for (std::size_t other = 0; other < SLOT_COUNT; ++other)
  {
    if (other != index && s_slots[other].display.loaded && s_slots[other].serial == input.serial)
      return "This figure is already on the portal.";
  }
  const auto actual =
      Core::System::GetInstance().GetSkylanderPortal().LoadSkylander(std::move(figure));
  if (actual == 0xff)
    return "The portal has no free figure slots.";
  input.portal_slot = actual;
  input.display.loaded = true;
  s_slots[index] = std::move(input);
  return {};
}

std::string LoadSelections()
{
  std::string first_error;
  Core::System::GetInstance().GetSkylanderPortal().Activate();
  for (std::size_t index = 0; index < SLOT_COUNT; ++index)
  {
    if (s_slots[index].display.path.empty() || s_slots[index].display.loaded)
      continue;
    const auto error = LoadLiveFigure(index);
    if (first_error.empty())
      first_error = error;
  }
  return first_error;
}
}  // namespace

std::string CollectionDirectory()
{
  std::string path = Config::Get(Config::MAIN_SKYLANDERS_PATH);
  if (path.empty())
    path = File::GetUserPath(D_USER_IDX) + "Skylanders";
  if (path.back() != '/')
    path += '/';
  return path;
}

std::string Refresh()
{
  std::lock_guard lock(s_mutex);
  return s_session ? std::string{} : ReadSelections();
}

std::array<Slot, SLOT_COUNT> GetSlots()
{
  std::lock_guard lock(s_mutex);
  std::array<Slot, SLOT_COUNT> result;
  for (std::size_t index = 0; index < SLOT_COUNT; ++index)
    result[index] = s_slots[index].display;
  return result;
}

std::vector<FigureFile> ListCollection(std::string* error)
{
  std::vector<FigureFile> files;
  const auto directory = CollectionDirectory();
  if (!File::CreateDirs(directory))
  {
    *error = "Could not open the Skylanders collection folder.";
    return files;
  }
  DIR* handle = ::opendir(directory.c_str());
  if (!handle)
  {
    *error = "Could not read the Skylanders collection folder.";
    return files;
  }
  while (auto* entry = ::readdir(handle))
  {
    const std::string name = entry->d_name;
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos)
      continue;
    std::string extension = name.substr(dot);
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return std::tolower(c); });
    if (extension != ".sky" && extension != ".bin" && extension != ".dmp" && extension != ".dump")
      continue;
    PortalSlot info;
    if (ReadFigure(directory + name, &info).empty())
      files.push_back({info.display.path, info.display.name + " / " + name});
  }
  ::closedir(handle);
  std::ranges::sort(files, {}, &FigureFile::name);
  return files;
}

const std::vector<CatalogEntry>& Catalog()
{
  static const auto entries = [] {
    std::vector<CatalogEntry> result;
    for (const auto& [ids, data] : USB::list_skylanders)
      result.push_back({ids.first, ids.second, static_cast<int>(data.game), data.name});
    std::ranges::sort(result, {}, &CatalogEntry::name);
    return result;
  }();
  return entries;
}

std::string SetEnabled(bool enabled)
{
  std::lock_guard lock(s_mutex);
  Config::SetBaseOrCurrent(Config::MAIN_EMULATE_SKYLANDER_PORTAL, enabled);
  Config::Save();
  if (!s_session)
    return {};
  if (enabled)
    return LoadSelections();
  for (auto& slot : s_slots)
    RemoveLiveFigure(slot);
  return {};
}

std::string SetFigure(std::size_t index, const std::string& path)
{
  std::lock_guard lock(s_mutex);
  if (index >= SLOT_COUNT)
    return "Invalid portal slot.";
  if (s_slots[index].display.path == path && (!s_session || s_slots[index].display.loaded))
    return {};
  PortalSlot input;
  if (auto error = ReadFigure(path, &input); !error.empty())
    return error;
  for (std::size_t other = 0; other < SLOT_COUNT; ++other)
  {
    if (other != index && s_slots[other].valid && s_slots[other].serial == input.serial)
      return "This figure is already assigned to another portal slot.";
  }
  const auto previous = s_slots[index].display.path;
  RemoveLiveFigure(s_slots[index]);
  s_slots[index] = std::move(input);
  if (s_session && Config::Get(Config::MAIN_EMULATE_SKYLANDER_PORTAL))
  {
    if (auto error = LoadLiveFigure(index); !error.empty())
    {
      s_slots[index] = {};
      if (!previous.empty())
      {
        ReadFigure(previous, &s_slots[index]);
        LoadLiveFigure(index);
      }
      return error;
    }
  }
  return SaveSelections() ?
             std::string{} :
             "Figure selected for this session, but the selection could not be saved.";
}

std::string ClearFigure(std::size_t index)
{
  std::lock_guard lock(s_mutex);
  if (index >= SLOT_COUNT)
    return "Invalid portal slot.";
  RemoveLiveFigure(s_slots[index]);
  s_slots[index] = {};
  return SaveSelections() ? std::string{} : "Figure removed, but the selection could not be saved.";
}

std::string CreateFigure(std::uint16_t id, std::uint16_t variant, const std::string& path)
{
  if (!USB::list_skylanders.contains({id, variant}))
    return "Unknown Skylander figure.";
  if (File::Exists(path))
    return "A file with this name already exists. Choose another name.";
  if (!File::CreateFullPath(path))
    return "Could not create the figure folder.";
  USB::SkylanderFigure figure{path};
  if (!figure.Create(id, variant))
    return "Could not create the figure file.";
  figure.Close();
  PortalSlot result;
  return ReadFigure(path, &result);
}

std::string BeginSession()
{
  std::lock_guard lock(s_mutex);
  for (auto& slot : s_slots)
    RemoveLiveFigure(slot);
  const auto error = ReadSelections();
  s_session = true;
  if (!error.empty())
    return error;
  return Config::Get(Config::MAIN_EMULATE_SKYLANDER_PORTAL) ? LoadSelections() : std::string{};
}

void EndSession()
{
  std::lock_guard lock(s_mutex);
  for (auto& slot : s_slots)
    RemoveLiveFigure(slot);
  s_session = false;
}
}  // namespace DolphinSwitch::Skylanders
