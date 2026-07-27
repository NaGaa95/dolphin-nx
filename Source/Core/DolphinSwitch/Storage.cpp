// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived in part from Cemu-NX storage code (MPL-2.0).

#include "DolphinSwitch/Storage.h"

#include <switch.h>
#include <usbhsfs.h>

#include <ctime>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <vector>

namespace DolphinSwitch::Storage
{
namespace
{
constexpr std::size_t SMB_READ_AHEAD_BYTES = 256 * 1024;
constexpr std::size_t SMB_READ_AHEAD_BUDGET = 8 * 1024 * 1024;

struct SmbMount;

struct SmbFile
{
  SmbMount* mount = nullptr;
  smb2fh* handle = nullptr;
  std::uint8_t* read_ahead = nullptr;
  std::size_t read_ahead_offset = 0;
  std::size_t read_ahead_size = 0;
  std::uint64_t position = 0;
};

struct SmbDir
{
  SmbMount* mount = nullptr;
  smb2dir* handle = nullptr;
};

struct SmbMount
{
  SmbShare config;
  std::string device_name;
  std::string root_path;
  smb2_context* context = nullptr;
  bool connected = false;
  devoptab_t devoptab{};
  std::mutex io_mutex;
  std::size_t read_ahead_bytes = 0;

  ~SmbMount()
  {
    if (!context)
      return;
    if (connected)
      smb2_disconnect_share(context);
    smb2_destroy_context(context);
  }
};

std::mutex s_mount_mutex;
std::vector<std::unique_ptr<SmbMount>> s_smb_mounts;
bool s_usb_initialized = false;
std::atomic<std::uint64_t> s_usb_generation{0};

void UsbStatusChanged(const UsbHsFsDevice*, u32, void*)
{
  s_usb_generation.fetch_add(1, std::memory_order_release);
}

int Fail(_reent* reent, int error)
{
  if (reent)
    reent->_errno = error > 0 ? error : EIO;
  return -1;
}

bool ValidId(const std::string& id)
{
  if (id.empty() || id.size() > 16)
    return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  });
}

std::string DeviceNameForId(const std::string& id)
{
  return ValidId(id) ? "dsmb_" + id : std::string{};
}

bool FixPath(const char* source, char* destination, std::size_t destination_size)
{
  if (!source || !destination || destination_size == 0)
    return false;
  const char* colon = std::strchr(source, ':');
  if (!colon)
    return false;
  const char* input = colon + 1;
  while (*input == '/')
    ++input;

  std::size_t length = 0;
  bool slash = false;
  for (; *input; ++input)
  {
    if (*input == '/')
    {
      if (slash)
        continue;
      slash = true;
    }
    else
    {
      slash = false;
    }
    if (length + 1 >= destination_size)
      return false;
    destination[length++] = *input;
  }
  while (length && destination[length - 1] == '/')
    --length;
  destination[length] = '\0';
  return true;
}

bool IsRootPath(const char* path)
{
  const char* colon = path ? std::strchr(path, ':') : nullptr;
  if (!colon)
    return false;
  ++colon;
  while (*colon == '/')
    ++colon;
  return *colon == '\0';
}

void FillStat(struct stat* output, const smb2_stat_64& input)
{
  std::memset(output, 0, sizeof(*output));
  switch (input.smb2_type)
  {
  case SMB2_TYPE_FILE:
    output->st_mode = S_IFREG | 0666;
    break;
  case SMB2_TYPE_DIRECTORY:
    output->st_mode = S_IFDIR | 0777;
    break;
  case SMB2_TYPE_LINK:
    output->st_mode = S_IFLNK | 0777;
    break;
  default:
    output->st_mode = S_IFREG | 0444;
    break;
  }
  output->st_ino = input.smb2_ino;
  output->st_nlink = input.smb2_nlink ? input.smb2_nlink : 1;
  output->st_size = static_cast<off_t>(input.smb2_size);
  output->st_atime = input.smb2_atime;
  output->st_mtime = input.smb2_mtime;
  output->st_ctime = input.smb2_ctime;
  output->st_blksize = 65536;
}

SmbMount* MountFrom(_reent* reent)
{
  return reent ? static_cast<SmbMount*>(reent->deviceData) : nullptr;
}

bool EnsureReadAhead(SmbFile* file)
{
  if (file->read_ahead)
    return true;
  if (file->mount->read_ahead_bytes > SMB_READ_AHEAD_BUDGET - SMB_READ_AHEAD_BYTES)
    return false;
  file->read_ahead = static_cast<std::uint8_t*>(std::malloc(SMB_READ_AHEAD_BYTES));
  if (!file->read_ahead)
    return false;
  file->mount->read_ahead_bytes += SMB_READ_AHEAD_BYTES;
  return true;
}

void ReleaseReadAhead(SmbFile* file)
{
  if (!file->read_ahead)
    return;
  std::free(file->read_ahead);
  file->read_ahead = nullptr;
  file->mount->read_ahead_bytes -= SMB_READ_AHEAD_BYTES;
  file->read_ahead_offset = 0;
  file->read_ahead_size = 0;
}

int SynchronizeFilePosition(SmbFile* file)
{
  if (file->read_ahead_size == 0)
    return 0;
  std::uint64_t position = 0;
  const int result = smb2_lseek(file->mount->context, file->handle,
                                static_cast<std::int64_t>(file->position), SEEK_SET, &position);
  if (result >= 0)
  {
    file->read_ahead_offset = 0;
    file->read_ahead_size = 0;
  }
  return result;
}

int SmbOpen(_reent* reent, void* state, const char* source, int flags, int)
{
  auto* mount = MountFrom(reent);
  auto* file = static_cast<SmbFile*>(state);
  *file = {};
  if (!mount)
    return Fail(reent, ENODEV);
  char path[PATH_MAX]{};
  if (!FixPath(source, path, sizeof(path)))
    return Fail(reent, ENAMETOOLONG);
  std::lock_guard lock(mount->io_mutex);
  file->handle = smb2_open(mount->context, path, flags);
  if (!file->handle)
    return Fail(reent, EIO);
  file->mount = mount;
  if ((flags & O_APPEND) != 0)
  {
    std::uint64_t position = 0;
    const int result = smb2_lseek(mount->context, file->handle, 0, SEEK_END, &position);
    if (result < 0)
    {
      smb2_close(mount->context, file->handle);
      *file = {};
      return Fail(reent, -result);
    }
    file->position = position;
  }
  reent->_errno = 0;
  return 0;
}

int SmbClose(_reent* reent, void* state)
{
  auto* file = static_cast<SmbFile*>(state);
  if (!file || !file->mount || !file->handle)
    return Fail(reent, EBADF);
  std::lock_guard lock(file->mount->io_mutex);
  ReleaseReadAhead(file);
  const int result = smb2_close(file->mount->context, file->handle);
  *file = {};
  if (result < 0)
    return Fail(reent, -result);
  reent->_errno = 0;
  return 0;
}

ssize_t SmbRead(_reent* reent, void* state, char* output, std::size_t length)
{
  auto* file = static_cast<SmbFile*>(state);
  if (!file || !file->mount || !file->handle)
    return Fail(reent, EBADF);
  std::lock_guard lock(file->mount->io_mutex);
  const std::size_t maximum =
      std::max<std::size_t>(1, smb2_get_max_read_size(file->mount->context));
  std::size_t total = 0;
  if (file->read_ahead_offset < file->read_ahead_size)
  {
    const std::size_t cached =
        std::min(length, file->read_ahead_size - file->read_ahead_offset);
    std::memcpy(output, file->read_ahead + file->read_ahead_offset, cached);
    file->read_ahead_offset += cached;
    file->position += cached;
    total += cached;
    if (file->read_ahead_offset == file->read_ahead_size)
      file->read_ahead_offset = file->read_ahead_size = 0;
  }
  while (total < length)
  {
    const std::size_t remaining = length - total;
    const bool use_read_ahead = remaining < SMB_READ_AHEAD_BYTES && EnsureReadAhead(file);
    const std::size_t amount =
        std::min(use_read_ahead ? SMB_READ_AHEAD_BYTES : remaining, maximum);
    std::uint8_t* destination = use_read_ahead ? file->read_ahead :
                                                reinterpret_cast<std::uint8_t*>(output + total);
    const int result = smb2_read(file->mount->context, file->handle, destination, amount);
    if (result < 0)
      return total ? static_cast<ssize_t>(total) : Fail(reent, -result);
    if (result == 0)
      break;
    if (use_read_ahead)
    {
      file->read_ahead_offset = 0;
      file->read_ahead_size = static_cast<std::size_t>(result);
      const std::size_t copied = std::min(remaining, file->read_ahead_size);
      std::memcpy(output + total, file->read_ahead, copied);
      file->read_ahead_offset = copied;
      file->position += copied;
      total += copied;
      if (file->read_ahead_offset == file->read_ahead_size)
        file->read_ahead_offset = file->read_ahead_size = 0;
      break;
    }
    const std::size_t bytes_read = static_cast<std::size_t>(result);
    file->position += bytes_read;
    total += bytes_read;
    if (bytes_read < amount)
      break;
  }
  reent->_errno = 0;
  return static_cast<ssize_t>(total);
}

ssize_t SmbWrite(_reent* reent, void* state, const char* input, std::size_t length)
{
  auto* file = static_cast<SmbFile*>(state);
  if (!file || !file->mount || !file->handle)
    return Fail(reent, EBADF);
  std::lock_guard lock(file->mount->io_mutex);
  const int synchronized = SynchronizeFilePosition(file);
  if (synchronized < 0)
    return Fail(reent, -synchronized);
  const std::size_t maximum =
      std::max<std::size_t>(1, smb2_get_max_write_size(file->mount->context));
  std::size_t total = 0;
  while (total < length)
  {
    const std::size_t amount = std::min(length - total, maximum);
    const int result = smb2_write(file->mount->context, file->handle,
                                  reinterpret_cast<const std::uint8_t*>(input + total), amount);
    if (result < 0)
      return total ? static_cast<ssize_t>(total) : Fail(reent, -result);
    if (result == 0)
      return total ? static_cast<ssize_t>(total) : Fail(reent, EIO);
    total += static_cast<std::size_t>(result);
    file->position += static_cast<std::size_t>(result);
  }
  reent->_errno = 0;
  return static_cast<ssize_t>(total);
}

off_t SmbSeek(_reent* reent, void* state, off_t position, int origin)
{
  auto* file = static_cast<SmbFile*>(state);
  if (!file || !file->mount || !file->handle)
  {
    Fail(reent, EBADF);
    return static_cast<off_t>(-1);
  }
  std::lock_guard lock(file->mount->io_mutex);
  std::uint64_t result_position = 0;
  if (origin == SEEK_SET || origin == SEEK_CUR)
  {
    if (file->position > static_cast<std::uint64_t>(LLONG_MAX))
    {
      Fail(reent, EOVERFLOW);
      return static_cast<off_t>(-1);
    }
    const std::int64_t base = origin == SEEK_SET ? 0 : static_cast<std::int64_t>(file->position);
    if (position > 0 && base > LLONG_MAX - position)
    {
      Fail(reent, EOVERFLOW);
      return static_cast<off_t>(-1);
    }
    const std::int64_t target = base + position;
    if (target < 0)
    {
      Fail(reent, EINVAL);
      return static_cast<off_t>(-1);
    }
    if (file->read_ahead_size > 0)
    {
      const std::uint64_t cache_start = file->position - file->read_ahead_offset;
      const std::uint64_t cache_end = cache_start + file->read_ahead_size;
      if (static_cast<std::uint64_t>(target) >= cache_start &&
          static_cast<std::uint64_t>(target) <= cache_end)
      {
        file->read_ahead_offset =
            static_cast<std::size_t>(static_cast<std::uint64_t>(target) - cache_start);
        file->position = static_cast<std::uint64_t>(target);
        reent->_errno = 0;
        return static_cast<off_t>(target);
      }
    }
    const int result =
        smb2_lseek(file->mount->context, file->handle, target, SEEK_SET, &result_position);
    if (result < 0)
    {
      Fail(reent, -result);
      return static_cast<off_t>(-1);
    }
  }
  else
  {
    const int result =
        smb2_lseek(file->mount->context, file->handle, position, origin, &result_position);
    if (result < 0)
    {
      Fail(reent, -result);
      return static_cast<off_t>(-1);
    }
  }
  if (result_position > static_cast<std::uint64_t>(LLONG_MAX))
  {
    Fail(reent, EOVERFLOW);
    return static_cast<off_t>(-1);
  }
  file->read_ahead_offset = file->read_ahead_size = 0;
  file->position = result_position;
  reent->_errno = 0;
  return static_cast<off_t>(result_position);
}

int SmbFstat(_reent* reent, void* state, struct stat* output)
{
  auto* file = static_cast<SmbFile*>(state);
  if (!file || !file->mount || !file->handle || !output)
    return Fail(reent, EBADF);
  std::lock_guard lock(file->mount->io_mutex);
  smb2_stat_64 info{};
  const int result = smb2_fstat(file->mount->context, file->handle, &info);
  if (result < 0)
    return Fail(reent, -result);
  FillStat(output, info);
  reent->_errno = 0;
  return 0;
}

int SmbStat(_reent* reent, const char* source, struct stat* output)
{
  auto* mount = MountFrom(reent);
  if (!mount || !output)
    return Fail(reent, EINVAL);
  if (IsRootPath(source))
  {
    std::memset(output, 0, sizeof(*output));
    output->st_mode = S_IFDIR | 0777;
    output->st_nlink = 1;
    reent->_errno = 0;
    return 0;
  }
  char path[PATH_MAX]{};
  if (!FixPath(source, path, sizeof(path)))
    return Fail(reent, ENAMETOOLONG);
  std::lock_guard lock(mount->io_mutex);
  smb2_stat_64 info{};
  const int result = smb2_stat(mount->context, path, &info);
  if (result < 0)
    return Fail(reent, -result);
  FillStat(output, info);
  reent->_errno = 0;
  return 0;
}

template <typename Operation>
int PathOperation(_reent* reent, const char* source, Operation operation)
{
  auto* mount = MountFrom(reent);
  if (!mount)
    return Fail(reent, ENODEV);
  char path[PATH_MAX]{};
  if (!FixPath(source, path, sizeof(path)))
    return Fail(reent, ENAMETOOLONG);
  std::lock_guard lock(mount->io_mutex);
  const int result = operation(mount, path);
  if (result < 0)
    return Fail(reent, -result);
  reent->_errno = 0;
  return 0;
}

int SmbUnlink(_reent* reent, const char* path)
{
  return PathOperation(reent, path,
                       [](SmbMount* mount, const char* fixed) {
                         return smb2_unlink(mount->context, fixed);
                       });
}

int SmbMkdir(_reent* reent, const char* path, int)
{
  return PathOperation(reent, path,
                       [](SmbMount* mount, const char* fixed) {
                         return smb2_mkdir(mount->context, fixed);
                       });
}

int SmbRmdir(_reent* reent, const char* path)
{
  return PathOperation(reent, path,
                       [](SmbMount* mount, const char* fixed) {
                         return smb2_rmdir(mount->context, fixed);
                       });
}

int SmbRename(_reent* reent, const char* source, const char* destination)
{
  auto* mount = MountFrom(reent);
  if (!mount)
    return Fail(reent, ENODEV);
  char old_path[PATH_MAX]{}, new_path[PATH_MAX]{};
  if (!FixPath(source, old_path, sizeof(old_path)) ||
      !FixPath(destination, new_path, sizeof(new_path)))
    return Fail(reent, ENAMETOOLONG);
  std::lock_guard lock(mount->io_mutex);
  const int result = smb2_rename(mount->context, old_path, new_path);
  if (result < 0)
    return Fail(reent, -result);
  reent->_errno = 0;
  return 0;
}

DIR_ITER* SmbDirOpen(_reent* reent, DIR_ITER* state, const char* source)
{
  auto* mount = MountFrom(reent);
  auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
  if (!mount || !directory)
  {
    Fail(reent, EINVAL);
    return nullptr;
  }
  *directory = {};
  char path[PATH_MAX]{};
  if (!FixPath(source, path, sizeof(path)))
  {
    Fail(reent, ENAMETOOLONG);
    return nullptr;
  }
  std::lock_guard lock(mount->io_mutex);
  directory->handle = smb2_opendir(mount->context, path);
  if (!directory->handle)
  {
    Fail(reent, EIO);
    return nullptr;
  }
  directory->mount = mount;
  reent->_errno = 0;
  return state;
}

int SmbDirReset(_reent* reent, DIR_ITER* state)
{
  auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
  if (!directory || !directory->mount || !directory->handle)
    return Fail(reent, EBADF);
  std::lock_guard lock(directory->mount->io_mutex);
  smb2_rewinddir(directory->mount->context, directory->handle);
  reent->_errno = 0;
  return 0;
}

int SmbDirNext(_reent* reent, DIR_ITER* state, char* name, struct stat* output)
{
  auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
  if (!directory || !directory->mount || !directory->handle || !name || !output)
    return Fail(reent, EBADF);
  std::lock_guard lock(directory->mount->io_mutex);
  const smb2dirent* entry =
      smb2_readdir(directory->mount->context, directory->handle);
  if (!entry)
    return Fail(reent, ENOENT);
  std::snprintf(name, NAME_MAX, "%s", entry->name);
  FillStat(output, entry->st);
  reent->_errno = 0;
  return 0;
}

int SmbDirClose(_reent* reent, DIR_ITER* state)
{
  auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
  if (!directory || !directory->mount || !directory->handle)
    return Fail(reent, EBADF);
  std::lock_guard lock(directory->mount->io_mutex);
  smb2_closedir(directory->mount->context, directory->handle);
  *directory = {};
  reent->_errno = 0;
  return 0;
}

int SmbStatvfs(_reent* reent, const char* source, struct statvfs* output)
{
  auto* mount = MountFrom(reent);
  if (!mount || !output)
    return Fail(reent, EINVAL);
  char path[PATH_MAX]{};
  if (!FixPath(source, path, sizeof(path)))
    return Fail(reent, ENAMETOOLONG);
  std::lock_guard lock(mount->io_mutex);
  struct smb2_statvfs info{};
  const int result = smb2_statvfs(mount->context, path, &info);
  if (result < 0)
    return Fail(reent, -result);
  std::memset(output, 0, sizeof(*output));
  output->f_bsize = info.f_bsize;
  output->f_frsize = info.f_frsize;
  output->f_blocks = info.f_blocks;
  output->f_bfree = info.f_bfree;
  output->f_bavail = info.f_bavail;
  output->f_files = info.f_files;
  output->f_ffree = info.f_ffree;
  output->f_favail = info.f_favail;
  output->f_fsid = info.f_fsid;
  output->f_flag = info.f_flag;
  output->f_namemax = info.f_namemax;
  reent->_errno = 0;
  return 0;
}

int SmbTruncate(_reent* reent, void* state, off_t length)
{
  auto* file = static_cast<SmbFile*>(state);
  if (!file || !file->mount || !file->handle)
    return Fail(reent, EBADF);
  std::lock_guard lock(file->mount->io_mutex);
  const int synchronized = SynchronizeFilePosition(file);
  if (synchronized < 0)
    return Fail(reent, -synchronized);
  const int result = smb2_ftruncate(file->mount->context, file->handle, length);
  if (result < 0)
    return Fail(reent, -result);
  reent->_errno = 0;
  return 0;
}

int SmbSync(_reent* reent, void* state)
{
  auto* file = static_cast<SmbFile*>(state);
  if (!file || !file->mount || !file->handle)
    return Fail(reent, EBADF);
  std::lock_guard lock(file->mount->io_mutex);
  const int result = smb2_fsync(file->mount->context, file->handle);
  if (result < 0)
    return Fail(reent, -result);
  reent->_errno = 0;
  return 0;
}
}  // namespace

std::string SmbRootPath(const std::string& id)
{
  const std::string device_name = DeviceNameForId(id);
  return device_name.empty() ? std::string{} : device_name + ":/";
}

std::string SmbBrowsePath(const SmbShare& share)
{
  std::string result = SmbRootPath(share.id);
  if (result.empty() || share.path.empty())
    return result;
  std::string path = share.path;
  std::replace(path.begin(), path.end(), '\\', '/');
  while (!path.empty() && path.front() == '/')
    path.erase(path.begin());
  while (!path.empty() && path.back() == '/')
    path.pop_back();
  return path.empty() ? result : result + path;
}

bool InitializeUsb(std::string* error)
{
  std::lock_guard lock(s_mount_mutex);
  if (s_usb_initialized)
    return true;
  usbHsFsSetFileSystemMountFlags(UsbHsFsMountFlags_None);
  const Result result = usbHsFsInitialize(0);
  if (R_FAILED(result))
  {
    if (error)
    {
      char message[80];
      std::snprintf(message, sizeof(message), "USB initialization failed (0x%08x)", result);
      *error = message;
    }
    return false;
  }
  s_usb_initialized = true;
  usbHsFsSetPopulateCallback(UsbStatusChanged, nullptr);
  return true;
}

std::uint64_t UsbStatusGeneration()
{
  return s_usb_generation.load(std::memory_order_acquire);
}

std::vector<Location> ListUsbLocations()
{
  std::vector<Location> locations;
  std::lock_guard lock(s_mount_mutex);
  if (!s_usb_initialized)
    return locations;
  std::array<UsbHsFsDevice, 32> devices{};
  const u32 count = usbHsFsListMountedDevices(devices.data(), devices.size());
  locations.reserve(count);
  for (u32 index = 0; index < count; ++index)
  {
    const UsbHsFsDevice& device = devices[index];
    Location location;
    location.path = device.name;
    if (location.path.empty())
      continue;
    if (location.path.back() != '/')
      location.path += '/';
    const std::uint64_t gib = device.capacity / (1024ULL * 1024ULL * 1024ULL);
    char label[256];
    std::snprintf(label, sizeof(label), "%s - %s%s%s (%llu GiB)", device.name,
                  LIBUSBHSFS_FS_TYPE_STR(device.fs_type),
                  device.product_name[0] ? " - " : "", device.product_name,
                  static_cast<unsigned long long>(gib));
    location.label = label;
    locations.emplace_back(std::move(location));
  }
  return locations;
}

bool MountSmb(const SmbShare& share, std::string* error)
{
  std::lock_guard lock(s_mount_mutex);
  if (!ValidId(share.id) || share.server.empty() || share.share.empty())
  {
    if (error)
      *error = "SMB share settings are incomplete";
    return false;
  }
  if (std::ranges::any_of(s_smb_mounts,
                          [&](const auto& mount) { return mount->config.id == share.id; }))
    return true;

  auto mount = std::make_unique<SmbMount>();
  mount->config = share;
  mount->device_name = DeviceNameForId(share.id);
  mount->root_path = mount->device_name + ":/";
  mount->context = smb2_init_context();
  if (!mount->context)
  {
    if (error)
      *error = "Could not create the SMB client";
    return false;
  }
  smb2_set_security_mode(mount->context, SMB2_NEGOTIATE_SIGNING_ENABLED);
  smb2_set_timeout(mount->context, 6);
  if (!share.user.empty())
    smb2_set_user(mount->context, share.user.c_str());
  if (!share.password.empty())
    smb2_set_password(mount->context, share.password.c_str());
  if (!share.domain.empty())
    smb2_set_domain(mount->context, share.domain.c_str());
  const int connected = smb2_connect_share(mount->context, share.server.c_str(),
                                           share.share.c_str(),
                                           share.user.empty() ? nullptr : share.user.c_str());
  if (connected < 0)
  {
    if (error)
    {
      const char* detail = smb2_get_error(mount->context);
      *error = detail && *detail ? detail : "Could not connect to the SMB share";
    }
    return false;
  }
  mount->connected = true;

  mount->devoptab.name = mount->device_name.c_str();
  mount->devoptab.structSize = sizeof(SmbFile);
  mount->devoptab.open_r = SmbOpen;
  mount->devoptab.close_r = SmbClose;
  mount->devoptab.write_r = SmbWrite;
  mount->devoptab.read_r = SmbRead;
  mount->devoptab.seek_r = SmbSeek;
  mount->devoptab.fstat_r = SmbFstat;
  mount->devoptab.stat_r = SmbStat;
  mount->devoptab.unlink_r = SmbUnlink;
  mount->devoptab.rename_r = SmbRename;
  mount->devoptab.mkdir_r = SmbMkdir;
  mount->devoptab.dirStateSize = sizeof(SmbDir);
  mount->devoptab.diropen_r = SmbDirOpen;
  mount->devoptab.dirreset_r = SmbDirReset;
  mount->devoptab.dirnext_r = SmbDirNext;
  mount->devoptab.dirclose_r = SmbDirClose;
  mount->devoptab.statvfs_r = SmbStatvfs;
  mount->devoptab.ftruncate_r = SmbTruncate;
  mount->devoptab.fsync_r = SmbSync;
  mount->devoptab.deviceData = mount.get();
  mount->devoptab.rmdir_r = SmbRmdir;
  mount->devoptab.lstat_r = SmbStat;
  if (AddDevice(&mount->devoptab) < 0)
  {
    if (error)
      *error = "No free filesystem slot is available for the SMB share";
    return false;
  }
  s_smb_mounts.emplace_back(std::move(mount));
  return true;
}

bool UnmountSmb(const std::string& id)
{
  std::lock_guard lock(s_mount_mutex);
  const auto iterator =
      std::ranges::find_if(s_smb_mounts, [&](const auto& mount) { return mount->config.id == id; });
  if (iterator == s_smb_mounts.end())
    return true;
  RemoveDevice((*iterator)->root_path.c_str());
  s_smb_mounts.erase(iterator);
  return true;
}

bool IsSmbMounted(const std::string& id)
{
  std::lock_guard lock(s_mount_mutex);
  return std::ranges::any_of(s_smb_mounts,
                             [&](const auto& mount) { return mount->config.id == id; });
}

void Shutdown()
{
  std::lock_guard lock(s_mount_mutex);
  for (auto& mount : s_smb_mounts)
    RemoveDevice(mount->root_path.c_str());
  s_smb_mounts.clear();
  if (s_usb_initialized)
  {
    usbHsFsSetPopulateCallback(nullptr, nullptr);
    usbHsFsExit();
    s_usb_initialized = false;
  }
}
}  // namespace DolphinSwitch::Storage
