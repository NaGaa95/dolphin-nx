// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/DolphinTools.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include <sys/stat.h>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/NandPaths.h"
#include "Common/ScopeGuard.h"
#include "Core/ActionReplay.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardUtils.h"
#include "Core/HW/WiiSave.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Network/NCD/WiiNetConfig.h"
#include "Core/IOS/Uids.h"
#include "Core/PatchEngine.h"
#include "Core/TitleDatabase.h"
#include "Core/WiiUtils.h"
#include "DiscIO/GameModDescriptor.h"
#include "DiscIO/NANDImporter.h"
#include "DiscIO/RiivolutionParser.h"
#include "DiscIO/WiiSaveBanner.h"

namespace DolphinSwitch::Tools
{
namespace
{
void SetError(std::string* error, std::string message)
{
  if (error)
    *error = std::move(message);
}

std::string GetLocalGameIniPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGS_IDX) + game_id + ".ini";
}

bool IsSafeIdentifier(std::string_view value)
{
  return !value.empty() && value.size() <= 32 &&
         std::ranges::all_of(value, [](unsigned char c) {
           return std::isalnum(c) || c == '-' || c == '_';
         });
}

Common::IniFile LoadEditableLocalGameIni(const std::string& game_id)
{
  Common::IniFile local;
  local.Load(GetLocalGameIniPath(game_id));
  return local;
}

Result MapWiiSaveResult(WiiSave::CopyResult result)
{
  switch (result)
  {
  case WiiSave::CopyResult::Success:
    return Result::Success;
  case WiiSave::CopyResult::Cancelled:
    return Result::Cancelled;
  case WiiSave::CopyResult::CorruptedSource:
    return Result::InvalidData;
  case WiiSave::CopyResult::TitleMissing:
    return Result::NotFound;
  case WiiSave::CopyResult::Error:
  case WiiSave::CopyResult::NumberOfEntries:
    return Result::IoError;
  }
  return Result::IoError;
}

std::optional<u32> ParseHexComponent(std::string_view text)
{
  if (text.size() != 8)
    return std::nullopt;

  u32 value = 0;
  const auto [end, parse_error] =
      std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (parse_error != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

std::string PrintableBytes(std::span<const u8> bytes)
{
  std::string result;
  result.reserve(bytes.size());
  for (const u8 byte : bytes)
    result.push_back(std::isprint(byte) ? static_cast<char>(byte) : '?');
  return result;
}

std::string JoinPath(std::string left, std::string_view right)
{
  if (!left.empty() && left.back() != '/' && left.back() != '\\')
    left += '/';
  left.append(right);
  return left;
}

bool CopyDirectoryTree(const std::string& source, const std::string& destination)
{
  const File::FSTEntry tree = File::ScanDirectoryTree(source, true);
  if (!tree.isDirectory || !File::CreateDirs(destination))
    return false;

  const auto copy_node = [&](const auto& self, const File::FSTEntry& node,
                             const std::string& destination_root) -> bool {
    for (const File::FSTEntry& child : node.children)
    {
      const std::string destination_path = JoinPath(destination_root, child.virtualName);
      if (child.isDirectory)
      {
        if (!File::CreateDirs(destination_path) || !self(self, child, destination_path))
          return false;
      }
      else
      {
        if (!File::CreateFullPath(destination_path) ||
            !File::CopyRegularFile(child.physicalName, destination_path))
        {
          return false;
        }
      }
    }
    return true;
  };

  return copy_node(copy_node, tree, destination);
}

std::string NormalizedPathForComparison(std::string path)
{
  std::replace(path.begin(), path.end(), '\\', '/');
  while (!path.empty() && path.back() == '/')
    path.pop_back();
  std::transform(path.begin(), path.end(), path.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return path;
}

constexpr u64 BOOTMII_NAND_SIZE = 0x21000000;
constexpr u64 BOOTMII_KEYS_SIZE = 0x400;
constexpr std::array<std::string_view, 3> NAND_CERTIFICATE_FILES = {
    "clientca.pem", "clientcakey.pem", "rootca.pem"};
std::mutex s_nand_certificate_mutex;

bool HasRequiredCertificateMaterial(const std::string& root)
{
  return File::IsFile(JoinPath(root, NAND_CERTIFICATE_FILES[0])) &&
         File::GetSize(JoinPath(root, NAND_CERTIFICATE_FILES[0])) > 0 &&
         File::IsFile(JoinPath(root, NAND_CERTIFICATE_FILES[1])) &&
         File::GetSize(JoinPath(root, NAND_CERTIFICATE_FILES[1])) > 0;
}

Result ValidateBootMiiBackup(const std::string& nand_path, const std::string& keys_path,
                             std::string* error)
{
  if (!File::IsFile(nand_path))
  {
    SetError(error, "The selected BootMii NAND image does not exist.");
    return Result::NotFound;
  }

  const u64 size = File::GetSize(nand_path);
  if (size != BOOTMII_NAND_SIZE && size != BOOTMII_NAND_SIZE + BOOTMII_KEYS_SIZE)
  {
    SetError(error, "The selected file is not a 528 MiB BootMii NAND image.");
    return Result::InvalidData;
  }
  if (size == BOOTMII_NAND_SIZE &&
      (keys_path.empty() || File::GetSize(keys_path) != BOOTMII_KEYS_SIZE))
  {
    SetError(error, "A separate 1024-byte keys.bin is required for this NAND image.");
    return Result::InvalidData;
  }
  return Result::Success;
}

Result PrepareCertificateStagingDirectory(std::string* staging_root, std::string* error)
{
  *staging_root = JoinPath(File::GetUserPath(D_CACHE_IDX), "NANDCertificateStaging");
  const std::string normalized_staging = NormalizedPathForComparison(*staging_root);
  const std::string normalized_active =
      NormalizedPathForComparison(File::GetUserPath(D_WIIROOT_IDX));
  if (normalized_staging.empty() || normalized_staging == normalized_active ||
      normalized_staging.starts_with(normalized_active + "/"))
  {
    SetError(error, "The certificate staging directory overlaps the active Wii NAND.");
    return Result::InvalidData;
  }

  if ((File::Exists(*staging_root) && !File::DeleteDirRecursively(*staging_root)) ||
      !File::CreateDirs(*staging_root))
  {
    SetError(error, "Dolphin could not create a temporary certificate staging directory.");
    return Result::IoError;
  }
  return Result::Success;
}

Result InstallCertificateMaterial(const std::string& source_root, std::string* error)
{
  if (!HasRequiredCertificateMaterial(source_root))
  {
    SetError(error, "The selected NAND does not contain a usable Wii SSL client certificate and "
                    "private key.");
    return Result::NotFound;
  }

  const std::string active_root = File::GetUserPath(D_WIIROOT_IDX);
  if (!File::CreateDirs(active_root))
  {
    SetError(error, "The active Wii NAND directory could not be created.");
    return Result::IoError;
  }

  std::vector<std::pair<std::string, std::string>> staged_files;
  const auto remove_staged_files = [&staged_files] {
    for (const auto& [temporary, destination] : staged_files)
    {
      (void)destination;
      File::Delete(temporary, File::IfAbsentBehavior::NoConsoleWarning);
    }
  };
  for (const std::string_view filename : NAND_CERTIFICATE_FILES)
  {
    const std::string source = JoinPath(source_root, filename);
    if (!File::IsFile(source) || File::GetSize(source) == 0)
      continue;

    const std::string destination = JoinPath(active_root, filename);
    const std::string temporary = destination + ".import";
    if (File::Exists(temporary))
      File::Delete(temporary, File::IfAbsentBehavior::NoConsoleWarning);
    if (!File::CopyRegularFile(source, temporary))
    {
      File::Delete(temporary, File::IfAbsentBehavior::NoConsoleWarning);
      remove_staged_files();
      SetError(error, "Wii certificate material could not be staged in the active NAND.");
      return Result::IoError;
    }
    staged_files.emplace_back(temporary, destination);
  }

  for (const auto& [temporary, destination] : staged_files)
  {
    if (!File::RenameSync(temporary, destination))
    {
      remove_staged_files();
      SetError(error, "Wii certificate material could not be committed to the active NAND.");
      return Result::IoError;
    }
  }
  return Result::Success;
}

Result OpenMemoryCard(const std::string& card_path, std::optional<Memcard::GCMemcard>* card,
                      std::string* error)
{
  auto [issues, opened] = Memcard::GCMemcard::Open(card_path);
  if (!opened || issues.HasCriticalErrors() || !opened->IsValid())
  {
    SetError(error, "The memory card is missing, unreadable, or corrupt.");
    return Result::InvalidData;
  }
  *card = std::move(opened);
  return Result::Success;
}

std::string GetRiivolutionRoot(const std::string& xml_path)
{
  std::string normalized = xml_path;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  std::string lowercase = normalized;
  std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  const std::size_t marker = lowercase.rfind("/riivolution/");
  if (marker != std::string::npos)
    return normalized.substr(0, marker);

  const std::size_t slash = normalized.find_last_of('/');
  return slash == std::string::npos ? std::string{"."} : normalized.substr(0, slash);
}
}  // namespace

const char* ResultMessage(Result result)
{
  switch (result)
  {
  case Result::Success:
    return "Success";
  case Result::Cancelled:
    return "Cancelled";
  case Result::NotFound:
    return "Not found";
  case Result::InvalidData:
    return "Invalid or corrupt data";
  case Result::Duplicate:
    return "Already present";
  case Result::NoSpace:
    return "Not enough free space";
  case Result::Unsupported:
    return "Unsupported";
  case Result::IoError:
    return "Storage operation failed";
  }
  return "Unknown error";
}

std::vector<ModEntry> LoadGameMods(const std::string& game_id, u16 revision)
{
  if (!IsSafeIdentifier(game_id))
    return {};
  const Common::IniFile defaults = SConfig::LoadDefaultGameIni(game_id, revision);
  const Common::IniFile local = LoadEditableLocalGameIni(game_id);
  std::vector<ModEntry> result;

  std::vector<PatchEngine::Patch> patches;
  PatchEngine::LoadPatchSection("OnFrame", &patches, defaults, local);
  for (std::size_t i = 0; i < patches.size(); ++i)
  {
    const auto& patch = patches[i];
    result.push_back({ModType::Patch, i, patch.name, patch.enabled});
  }

  const std::vector<ActionReplay::ARCode> ar_codes = ActionReplay::LoadCodes(defaults, local);
  for (std::size_t i = 0; i < ar_codes.size(); ++i)
  {
    const auto& code = ar_codes[i];
    result.push_back({ModType::ActionReplay, i, code.name, code.enabled});
  }

  const std::vector<Gecko::GeckoCode> gecko_codes = Gecko::LoadCodes(defaults, local);
  for (std::size_t i = 0; i < gecko_codes.size(); ++i)
  {
    const auto& code = gecko_codes[i];
    result.push_back({ModType::Gecko, i, code.name, code.enabled});
  }
  return result;
}

Result SetGameModEnabled(const std::string& game_id, u16 revision, ModType type,
                         std::size_t index, bool enabled, std::string* error)
{
  if (!IsSafeIdentifier(game_id))
  {
    SetError(error, "This title has no game ID.");
    return Result::InvalidData;
  }

  const Common::IniFile defaults = SConfig::LoadDefaultGameIni(game_id, revision);
  Common::IniFile local = LoadEditableLocalGameIni(game_id);
  switch (type)
  {
  case ModType::Patch:
  {
    std::vector<PatchEngine::Patch> codes;
    PatchEngine::LoadPatchSection("OnFrame", &codes, defaults, local);
    if (index >= codes.size())
      return Result::NotFound;
    codes[index].enabled = enabled;
    PatchEngine::SavePatchSection(&local, codes);
    break;
  }
  case ModType::ActionReplay:
  {
    std::vector<ActionReplay::ARCode> codes = ActionReplay::LoadCodes(defaults, local);
    if (index >= codes.size())
      return Result::NotFound;
    codes[index].enabled = enabled;
    ActionReplay::SaveCodes(&local, codes);
    break;
  }
  case ModType::Gecko:
  {
    std::vector<Gecko::GeckoCode> codes = Gecko::LoadCodes(defaults, local);
    if (index >= codes.size())
      return Result::NotFound;
    codes[index].enabled = enabled;
    Gecko::SaveCodes(local, codes);
    break;
  }
  }

  const std::string path = GetLocalGameIniPath(game_id);
  if (!File::CreateFullPath(path) || !local.Save(path))
  {
    SetError(error, "Could not write the per-game INI file.");
    return Result::IoError;
  }
  return Result::Success;
}

Result DownloadGeckoCodes(const std::string& game_id, const std::string& game_tdb_id, u16 revision,
                          std::size_t* added, std::string* error)
{
  if (added)
    *added = 0;
  if (!IsSafeIdentifier(game_id) || !IsSafeIdentifier(game_tdb_id))
    return Result::InvalidData;

  const auto downloaded = Gecko::DownloadCodes(game_tdb_id);
  if (!downloaded)
  {
    SetError(error, fmt::format("GameTDB returned network error {}.", downloaded.error()));
    return Result::IoError;
  }

  const Common::IniFile defaults = SConfig::LoadDefaultGameIni(game_id, revision);
  Common::IniFile local = LoadEditableLocalGameIni(game_id);
  std::vector<Gecko::GeckoCode> codes = Gecko::LoadCodes(defaults, local);
  std::size_t count = 0;
  for (Gecko::GeckoCode code : *downloaded)
  {
    const bool duplicate = std::any_of(codes.begin(), codes.end(), [&](const auto& existing) {
      return existing.name == code.name && existing.codes == code.codes;
    });
    if (!duplicate)
    {
      code.user_defined = true;
      codes.emplace_back(std::move(code));
      ++count;
    }
  }

  if (count != 0)
  {
    Gecko::SaveCodes(local, codes);
    const std::string path = GetLocalGameIniPath(game_id);
    if (!File::CreateFullPath(path) || !local.Save(path))
    {
      SetError(error, "Downloaded codes could not be saved.");
      return Result::IoError;
    }
  }
  if (added)
    *added = count;
  return count == 0 ? Result::Duplicate : Result::Success;
}

std::string GetDefaultMemcardPath(int slot)
{
  const ExpansionInterface::Slot exi_slot =
      slot == 1 ? ExpansionInterface::Slot::B : ExpansionInterface::Slot::A;
  return Config::GetMemcardPath(exi_slot, Config::Get(Config::MAIN_FALLBACK_REGION));
}

std::vector<GCSaveEntry> ListGCSaves(const std::string& card_path, std::string* error)
{
  std::optional<Memcard::GCMemcard> card;
  if (OpenMemoryCard(card_path, &card, error) != Result::Success)
    return {};

  std::vector<GCSaveEntry> result;
  result.reserve(card->GetNumFiles());
  for (u8 file_number = 0; file_number < card->GetNumFiles(); ++file_number)
  {
    const u8 index = card->GetFileIndex(file_number);
    const auto directory_entry = card->GetDEntry(index);
    if (!directory_entry)
      continue;
    const auto comments = card->GetSaveComments(index);
    result.push_back({index,
                      comments ? comments->first : Memcard::GenerateFilename(*directory_entry),
                      PrintableBytes(directory_entry->m_gamecode),
                      static_cast<u16>(directory_entry->m_block_count)});
  }
  return result;
}

Result ImportGCSave(const std::string& card_path, const std::string& save_path, std::string* error)
{
  const auto save = Memcard::ReadSavefile(save_path);
  if (!std::holds_alternative<Memcard::Savefile>(save))
  {
    SetError(error, "The selected GCI/GCS/SAV file is invalid.");
    return Result::InvalidData;
  }

  std::optional<Memcard::GCMemcard> card;
  const Result opened = OpenMemoryCard(card_path, &card, error);
  if (opened != Result::Success)
    return opened;

  switch (card->ImportFile(std::get<Memcard::Savefile>(save)))
  {
  case Memcard::GCMemcardImportFileRetVal::SUCCESS:
    if (!card->Save())
    {
      SetError(error, "The updated memory card could not be written.");
      return Result::IoError;
    }
    return Result::Success;
  case Memcard::GCMemcardImportFileRetVal::OUTOFDIRENTRIES:
  case Memcard::GCMemcardImportFileRetVal::OUTOFBLOCKS:
    return Result::NoSpace;
  case Memcard::GCMemcardImportFileRetVal::TITLEPRESENT:
    return Result::Duplicate;
  case Memcard::GCMemcardImportFileRetVal::NOMEMCARD:
    return Result::NotFound;
  }
  return Result::IoError;
}

Result ExportGCSave(const std::string& card_path, u8 directory_index,
                    const std::string& destination, std::string* error)
{
  std::optional<Memcard::GCMemcard> card;
  const Result opened = OpenMemoryCard(card_path, &card, error);
  if (opened != Result::Success)
    return opened;
  const auto save = card->ExportFile(directory_index);
  if (!save)
    return Result::NotFound;

  std::string output = destination;
  if (File::IsDirectory(output))
    output = JoinPath(output, Memcard::GenerateFilename(save->dir_entry) + ".gci");
  if (!File::CreateFullPath(output) ||
      !Memcard::WriteSavefile(output, *save, Memcard::SavefileFormat::GCI))
  {
    SetError(error, "Could not export the GameCube save.");
    return Result::IoError;
  }
  return Result::Success;
}

Result DeleteGCSave(const std::string& card_path, u8 directory_index, std::string* error)
{
  std::optional<Memcard::GCMemcard> card;
  const Result opened = OpenMemoryCard(card_path, &card, error);
  if (opened != Result::Success)
    return opened;
  if (!card->GetDEntry(directory_index))
    return Result::NotFound;
  if (card->RemoveFile(directory_index) != Memcard::GCMemcardRemoveFileRetVal::SUCCESS ||
      !card->Save())
  {
    SetError(error, "The save could not be removed from the memory card.");
    return Result::IoError;
  }
  return Result::Success;
}

std::vector<WiiSaveEntry> ListWiiSaves()
{
  std::vector<WiiSaveEntry> result;
  const File::FSTEntry title_root =
      File::ScanDirectoryTree(JoinPath(File::GetUserPath(D_WIIROOT_IDX), "title"), false);
  for (const File::FSTEntry& high_directory : title_root.children)
  {
    const std::optional<u32> high = ParseHexComponent(high_directory.virtualName);
    if (!high_directory.isDirectory || !high)
      continue;
    const File::FSTEntry high_contents =
        File::ScanDirectoryTree(high_directory.physicalName, false);
    for (const File::FSTEntry& low_directory : high_contents.children)
    {
      const std::optional<u32> low = ParseHexComponent(low_directory.virtualName);
      if (!low_directory.isDirectory || !low)
        continue;
      const u64 title_id = (static_cast<u64>(*high) << 32) | *low;
      const std::string data_path = JoinPath(low_directory.physicalName, "data");
      if (!File::IsDirectory(data_path) ||
          File::ScanDirectoryTree(data_path, false).children.empty())
      {
        continue;
      }

      DiscIO::WiiSaveBanner banner(title_id);
      const std::string fallback = fmt::format("{:016x}", title_id);
      result.push_back({title_id, banner.IsValid() ? banner.GetName() : fallback,
                        banner.IsValid() ? banner.GetDescription() : std::string{}});
    }
  }
  std::sort(result.begin(), result.end(),
            [](const auto& left, const auto& right) { return left.name < right.name; });
  return result;
}

Result ImportWiiSave(const std::string& data_bin_path, bool overwrite, std::string* error)
{
  const Result result = MapWiiSaveResult(
      WiiSave::Import(data_bin_path, [overwrite] { return overwrite; }));
  if (result != Result::Success)
    SetError(error, ResultMessage(result));
  return result;
}

Result ExportWiiSave(u64 title_id, const std::string& destination, std::string* error)
{
  if (!File::CreateDirs(destination))
    return Result::IoError;
  const Result result = MapWiiSaveResult(WiiSave::Export(title_id, destination));
  if (result != Result::Success)
    SetError(error, "The Wii save could not be exported as data.bin.");
  return result;
}

Result ExportAllWiiSaves(const std::string& destination, std::size_t* count, std::string* error)
{
  if (!File::CreateDirs(destination))
  {
    SetError(error, "The export directory could not be created.");
    return Result::IoError;
  }
  const std::size_t exported = WiiSave::ExportAll(destination);
  if (count)
    *count = exported;
  if (exported == 0)
    return ListWiiSaves().empty() ? Result::NotFound : Result::IoError;
  return Result::Success;
}

Result DeleteWiiSave(u64 title_id, std::string* error)
{
  const std::string host_data_path =
      Common::GetTitleDataPath(title_id, Common::FromWhichRoot::Configured);
  const bool existed = File::IsDirectory(host_data_path);
  IOS::HLE::Kernel ios;
  const IOS::HLE::FS::ResultCode result =
      ios.GetFS()->Delete(IOS::PID_KERNEL, IOS::PID_KERNEL, Common::GetTitleDataPath(title_id));
  if (result != IOS::HLE::FS::ResultCode::Success &&
      result != IOS::HLE::FS::ResultCode::NotFound)
  {
    SetError(error, "The Wii save directory could not be removed.");
    return Result::IoError;
  }
  if (File::IsDirectory(host_data_path) && !File::DeleteDirRecursively(host_data_path))
  {
    SetError(error, "The Wii save directory could not be removed.");
    return Result::IoError;
  }
  if (!existed && result == IOS::HLE::FS::ResultCode::NotFound)
    return Result::NotFound;
  return Result::Success;
}

std::vector<InstalledTitle> ListInstalledWiiTitles()
{
  IOS::HLE::Kernel ios;
  Core::TitleDatabase title_database;
  const DiscIO::Language language = SConfig::GetInstance().GetCurrentLanguage(true);
  std::vector<InstalledTitle> result;
  for (const u64 title_id : ios.GetESCore().GetInstalledTitles())
  {
    const auto content =
        ios.GetFS()->ReadDirectory(IOS::PID_KERNEL, IOS::PID_KERNEL,
                                   Common::GetTitleContentPath(title_id));
    if (!content || std::ranges::none_of(*content, [](const std::string& name) {
          return name != "title.tmd";
        }))
    {
      continue;
    }

    std::string name = title_database.GetChannelName(title_id, language);
    if (name.empty())
    {
      DiscIO::WiiSaveBanner banner(title_id);
      if (banner.IsValid())
        name = banner.GetName();
    }
    if (name.empty())
      name = fmt::format("Title {:016x}", title_id);

    InstalledTitle title;
    title.title_id = title_id;
    title.name = std::move(name);
    title.system_title = static_cast<u32>(title_id >> 32) == 1;
    const IOS::ES::TMDReader tmd = ios.GetESCore().FindInstalledTMD(title_id);
    if (tmd.IsValid())
    {
      title.game_id = tmd.GetGameID();
      title.game_tdb_id = tmd.GetGameTDBID();
      title.revision = tmd.GetTitleVersion();
      title.region = tmd.GetRegion();
      title.bootable = IOS::ES::IsChannel(title_id);
    }
    struct stat info{};
    if (::stat(Common::GetTMDFileName(title_id, Common::FromWhichRoot::Configured).c_str(),
               &info) == 0)
    {
      title.modified = info.st_mtime;
    }
    result.emplace_back(std::move(title));
  }
  std::sort(result.begin(), result.end(),
            [](const auto& left, const auto& right) { return left.name < right.name; });
  return result;
}

Result InstallWAD(const std::string& wad_path, std::string* error)
{
  if (!File::IsFile(wad_path))
    return Result::NotFound;
  if (!WiiUtils::InstallWAD(wad_path))
  {
    SetError(error, "Dolphin rejected the WAD or could not write it to NAND.");
    return Result::InvalidData;
  }
  return Result::Success;
}

Result UninstallWiiTitle(u64 title_id, std::string* error)
{
  if (!WiiUtils::IsTitleInstalled(title_id))
    return Result::NotFound;
  if (!WiiUtils::UninstallTitle(title_id))
  {
    SetError(error, "The installed title could not be removed. Its save was left intact.");
    return Result::IoError;
  }
  return Result::Success;
}

NANDStatus CheckNAND()
{
  IOS::HLE::Kernel ios;
  const WiiUtils::NANDCheckResult status = WiiUtils::CheckNAND(ios);
  return {status.bad,
          status.titles_to_remove.size(),
          status.used_clusters_user,
          status.used_clusters_system,
          status.used_inodes_user,
          status.used_inodes_system};
}

Result BackupNAND(const std::string& destination, std::string* error)
{
  const std::string source = File::GetUserPath(D_WIIROOT_IDX);
  const std::string normalized_source = NormalizedPathForComparison(source);
  const std::string normalized_destination = NormalizedPathForComparison(destination);
  if (normalized_destination.empty() || normalized_destination == normalized_source ||
      normalized_destination.starts_with(normalized_source + "/") ||
      normalized_source.starts_with(normalized_destination + "/"))
  {
    SetError(error, "The backup destination must be outside the active Wii NAND.");
    return Result::InvalidData;
  }
  if (!CopyDirectoryTree(source, destination))
  {
    SetError(error, "The NAND backup was incomplete. Check free space and the destination.");
    return Result::IoError;
  }
  return Result::Success;
}

Result RepairNAND(std::string* error)
{
  IOS::HLE::Kernel ios;
  if (!WiiUtils::RepairNAND(ios))
  {
    SetError(error, "Dolphin could not repair every NAND inconsistency.");
    return Result::IoError;
  }
  return Result::Success;
}

Result ImportBootMiiNAND(const std::string& nand_path, const std::string& keys_path,
                         std::string* error)
{
  if (const Result validation = ValidateBootMiiBackup(nand_path, keys_path, error);
      validation != Result::Success)
    return validation;

  DiscIO::NANDImporter importer;
  bool completed = false;
  importer.ImportNANDBin(
      nand_path,
      [&completed](DiscIO::NANDImporter::Step step, int current, int maximum) {
        if (step == DiscIO::NANDImporter::Step::Extracting && maximum > 0 && current == maximum)
          completed = true;
        return false;
      },
      [&keys_path] { return keys_path; });
  if (!completed)
  {
    SetError(error, "The NAND image could not be extracted.");
    return Result::IoError;
  }
  return Result::Success;
}

Result ExtractNANDCertificates(const NANDCertificateSource& source, std::string* error)
{
  std::lock_guard lock{s_nand_certificate_mutex};
  if (const Result validation = ValidateBootMiiBackup(source.path, source.keys_path, error);
      validation != Result::Success)
    return validation;

  std::string staging_root;
  if (const Result staging_result = PrepareCertificateStagingDirectory(&staging_root, error);
      staging_result != Result::Success)
    return staging_result;
  Common::ScopeGuard staging_guard([&] {
    if (File::Exists(staging_root))
      File::DeleteDirRecursively(staging_root);
  });

  DiscIO::NANDImporter importer(staging_root);
  bool completed = false;
  importer.ImportNANDBin(
      source.path,
      [&completed](DiscIO::NANDImporter::Step step, int current, int maximum) {
        if (step == DiscIO::NANDImporter::Step::Extracting && maximum > 0 && current == maximum)
          completed = true;
        return false;
      },
      [&source] { return source.keys_path; });
  if (!completed || !HasRequiredCertificateMaterial(staging_root))
  {
    SetError(error, "The BootMii backup could not be decrypted or does not contain usable Wii "
                    "SSL certificate material.");
    return Result::InvalidData;
  }

  return InstallCertificateMaterial(staging_root, error);
}

WiiNetworkStatus GetWiiNetworkStatus()
{
  const std::string root = File::GetUserPath(D_WIIROOT_IDX);
  return {File::IsFile(JoinPath(root, "shared2/sys/net/02/config.dat")),
          File::IsFile(JoinPath(root, "clientca.pem")),
          File::IsFile(JoinPath(root, "clientcakey.pem"))};
}

Result ResetWiiNetworkConfiguration(std::string* error)
{
  IOS::HLE::Kernel ios;
  IOS::HLE::Net::WiiNetConfig config;
  config.ResetConfig(ios.GetFS().get());
  if (!GetWiiNetworkStatus().network_config_present)
  {
    SetError(error, "The Wii wired/DHCP network profile could not be written to NAND.");
    return Result::IoError;
  }
  return Result::Success;
}

Result CreateRiivolutionPreset(const std::string& base_game_path, const std::string& game_id,
                               u16 revision, const std::string& xml_path,
                               const std::string& output_path, std::string* error)
{
  if (!File::IsFile(base_game_path) || !IsSafeIdentifier(game_id) || output_path.empty())
  {
    SetError(error, "The base game, game ID, or descriptor path is invalid.");
    return Result::InvalidData;
  }
  const std::string normalized_output = NormalizedPathForComparison(output_path);
  if (normalized_output == NormalizedPathForComparison(base_game_path) ||
      normalized_output == NormalizedPathForComparison(xml_path))
  {
    SetError(error, "The launch descriptor must not overwrite the game or Riivolution XML.");
    return Result::InvalidData;
  }
  std::optional<DiscIO::Riivolution::Disc> disc = DiscIO::Riivolution::ParseFile(xml_path);
  if (!disc)
  {
    SetError(error, "The selected file is not a valid Riivolution XML.");
    return Result::InvalidData;
  }
  if (!disc->IsValidForGame(game_id, revision, std::nullopt))
  {
    SetError(error, "This Riivolution patch does not match the selected game.");
    return Result::InvalidData;
  }

  const std::string root = GetRiivolutionRoot(xml_path);
  if (game_id.size() >= 4)
  {
    const std::string config_path =
        JoinPath(root, fmt::format("riivolution/config/{}.xml", game_id.substr(0, 4)));
    if (const auto config = DiscIO::Riivolution::ParseConfigFile(config_path))
      DiscIO::Riivolution::ApplyConfigDefaults(&*disc, *config);
  }

  DiscIO::GameModDescriptor descriptor;
  descriptor.base_file = base_game_path;
  descriptor.display_name = "Riivolution - " + game_id;
  DiscIO::GameModDescriptorRiivolution riivolution;
  DiscIO::GameModDescriptorRiivolutionPatch patch;
  patch.xml = xml_path;
  patch.root = root;
  for (const auto& section : disc->m_sections)
  {
    for (const auto& option : section.m_options)
    {
      if (option.m_selected_choice == 0 || option.m_selected_choice > option.m_choices.size())
        continue;
      patch.options.push_back(
          {section.m_name, option.m_id, option.m_name, option.m_selected_choice});
    }
  }
  riivolution.patches.emplace_back(std::move(patch));
  descriptor.riivolution = std::move(riivolution);

  if (!File::CreateFullPath(output_path) ||
      !DiscIO::WriteGameModDescriptorFile(output_path, descriptor, true))
  {
    SetError(error, "The Riivolution launch descriptor could not be written.");
    return Result::IoError;
  }
  return Result::Success;
}
}  // namespace DolphinSwitch::Tools
