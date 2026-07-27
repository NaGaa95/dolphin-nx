// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/Enums.h"

namespace DolphinSwitch::Tools
{
enum class Result
{
  Success,
  Cancelled,
  NotFound,
  InvalidData,
  Duplicate,
  NoSpace,
  Unsupported,
  IoError,
};

const char* ResultMessage(Result result);

enum class ModType
{
  Patch,
  ActionReplay,
  Gecko,
};

struct ModEntry
{
  ModType type = ModType::Patch;
  std::size_t index = 0;
  std::string name;
  bool enabled = false;
};

std::vector<ModEntry> LoadGameMods(const std::string& game_id, u16 revision);
Result SetGameModEnabled(const std::string& game_id, u16 revision, ModType type,
                         std::size_t index, bool enabled, std::string* error = nullptr);
Result DownloadGeckoCodes(const std::string& game_id, const std::string& game_tdb_id,
                          u16 revision, std::size_t* added, std::string* error = nullptr);

struct GCSaveEntry
{
  u8 directory_index = 0;
  std::string title;
  std::string game_code;
  u16 blocks = 0;
};

std::string GetDefaultMemcardPath(int slot);
std::vector<GCSaveEntry> ListGCSaves(const std::string& card_path, std::string* error = nullptr);
Result ImportGCSave(const std::string& card_path, const std::string& save_path,
                    std::string* error = nullptr);
Result ExportGCSave(const std::string& card_path, u8 directory_index,
                    const std::string& destination, std::string* error = nullptr);
Result DeleteGCSave(const std::string& card_path, u8 directory_index,
                    std::string* error = nullptr);

struct WiiSaveEntry
{
  u64 title_id = 0;
  std::string name;
  std::string description;
};

std::vector<WiiSaveEntry> ListWiiSaves();
Result ImportWiiSave(const std::string& data_bin_path, bool overwrite,
                     std::string* error = nullptr);
Result ExportWiiSave(u64 title_id, const std::string& destination,
                     std::string* error = nullptr);
Result ExportAllWiiSaves(const std::string& destination, std::size_t* count,
                         std::string* error = nullptr);
Result DeleteWiiSave(u64 title_id, std::string* error = nullptr);

struct InstalledTitle
{
  u64 title_id = 0;
  std::string name;
  std::string game_id;
  std::string game_tdb_id;
  u16 revision = 0;
  std::int64_t modified = 0;
  DiscIO::Region region = DiscIO::Region::Unknown;
  bool system_title = false;
  bool bootable = false;
};

std::vector<InstalledTitle> ListInstalledWiiTitles();
Result InstallWAD(const std::string& wad_path, std::string* error = nullptr);
Result UninstallWiiTitle(u64 title_id, std::string* error = nullptr);

struct NANDStatus
{
  bool bad = false;
  std::size_t titles_to_remove = 0;
  u64 used_clusters_user = 0;
  u64 used_clusters_system = 0;
  u64 used_inodes_user = 0;
  u64 used_inodes_system = 0;
};

NANDStatus CheckNAND();
Result BackupNAND(const std::string& destination, std::string* error = nullptr);
Result RepairNAND(std::string* error = nullptr);
Result ImportBootMiiNAND(const std::string& nand_path, const std::string& keys_path,
                         std::string* error = nullptr);

struct NANDCertificateSource
{
  std::string path;
  std::string keys_path;
};

Result ExtractNANDCertificates(const NANDCertificateSource& source,
                               std::string* error = nullptr);

struct WiiNetworkStatus
{
  bool network_config_present = false;
  bool client_certificate_present = false;
  bool client_private_key_present = false;
};

WiiNetworkStatus GetWiiNetworkStatus();
Result ResetWiiNetworkConfiguration(std::string* error = nullptr);

Result CreateRiivolutionPreset(const std::string& base_game_path, const std::string& game_id,
                               u16 revision, const std::string& xml_path,
                               const std::string& output_path, std::string* error = nullptr);
}  // namespace DolphinSwitch::Tools
