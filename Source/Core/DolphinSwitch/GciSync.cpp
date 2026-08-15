// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/GciSync.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <picojson.h>

#include "Common/Config/Config.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/JsonUtil.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "DolphinSwitch/Launcher.h"
#include "DolphinSwitch/Storage.h"

namespace DolphinSwitch
{
namespace
{
constexpr std::string_view SYNC_TEMP_SUFFIX = ".dolphin-gci-sync-tmp";
constexpr std::string_view SYNC_BACKUP_SUFFIX = ".dolphin-gci-sync-backup";
constexpr std::size_t MAX_SYNC_FILES = 4096;
constexpr u64 MAX_SYNC_BYTES = 1024ULL * 1024 * 1024;

struct FileState
{
  std::string relative_path;
  u64 size = 0;
  std::string sha1;
};

using Snapshot = std::map<std::string, FileState>;

struct Manifest
{
  bool exists = false;
  bool session_active = false;
  bool pending = false;
  Snapshot baseline;
};

enum class ActionKind
{
  CopyLocalToRemote,
  CopyRemoteToLocal,
  DeleteLocal,
  DeleteRemote,
};

struct SyncAction
{
  ActionKind kind;
  std::optional<FileState> local;
  std::optional<FileState> remote;
};

struct SyncPlan
{
  std::vector<SyncAction> actions;
  std::vector<std::string> conflicts;
};

std::string JoinPath(std::string left, std::string_view right)
{
  if (!left.empty() && left.back() != '/' && left.back() != '\\')
    left.push_back('/');
  left.append(right);
  return left;
}

std::string NormalizePath(std::string path)
{
  std::replace(path.begin(), path.end(), '\\', '/');
  while (path.size() > 1 && path.back() == '/')
    path.pop_back();
  return path;
}

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

bool IsHexDigest(std::string_view value)
{
  return value.size() == Common::SHA1::DIGEST_LEN * 2 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isxdigit(character) != 0;
         });
}

bool IsSafeRelativePath(std::string_view path)
{
  if (path.empty() || path.front() == '/' || path.front() == '\\' || path.find(':') != path.npos)
    return false;

  std::size_t offset = 0;
  while (offset <= path.size())
  {
    const std::size_t separator = path.find_first_of("/\\", offset);
    const std::string_view component = path.substr(
        offset, separator == path.npos ? path.size() - offset : separator - offset);
    if (component.empty() || component == "." || component == "..")
      return false;
    if (separator == path.npos)
      break;
    offset = separator + 1;
  }
  return true;
}

bool IsSyncArtifact(std::string_view path)
{
  return path.ends_with(SYNC_TEMP_SUFFIX) || path.ends_with(SYNC_BACKUP_SUFFIX);
}

std::optional<std::string> SmbIdFromPath(std::string_view path)
{
  const std::size_t colon = path.find(':');
  if (colon == path.npos)
    return std::nullopt;
  const std::string device = Lower(std::string(path.substr(0, colon)));
  if (!device.starts_with("dsmb_") || device.size() <= 5)
    return std::nullopt;
  return device.substr(5);
}

bool FlushDurably(File::IOFile* file)
{
  if (!file || !file->Flush())
    return false;
#ifdef _WIN32
  return _commit(_fileno(file->GetHandle())) == 0;
#else
  return ::fsync(fileno(file->GetHandle())) == 0;
#endif
}

bool RenameWithoutDeleting(const std::string& source, const std::string& destination)
{
  return std::rename(source.c_str(), destination.c_str()) == 0;
}

bool AtomicReplace(const std::string& temporary, const std::string& destination,
                   std::string* error)
{
  if (RenameWithoutDeleting(temporary, destination))
    return true;

  if (!File::IsFile(destination))
  {
    if (error)
      *error = "Could not atomically install " + destination + '.';
    return false;
  }

  const std::string backup = destination + std::string(SYNC_BACKUP_SUFFIX);
  File::Delete(backup, File::IfAbsentBehavior::NoConsoleWarning);
  if (!RenameWithoutDeleting(destination, backup))
  {
    if (error)
      *error = "Could not preserve the previous SMB save before replacing " + destination + '.';
    return false;
  }

  if (!RenameWithoutDeleting(temporary, destination))
  {
    RenameWithoutDeleting(backup, destination);
    if (error)
      *error = "Could not atomically replace " + destination + "; the previous copy was restored.";
    return false;
  }

  if (!File::Delete(backup, File::IfAbsentBehavior::NoConsoleWarning))
  {
    if (error)
      *error = "The new save was installed, but its synchronization backup could not be removed.";
    return false;
  }
  return true;
}

bool WriteAtomic(const std::string& destination, std::string_view contents, std::string* error)
{
  if (!File::CreateFullPath(destination))
  {
    if (error)
      *error = "Could not create the local GCI cache directory.";
    return false;
  }

  const std::string temporary = destination + std::string(SYNC_TEMP_SUFFIX);
  File::Delete(temporary, File::IfAbsentBehavior::NoConsoleWarning);
  File::IOFile output(temporary, "wb");
  if (!output || !output.WriteString(contents) || !FlushDurably(&output) || !output.Close())
  {
    File::Delete(temporary, File::IfAbsentBehavior::NoConsoleWarning);
    if (error)
      *error = "Could not durably write the local GCI synchronization journal.";
    return false;
  }
  return AtomicReplace(temporary, destination, error);
}

bool HashFile(const std::string& path, std::string relative_path, FileState* state,
              std::string* error)
{
  File::IOFile input(path, "rb");
  if (!input)
  {
    if (error)
      *error = "Could not read " + path + '.';
    return false;
  }

  const u64 size = input.GetSize();
  if (size == UINT64_MAX || size > MAX_SYNC_BYTES)
  {
    if (error)
      *error = "A GCI synchronization file is unexpectedly large: " + path;
    return false;
  }

  auto context = Common::SHA1::CreateContext();
  std::array<u8, 128 * 1024> buffer{};
  u64 bytes_read_total = 0;
  while (true)
  {
    const std::size_t bytes_read =
        std::fread(buffer.data(), sizeof(buffer.front()), buffer.size(), input.GetHandle());
    if (bytes_read != 0)
    {
      context->Update(buffer.data(), bytes_read);
      bytes_read_total += bytes_read;
    }
    if (bytes_read != buffer.size())
    {
      if (std::ferror(input.GetHandle()))
      {
        if (error)
          *error = "A GCI synchronization file changed or became unavailable while reading: " +
                   path;
        return false;
      }
      break;
    }
  }

  if (bytes_read_total != size)
  {
    if (error)
      *error = "A GCI synchronization file changed while it was being hashed: " + path;
    return false;
  }

  state->relative_path = std::move(relative_path);
  state->size = size;
  state->sha1 = Lower(Common::SHA1::DigestToString(context->Finish()));
  return true;
}

bool SameState(const FileState* left, const FileState* right)
{
  if (!left || !right)
    return left == right;
  return left->size == right->size && left->sha1 == right->sha1;
}

const FileState* FindState(const Snapshot& snapshot, const std::string& key)
{
  const auto iterator = snapshot.find(key);
  return iterator == snapshot.end() ? nullptr : &iterator->second;
}

bool RecoverAtomicArtifacts(const std::string& root, std::string* error)
{
  if (!File::IsDirectory(root))
    return true;

  const File::FSTEntry tree = File::ScanDirectoryTree(root, true);
  if (!tree.isDirectory)
  {
    if (error)
      *error = "Could not inspect the GCI folder " + root + '.';
    return false;
  }

  std::vector<std::string> backups;
  std::vector<std::string> temporary_files;
  const auto collect = [&](const auto& self, const File::FSTEntry& entry) -> void {
    for (const File::FSTEntry& child : entry.children)
    {
      if (child.isDirectory)
      {
        self(self, child);
      }
      else if (child.physicalName.ends_with(SYNC_BACKUP_SUFFIX))
      {
        backups.push_back(child.physicalName);
      }
      else if (child.physicalName.ends_with(SYNC_TEMP_SUFFIX))
      {
        temporary_files.push_back(child.physicalName);
      }
    }
  };
  collect(collect, tree);

  for (const std::string& backup : backups)
  {
    const std::string destination =
        backup.substr(0, backup.size() - SYNC_BACKUP_SUFFIX.size());
    if (File::IsFile(destination))
    {
      if (!File::Delete(backup, File::IfAbsentBehavior::NoConsoleWarning))
      {
        if (error)
          *error = "Could not remove a completed GCI synchronization backup.";
        return false;
      }
    }
    else if (!RenameWithoutDeleting(backup, destination))
    {
      if (error)
        *error = "Could not restore an interrupted GCI synchronization backup.";
      return false;
    }
  }
  for (const std::string& temporary : temporary_files)
  {
    if (!File::Delete(temporary, File::IfAbsentBehavior::NoConsoleWarning))
    {
      if (error)
        *error = "Could not remove an interrupted GCI synchronization temporary file.";
      return false;
    }
  }
  return true;
}

bool ScanSnapshot(const std::string& root, Snapshot* snapshot, std::string* error)
{
  snapshot->clear();
  if (!File::IsDirectory(root))
  {
    if (error)
      *error = "The GCI folder is unavailable: " + root;
    return false;
  }

  const File::FSTEntry tree = File::ScanDirectoryTree(root, true);
  if (!tree.isDirectory)
  {
    if (error)
      *error = "Could not enumerate the GCI folder: " + root;
    return false;
  }

  u64 total_size = 0;
  const auto scan = [&](const auto& self, const File::FSTEntry& entry,
                        const std::string& parent) -> bool {
    for (const File::FSTEntry& child : entry.children)
    {
      const std::string relative = parent.empty() ? child.virtualName :
                                                    JoinPath(parent, child.virtualName);
      if (!IsSafeRelativePath(relative))
      {
        if (error)
          *error = "The GCI folder contains an unsafe relative path.";
        return false;
      }
      if (child.isDirectory)
      {
        if (!self(self, child, relative))
          return false;
        continue;
      }
      if (IsSyncArtifact(relative))
        continue;
      if (snapshot->size() >= MAX_SYNC_FILES)
      {
        if (error)
          *error = "The GCI folder contains too many files to synchronize safely.";
        return false;
      }

      FileState state;
      if (!HashFile(child.physicalName, relative, &state, error))
        return false;
      if (state.size > MAX_SYNC_BYTES - total_size)
      {
        if (error)
          *error = "The GCI folder is unexpectedly large and was not synchronized.";
        return false;
      }
      total_size += state.size;
      const std::string key = Lower(NormalizePath(relative));
      if (!snapshot->emplace(key, std::move(state)).second)
      {
        if (error)
          *error = "The GCI folder contains names which differ only by letter case.";
        return false;
      }
    }
    return true;
  };
  return scan(scan, tree, {});
}

bool ScanRemoteSnapshot(const std::string& root, Snapshot* snapshot, std::string* error)
{
  if (!ScanSnapshot(root, snapshot, error))
    return false;
  const std::optional<std::string> id = SmbIdFromPath(root);
  if (!id || Storage::GetSmbConnectionState(*id) != Storage::SmbConnectionState::Connected)
  {
    if (error)
      *error = "The SMB GCI folder disconnected while it was being enumerated.";
    return false;
  }
  return true;
}

bool ParseUnsigned(std::string_view value, u64* output)
{
  const auto [end, parse_error] =
      std::from_chars(value.data(), value.data() + value.size(), *output);
  return parse_error == std::errc{} && end == value.data() + value.size();
}

bool LoadManifest(const std::string& path, const std::string& remote_path, Manifest* manifest,
                  std::string* error)
{
  *manifest = {};
  if (!File::IsFile(path))
    return true;

  picojson::value root_value;
  std::string parse_error;
  if (!JsonFromFile(path, &root_value, &parse_error) || !root_value.is<picojson::object>())
  {
    if (error)
      *error = "The local GCI synchronization journal is damaged and was not ignored.";
    return false;
  }
  const picojson::object& root = root_value.get<picojson::object>();
  if (ReadNumericFromJson<int>(root, "version").value_or(0) != 1 ||
      NormalizePath(ReadStringFromJson(root, "remote_path").value_or("")) !=
          NormalizePath(remote_path))
  {
    if (error)
      *error = "The local GCI synchronization journal does not match its SMB folder.";
    return false;
  }

  const auto files_iterator = root.find("baseline");
  if (files_iterator == root.end() || !files_iterator->second.is<picojson::array>())
  {
    if (error)
      *error = "The local GCI synchronization journal has no valid baseline.";
    return false;
  }

  u64 total_size = 0;
  for (const picojson::value& file_value : files_iterator->second.get<picojson::array>())
  {
    if (!file_value.is<picojson::object>() || manifest->baseline.size() >= MAX_SYNC_FILES)
    {
      if (error)
        *error = "The local GCI synchronization journal contains invalid file records.";
      return false;
    }
    const picojson::object& file = file_value.get<picojson::object>();
    FileState state;
    state.relative_path = ReadStringFromJson(file, "path").value_or("");
    state.sha1 = Lower(ReadStringFromJson(file, "sha1").value_or(""));
    const std::string size = ReadStringFromJson(file, "size").value_or("");
    if (!IsSafeRelativePath(state.relative_path) || !IsHexDigest(state.sha1) ||
        !ParseUnsigned(size, &state.size) || state.size > MAX_SYNC_BYTES - total_size)
    {
      if (error)
        *error = "The local GCI synchronization journal contains an unsafe file record.";
      return false;
    }
    total_size += state.size;
    const std::string key = Lower(NormalizePath(state.relative_path));
    if (!manifest->baseline.emplace(key, std::move(state)).second)
    {
      if (error)
        *error = "The local GCI synchronization journal contains duplicate file records.";
      return false;
    }
  }

  manifest->exists = true;
  manifest->session_active = ReadBoolFromJson(root, "session_active").value_or(false);
  manifest->pending = ReadBoolFromJson(root, "pending").value_or(false);
  return true;
}

bool SaveManifest(const std::string& path, const std::string& remote_path,
                  const Snapshot& baseline, bool session_active, bool pending,
                  std::string* error)
{
  picojson::array files;
  files.reserve(baseline.size());
  for (const auto& [key, state] : baseline)
  {
    (void)key;
    picojson::object file;
    file.emplace("path", picojson::value(state.relative_path));
    file.emplace("sha1", picojson::value(state.sha1));
    file.emplace("size", picojson::value(std::to_string(state.size)));
    files.emplace_back(std::move(file));
  }

  picojson::object root;
  root.emplace("version", picojson::value(1.0));
  root.emplace("remote_path", picojson::value(remote_path));
  root.emplace("session_active", picojson::value(session_active));
  root.emplace("pending", picojson::value(pending));
  root.emplace("baseline", picojson::value(std::move(files)));
  return WriteAtomic(path, picojson::value(std::move(root)).serialize(true), error);
}

SyncPlan BuildPlan(const Snapshot& baseline, const Snapshot& local, const Snapshot& remote)
{
  std::set<std::string> keys;
  for (const auto& [key, state] : baseline)
    keys.insert(key);
  for (const auto& [key, state] : local)
    keys.insert(key);
  for (const auto& [key, state] : remote)
    keys.insert(key);

  SyncPlan plan;
  for (const std::string& key : keys)
  {
    const FileState* const baseline_state = FindState(baseline, key);
    const FileState* const local_state = FindState(local, key);
    const FileState* const remote_state = FindState(remote, key);
    if (SameState(local_state, remote_state))
      continue;

    if (SameState(local_state, baseline_state))
    {
      plan.actions.push_back({remote_state ? ActionKind::CopyRemoteToLocal :
                                             ActionKind::DeleteLocal,
                              local_state ? std::optional<FileState>(*local_state) : std::nullopt,
                              remote_state ? std::optional<FileState>(*remote_state) :
                                             std::nullopt});
    }
    else if (SameState(remote_state, baseline_state))
    {
      plan.actions.push_back({local_state ? ActionKind::CopyLocalToRemote :
                                            ActionKind::DeleteRemote,
                              local_state ? std::optional<FileState>(*local_state) : std::nullopt,
                              remote_state ? std::optional<FileState>(*remote_state) :
                                             std::nullopt});
    }
    else
    {
      plan.conflicts.push_back(key);
    }
  }
  return plan;
}

std::string StatePath(const std::string& root, const std::optional<FileState>& state,
                      const std::optional<FileState>& other)
{
  if (state)
    return JoinPath(root, state->relative_path);
  return other ? JoinPath(root, other->relative_path) : root;
}

bool ValidateState(const std::string& path, const std::optional<FileState>& expected,
                   std::string* error)
{
  if (!expected)
  {
    if (!File::Exists(path))
      return true;
    if (error)
      *error = "A GCI synchronization destination changed before it could be updated.";
    return false;
  }

  FileState current;
  if (!HashFile(path, expected->relative_path, &current, error))
    return false;
  if (!SameState(&current, &*expected))
  {
    if (error)
      *error = "A GCI file changed during synchronization; no newer data was overwritten.";
    return false;
  }
  return true;
}

bool CopyFileDurably(const std::string& source, const FileState& expected_source,
                     const std::string& destination,
                     const std::optional<FileState>& expected_destination, std::string* error)
{
  if (!File::CreateFullPath(destination))
  {
    if (error)
      *error = "Could not create a GCI synchronization destination folder.";
    return false;
  }

  const std::string temporary = destination + std::string(SYNC_TEMP_SUFFIX);
  File::Delete(temporary, File::IfAbsentBehavior::NoConsoleWarning);
  File::IOFile input(source, "rb");
  File::IOFile output(temporary, "wb");
  if (!input || !output)
  {
    if (error)
      *error = "Could not open a GCI file for synchronization.";
    return false;
  }

  std::array<u8, 128 * 1024> buffer{};
  while (true)
  {
    const std::size_t bytes_read =
        std::fread(buffer.data(), sizeof(buffer.front()), buffer.size(), input.GetHandle());
    if (bytes_read != 0 &&
        std::fwrite(buffer.data(), sizeof(buffer.front()), bytes_read, output.GetHandle()) !=
            bytes_read)
    {
      if (error)
        *error = "A GCI synchronization write failed; the previous destination was kept.";
      return false;
    }
    if (bytes_read != buffer.size())
    {
      if (std::ferror(input.GetHandle()))
      {
        if (error)
          *error = "A GCI synchronization read failed; the previous destination was kept.";
        return false;
      }
      break;
    }
  }

  if (!FlushDurably(&output) || !output.Close())
  {
    File::Delete(temporary, File::IfAbsentBehavior::NoConsoleWarning);
    if (error)
      *error = "A GCI synchronization write could not be flushed safely.";
    return false;
  }

  FileState copied;
  if (!HashFile(temporary, expected_source.relative_path, &copied, error) ||
      !SameState(&copied, &expected_source))
  {
    File::Delete(temporary, File::IfAbsentBehavior::NoConsoleWarning);
    if (error && error->empty())
      *error = "A copied GCI file failed verification; the previous destination was kept.";
    return false;
  }
  // The full plan is checked before any writes, but copying and hashing can take long enough for
  // another SMB client to modify the destination. Check it once more immediately before the
  // atomic rename so that a newer save is never silently overwritten in that race window.
  if (!ValidateState(destination, expected_destination, error))
  {
    File::Delete(temporary, File::IfAbsentBehavior::NoConsoleWarning);
    return false;
  }
  return AtomicReplace(temporary, destination, error);
}

bool DeleteFileAtomically(const std::string& path, const FileState& expected, std::string* error)
{
  if (!ValidateState(path, expected, error))
    return false;

  const std::string backup = path + std::string(SYNC_BACKUP_SUFFIX);
  File::Delete(backup, File::IfAbsentBehavior::NoConsoleWarning);
  if (!RenameWithoutDeleting(path, backup))
  {
    if (error)
      *error = "Could not safely stage a deleted GCI file.";
    return false;
  }
  if (!File::Delete(backup, File::IfAbsentBehavior::NoConsoleWarning))
  {
    RenameWithoutDeleting(backup, path);
    if (error)
      *error = "Could not commit a GCI file deletion; the file was restored.";
    return false;
  }
  return true;
}

bool ValidateAction(const SyncAction& action, const std::string& local_root,
                    const std::string& remote_root, std::string* error)
{
  const std::string local_path = StatePath(local_root, action.local, action.remote);
  const std::string remote_path = StatePath(remote_root, action.remote, action.local);
  return ValidateState(local_path, action.local, error) &&
         ValidateState(remote_path, action.remote, error);
}

bool ApplyAction(const SyncAction& action, const std::string& local_root,
                 const std::string& remote_root, std::string* error)
{
  const std::string local_path = StatePath(local_root, action.local, action.remote);
  const std::string remote_path = StatePath(remote_root, action.remote, action.local);
  switch (action.kind)
  {
  case ActionKind::CopyLocalToRemote:
    return CopyFileDurably(local_path, *action.local, remote_path, action.remote, error);
  case ActionKind::CopyRemoteToLocal:
    return CopyFileDurably(remote_path, *action.remote, local_path, action.local, error);
  case ActionKind::DeleteLocal:
    return DeleteFileAtomically(local_path, *action.local, error);
  case ActionKind::DeleteRemote:
    return DeleteFileAtomically(remote_path, *action.remote, error);
  }
  return false;
}

bool Reconcile(const Snapshot& baseline, const std::string& local_root,
               const std::string& remote_root, const std::string& conflict_root,
               Snapshot* synchronized, std::string* error)
{
  Snapshot local;
  Snapshot remote;
  if (!ScanSnapshot(local_root, &local, error) ||
      !ScanRemoteSnapshot(remote_root, &remote, error))
    return false;

  const SyncPlan plan = BuildPlan(baseline, local, remote);
  if (!plan.conflicts.empty())
  {
    const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    const std::string conflict_directory = JoinPath(conflict_root, std::to_string(timestamp));
    for (const std::string& key : plan.conflicts)
    {
      const FileState* const local_state = FindState(local, key);
      const FileState* const remote_state = FindState(remote, key);
      if (local_state &&
          !CopyFileDurably(JoinPath(local_root, local_state->relative_path), *local_state,
                           JoinPath(JoinPath(conflict_directory, "local"),
                                    local_state->relative_path),
                           std::nullopt, error))
      {
        return false;
      }
      if (remote_state &&
          !CopyFileDurably(JoinPath(remote_root, remote_state->relative_path), *remote_state,
                           JoinPath(JoinPath(conflict_directory, "remote"),
                                    remote_state->relative_path),
                           std::nullopt, error))
      {
        return false;
      }
    }
    const std::string readme =
        "Dolphin found changes to both the local SD cache and the SMB GCI folder since the last "
        "successful sync. Neither live copy was overwritten. Compare the local and remote "
        "subfolders, keep the wanted files, then retry.\n";
    if (!WriteAtomic(JoinPath(conflict_directory, "README.txt"), readme, error))
      return false;
    if (error)
      *error = "The SD and SMB GCI folders both changed. Neither was overwritten. Conflict "
               "copies were saved in:\n" +
               conflict_directory;
    return false;
  }

  // Validate the complete plan before changing either side. This catches external edits made
  // after the snapshots and keeps a partially applied plan recoverable through the old baseline.
  for (const SyncAction& action : plan.actions)
  {
    if (!ValidateAction(action, local_root, remote_root, error))
      return false;
  }
  for (const SyncAction& action : plan.actions)
  {
    if (!ApplyAction(action, local_root, remote_root, error))
      return false;
  }

  Snapshot final_local;
  Snapshot final_remote;
  if (!ScanSnapshot(local_root, &final_local, error) ||
      !ScanRemoteSnapshot(remote_root, &final_remote, error))
  {
    return false;
  }
  const SyncPlan verification = BuildPlan(final_local, final_local, final_remote);
  if (!verification.actions.empty() || !verification.conflicts.empty())
  {
    if (error)
      *error = "The GCI folders changed again before synchronization could be verified.";
    return false;
  }
  *synchronized = std::move(final_local);
  return true;
}

bool EnsureRemoteRoot(const std::string& remote_path, std::string* error)
{
  const std::optional<std::string> id = SmbIdFromPath(remote_path);
  if (!id)
  {
    if (error)
      *error = "The configured SMB GCI folder has an invalid device path.";
    return false;
  }

  const std::string root = Storage::SmbRootPath(*id);
  std::string prepared;
  (void)PrepareLaunchStorage(root, &prepared);
  if (File::IsDirectory(root) &&
      Storage::GetSmbConnectionState(*id) == Storage::SmbConnectionState::Connected)
  {
    return true;
  }

  std::string reconnect_error;
  if (Storage::IsSmbMounted(*id) && Storage::ReconnectSmb(*id, &reconnect_error) &&
      File::IsDirectory(root))
  {
    return true;
  }
  if (error)
  {
    *error = reconnect_error.empty() ? "The SMB save share is unavailable." :
                                      "The SMB save share is unavailable: " + reconnect_error;
  }
  return false;
}

std::string CacheKey(const std::string& remote_path, ExpansionInterface::Slot slot)
{
  // Preserve the remote path's case in the cache identity. Samba servers can expose
  // case-sensitive paths, where two differently cased directory names are not interchangeable.
  const std::string identity = NormalizePath(remote_path) +
                               (slot == ExpansionInterface::Slot::A ? "\nslot-a" : "\nslot-b");
  return Lower(Common::SHA1::DigestToString(Common::SHA1::CalculateDigest(identity)));
}

void AppendMessage(std::string* destination, std::string_view message)
{
  if (!destination || message.empty())
    return;
  if (!destination->empty())
    destination->append("\n\n");
  destination->append(message);
}
}  // namespace

struct GciSyncSession::Impl
{
  struct SlotState
  {
    ExpansionInterface::Slot slot = ExpansionInterface::Slot::A;
    std::string remote_path;
    std::string cache_root;
    std::string local_path;
    std::string manifest_path;
    std::string conflict_root;
    Snapshot baseline;
    bool prepared_offline = false;
  };

  bool PrepareSlot(ExpansionInterface::Slot slot, const std::string& configured_path,
                   SlotState* output, std::string* error)
  {
    SlotState state;
    state.slot = slot;
    state.remote_path = NormalizePath(configured_path);
    state.cache_root = JoinPath(File::GetUserPath(D_GCUSER_IDX),
                                JoinPath("Network GCI Cache", CacheKey(state.remote_path, slot)));
    state.local_path = JoinPath(state.cache_root, "files");
    state.manifest_path = JoinPath(state.cache_root, "sync-state.json");
    state.conflict_root = JoinPath(state.cache_root, "conflicts");

    // The journal itself is atomically replaced outside the live `files` directory. Recover it
    // before loading so a force close between the two rename operations cannot make an existing
    // cache look like an uninitialized one.
    if (!RecoverAtomicArtifacts(state.cache_root, error))
      return false;

    Manifest manifest;
    if (!LoadManifest(state.manifest_path, state.remote_path, &manifest, error))
      return false;
    const bool local_exists = File::IsDirectory(state.local_path);
    if (manifest.exists && !local_exists)
    {
      if (error)
        *error = "The SMB GCI cache journal exists, but its local save folder is missing. The "
                 "remote card was not touched.";
      return false;
    }
    if (!local_exists && !File::CreateDirs(state.local_path))
    {
      if (error)
        *error = "Could not create the local SD cache for the SMB GCI folder.";
      return false;
    }
    if (!RecoverAtomicArtifacts(state.local_path, error))
      return false;

    std::string remote_error;
    if (!EnsureRemoteRoot(state.remote_path, &remote_error))
    {
      if (!manifest.exists)
      {
        if (error)
          *error = remote_error +
                   " There is no verified local cache yet, so Dolphin refused to create a blank "
                   "memory card.";
        return false;
      }
      state.prepared_offline = true;
      state.baseline = manifest.baseline;
      if (!SaveManifest(state.manifest_path, state.remote_path, state.baseline, true, true, error))
        return false;
      *output = std::move(state);
      return true;
    }

    if (File::Exists(state.remote_path) && !File::IsDirectory(state.remote_path))
    {
      if (error)
        *error = "The configured SMB GCI path exists but is not a directory.";
      return false;
    }
    if (!File::IsDirectory(state.remote_path))
    {
      if (manifest.exists && !manifest.baseline.empty())
      {
        if (error)
          *error = "The SMB share is online, but the previously synchronized GCI folder is "
                   "missing. Dolphin kept the SD cache and refused to treat this as deleting all "
                   "saves.";
        return false;
      }
      if (!File::CreateDirs(state.remote_path))
      {
        if (error)
          *error = "Could not create the configured GCI folder on the SMB share.";
        return false;
      }
    }
    if (!RecoverAtomicArtifacts(state.remote_path, error))
      return false;

    Snapshot synchronized;
    if (!Reconcile(manifest.baseline, state.local_path, state.remote_path, state.conflict_root,
                   &synchronized, error))
    {
      return false;
    }
    state.baseline = std::move(synchronized);
    if (!SaveManifest(state.manifest_path, state.remote_path, state.baseline, true, false, error))
      return false;
    *output = std::move(state);
    return true;
  }

  void CancelPrepared()
  {
    for (const SlotState& state : slots)
    {
      std::string ignored;
      SaveManifest(state.manifest_path, state.remote_path, state.baseline, false,
                   state.prepared_offline, &ignored);
    }
    slots.clear();
  }

  bool Prepare(Core::System& system, std::string* error)
  {
    if (prepared)
      return true;
    if (system.IsWii() || NetPlay::IsNetPlayRunning() || system.GetMovie().IsPlayingInput())
    {
      prepared = true;
      return true;
    }

    std::vector<std::pair<ExpansionInterface::Slot, std::string>> candidates;
    std::set<std::string> remote_paths;
    const DiscIO::Region region =
        Config::ToGameCubeRegion(SConfig::GetInstance().m_region);
    for (const ExpansionInterface::Slot slot : ExpansionInterface::MEMCARD_SLOTS)
    {
      if (Config::Get(Config::GetInfoForEXIDevice(slot)) !=
              ExpansionInterface::EXIDeviceType::MemoryCardFolder ||
          !Config::Get(Config::GetInfoForGCIPathOverride(slot)).empty())
      {
        continue;
      }
      // GCI folder settings store a base path. The EXI device expands that base to the current
      // title's region directory before opening it, so synchronize and override that exact
      // effective directory rather than accidentally caching the parent of USA/JAP/EUR.
      const std::string path = NormalizePath(Config::GetGCIFolderPath(slot, region));
      if (!SmbIdFromPath(path))
        continue;
      if (!remote_paths.insert(Lower(path)).second)
      {
        if (error)
          *error = "Memory card slots A and B cannot safely synchronize the same SMB GCI folder.";
        return false;
      }
      candidates.emplace_back(slot, path);
    }

    for (const auto& [slot, path] : candidates)
    {
      SlotState state;
      if (!PrepareSlot(slot, path, &state, error))
      {
        CancelPrepared();
        return false;
      }
      slots.emplace_back(std::move(state));
    }

    for (const SlotState& state : slots)
      Config::SetCurrent(Config::GetInfoForGCIPathOverride(state.slot), state.local_path);
    prepared = true;
    return true;
  }

  bool Finish(std::string* warning)
  {
    bool success = true;
    for (SlotState& state : slots)
    {
      std::string slot_error;
      if (!File::IsDirectory(state.local_path))
      {
        success = false;
        AppendMessage(warning, "The local SD GCI cache unexpectedly disappeared. The SMB copy was "
                               "not modified.");
        continue;
      }

      if (!EnsureRemoteRoot(state.remote_path, &slot_error))
      {
        success = false;
        std::string journal_error;
        SaveManifest(state.manifest_path, state.remote_path, state.baseline, false, true,
                     &journal_error);
        AppendMessage(warning,
                      "The SMB save share is offline. Your save remains safe in the local SD "
                      "cache and will be synchronized the next time this memory-card folder is "
                      "used.");
        if (!journal_error.empty())
          AppendMessage(warning, journal_error);
        continue;
      }

      if (!File::IsDirectory(state.remote_path))
      {
        success = false;
        std::string journal_error;
        SaveManifest(state.manifest_path, state.remote_path, state.baseline, false, true,
                     &journal_error);
        AppendMessage(warning,
                      "The SMB GCI folder disappeared while the game was running. Dolphin kept "
                      "the complete SD cache and did not recreate or overwrite the remote card.");
        continue;
      }
      if (!RecoverAtomicArtifacts(state.remote_path, &slot_error) ||
          !RecoverAtomicArtifacts(state.local_path, &slot_error))
      {
        success = false;
      }
      else
      {
        Snapshot synchronized;
        if (Reconcile(state.baseline, state.local_path, state.remote_path, state.conflict_root,
                      &synchronized, &slot_error))
        {
          state.baseline = std::move(synchronized);
          if (SaveManifest(state.manifest_path, state.remote_path, state.baseline, false, false,
                           &slot_error))
          {
            continue;
          }
        }
        success = false;
      }

      std::string journal_error;
      SaveManifest(state.manifest_path, state.remote_path, state.baseline, false, true,
                   &journal_error);
      AppendMessage(warning, slot_error.empty() ?
                                 "The SMB GCI synchronization did not complete. The authoritative "
                                 "save is still safe in the local SD cache." :
                                 slot_error +
                                     "\nThe authoritative save is still safe in the local SD "
                                     "cache.");
      if (!journal_error.empty())
        AppendMessage(warning, journal_error);
    }
    slots.clear();
    prepared = false;
    return success;
  }

  bool prepared = false;
  std::vector<SlotState> slots;
};

GciSyncSession::GciSyncSession() : m_impl(std::make_unique<Impl>())
{
}

GciSyncSession::~GciSyncSession() = default;

bool GciSyncSession::Prepare(Core::System& system, std::string* error)
{
  if (error)
    error->clear();
  return m_impl->Prepare(system, error);
}

bool GciSyncSession::Finish(std::string* warning)
{
  if (warning)
    warning->clear();
  return m_impl->Finish(warning);
}
}  // namespace DolphinSwitch
