// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/FS/HostBackend/FS.h"

#include <algorithm>
#include <cstdio>
#include <expected>
#include <memory>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

namespace IOS::HLE::FS
{
// This isn't theadsafe, but it's only called from the CPU thread.
std::shared_ptr<File::IOFile> HostFileSystem::OpenHostFile(const std::string& host_path)
{
  // On the wii, all file operations are strongly ordered.
  // If a game opens the same file twice (or 8 times, looking at you PokePark Wii)
  // and writes to one file handle, it will be able to immediately read the written
  // data from the other handle.
  // On 'real' operating systems, there are various buffers and caches meaning
  // applications doing such naughty things will not get expected results.

  // So we fix this by catching any attempts to open the same file twice and
  // only opening one file. Accesses to a single file handle are ordered.
  //
  // Hall of Shame:
  //    - PokePark Wii (gets stuck on the loading screen of Pikachu falling)
  //    - PokePark 2 (Also gets stuck while loading)
  //    - Wii System Menu (Can't access the system settings, gets stuck on blank screen)
  //    - The Beatles: Rock Band (saving doesn't work)

  // Check if the file has already been opened.
  auto search = m_open_files.find(host_path);
  if (search != m_open_files.end())
  {
    // Lock a shared pointer to use.
    return search->second.lock();
  }

  // All files are opened read/write. Actual access rights will be controlled per handle by the
  // read/write functions below
  File::IOFile file;
  while (!file.Open(host_path, "r+b"))
  {
    const bool try_again =
        PanicYesNoFmt("File \"{}\" could not be opened!\n"
                      "This may happen with improper permissions or use by another process.\n"
                      "Press \"Yes\" to make another attempt.",
                      host_path);

    if (!try_again)
    {
      // We've failed to open the file:
      ERROR_LOG_FMT(IOS_FS, "OpenHostFile {}", host_path);
      return nullptr;
    }
  }

#ifdef __SWITCH__
  constexpr size_t NAND_WRITE_BUFFER_SIZE = 16 * 1024;
  if (std::setvbuf(file.GetHandle(), nullptr, _IOFBF, NAND_WRITE_BUFFER_SIZE) != 0)
    WARN_LOG_FMT(IOS_FS, "Failed to allocate the NAND write buffer for {}", host_path);
#endif

  // This code will be called when all references to the shared pointer below have been removed.
  auto deleter = [this, host_path](File::IOFile* ptr) {
    delete ptr;                     // IOFile's deconstructor closes the file.
    m_open_files.erase(host_path);  // erase the weak pointer from the list of open files.
  };

  // Use the custom deleter from above.
  std::shared_ptr<File::IOFile> file_ptr(new File::IOFile(std::move(file)), deleter);

  // Store a weak pointer to our newly opened file in the cache.
  m_open_files[host_path] = std::weak_ptr<File::IOFile>(file_ptr);

  return file_ptr;
}

Result<FileHandle> HostFileSystem::OpenFile(Uid, Gid, const std::string& path, Mode mode)
{
  Handle* handle = AssignFreeHandle();
  if (!handle)
    return std::unexpected{ResultCode::NoFreeHandle};

  const std::string host_path = BuildFilename(path).host_path;

#ifdef __SWITCH__
  // Reuse native handles before querying a devoptab path again.
  if (const auto existing = m_open_files.find(host_path); existing != m_open_files.end())
    handle->host_file = existing->second.lock();
#endif

  if (handle->host_file)
  {
#ifdef __SWITCH__
    const auto shared_handle = std::ranges::find_if(m_handles, [&](const Handle& other) {
      return &other != handle && other.opened && other.host_file == handle->host_file;
    });
    if (shared_handle != m_handles.end())
    {
      handle->file_size = shared_handle->file_size;
      handle->host_file_offset = shared_handle->host_file_offset;
      handle->last_operation = shared_handle->last_operation;
    }
    else
    {
      handle->file_size = static_cast<u32>(handle->host_file->GetSize());
      handle->host_file_offset = static_cast<u32>(handle->host_file->Tell());
    }
#endif
    handle->wii_path = path;
    handle->mode = mode;
    handle->file_offset = 0;
    return FileHandle{this, ConvertHandleToFd(handle)};
  }

  if (File::IsDirectory(host_path))
  {
    *handle = Handle{};
    return std::unexpected{ResultCode::Invalid};
  }

  if (!File::IsFile(host_path))
  {
    *handle = Handle{};
    return std::unexpected{ResultCode::NotFound};
  }

  handle->host_file = OpenHostFile(host_path);
  if (!handle->host_file)
  {
    *handle = Handle{};
    return std::unexpected{ResultCode::AccessDenied};
  }

  handle->wii_path = path;
  handle->mode = mode;
  handle->file_offset = 0;
#ifdef __SWITCH__
  handle->file_size = static_cast<u32>(handle->host_file->GetSize());
  handle->host_file_offset = static_cast<u32>(handle->host_file->Tell());
#endif
  return FileHandle{this, ConvertHandleToFd(handle)};
}

ResultCode HostFileSystem::Close(Fd fd)
{
  Handle* handle = GetHandleFromFd(fd);
  if (!handle)
    return ResultCode::Invalid;

  // Let go of our pointer to the file, it will automatically close if we are the last handle
  // accessing it.
  *handle = Handle{};
  return ResultCode::Success;
}

Result<u32> HostFileSystem::ReadBytesFromFile(Fd fd, u8* ptr, u32 count)
{
  Handle* handle = GetHandleFromFd(fd);
  if (!handle || !handle->host_file->IsOpen())
    return std::unexpected{ResultCode::Invalid};

  if ((u8(handle->mode) & u8(Mode::Read)) == 0)
    return std::unexpected{ResultCode::AccessDenied};

#ifdef __SWITCH__
  const u32 file_size = handle->file_size;
#else
  const u32 file_size = static_cast<u32>(handle->host_file->GetSize());
#endif
  if (handle->file_offset > file_size)
    return std::unexpected{ResultCode::Invalid};

  // IOS has this check in the read request handler.
  if (count > file_size - handle->file_offset)
    count = file_size - handle->file_offset;

#ifdef __SWITCH__
  if (count == 0)
    return 0;

  if (handle->last_operation != Handle::Operation::Read ||
      handle->host_file_offset != handle->file_offset)
  {
    if (!handle->host_file->Seek(handle->file_offset, File::SeekOrigin::Begin))
      return std::unexpected{ResultCode::AccessDenied};
  }
#else
  // File might be opened twice, need to seek before we read
  handle->host_file->Seek(handle->file_offset, File::SeekOrigin::Begin);
#endif
  const u32 actually_read = static_cast<u32>(fread(ptr, 1, count, handle->host_file->GetHandle()));

#ifdef __SWITCH__
  const u32 new_host_offset = handle->file_offset + actually_read;
  for (Handle& other : m_handles)
  {
    if (other.opened && other.host_file == handle->host_file)
    {
      other.host_file_offset = new_host_offset;
      other.last_operation = Handle::Operation::Read;
    }
  }
#endif

  if (actually_read != count && ferror(handle->host_file->GetHandle()))
    return std::unexpected{ResultCode::AccessDenied};

  // IOS returns the number of bytes read and adds that value to the seek position,
  // instead of adding the *requested* read length.
  handle->file_offset += actually_read;
  return actually_read;
}

Result<u32> HostFileSystem::WriteBytesToFile(Fd fd, const u8* ptr, u32 count)
{
  Handle* handle = GetHandleFromFd(fd);
  if (!handle || !handle->host_file->IsOpen())
    return std::unexpected{ResultCode::Invalid};

  if ((u8(handle->mode) & u8(Mode::Write)) == 0)
    return std::unexpected{ResultCode::AccessDenied};

#ifdef __SWITCH__
  if (count == 0)
    return 0;

  if (handle->last_operation != Handle::Operation::Write ||
      handle->host_file_offset != handle->file_offset)
  {
    if (!handle->host_file->Seek(handle->file_offset, File::SeekOrigin::Begin))
      return std::unexpected{ResultCode::AccessDenied};
  }
#else
  // File might be opened twice, need to seek before we read
  handle->host_file->Seek(handle->file_offset, File::SeekOrigin::Begin);
#endif
  if (!handle->host_file->WriteBytes(ptr, count))
    return std::unexpected{ResultCode::AccessDenied};

  handle->file_offset += count;
#ifdef __SWITCH__
  for (Handle& other : m_handles)
  {
    if (other.opened && other.host_file == handle->host_file)
    {
      other.file_size = std::max(other.file_size, handle->file_offset);
      other.host_file_offset = handle->file_offset;
      other.last_operation = Handle::Operation::Write;
    }
  }
#endif
  return count;
}

Result<u32> HostFileSystem::SeekFile(Fd fd, std::uint32_t offset, SeekMode mode)
{
  Handle* handle = GetHandleFromFd(fd);
  if (!handle || !handle->host_file->IsOpen())
    return std::unexpected{ResultCode::Invalid};

  u32 new_position = 0;
  switch (mode)
  {
  case SeekMode::Set:
    new_position = offset;
    break;
  case SeekMode::Current:
    new_position = handle->file_offset + offset;
    break;
  case SeekMode::End:
#ifdef __SWITCH__
    new_position = handle->file_size + offset;
#else
    new_position = handle->host_file->GetSize() + offset;
#endif
    break;
  default:
    return std::unexpected{ResultCode::Invalid};
  }

  // This differs from POSIX behaviour which allows seeking past the end of the file.
#ifdef __SWITCH__
  if (handle->file_size < new_position)
#else
  if (handle->host_file->GetSize() < new_position)
#endif
    return std::unexpected{ResultCode::Invalid};

  handle->file_offset = new_position;
  return handle->file_offset;
}

Result<FileStatus> HostFileSystem::GetFileStatus(Fd fd)
{
  const Handle* handle = GetHandleFromFd(fd);
  if (!handle || !handle->host_file->IsOpen())
    return std::unexpected{ResultCode::Invalid};

  FileStatus status;
#ifdef __SWITCH__
  status.size = handle->file_size;
#else
  status.size = handle->host_file->GetSize();
#endif
  status.offset = handle->file_offset;
  return status;
}

HostFileSystem::Handle* HostFileSystem::AssignFreeHandle()
{
  const auto it =
      std::ranges::find_if(m_handles, [](const Handle& handle) { return !handle.opened; });
  if (it == m_handles.end())
    return nullptr;

  *it = Handle{};
  it->opened = true;
  return &*it;
}

HostFileSystem::Handle* HostFileSystem::GetHandleFromFd(Fd fd)
{
  if (fd >= m_handles.size() || !m_handles[fd].opened)
    return nullptr;
  return &m_handles[fd];
}

Fd HostFileSystem::ConvertHandleToFd(const Handle* handle) const
{
  return handle - m_handles.data();
}

}  // namespace IOS::HLE::FS
