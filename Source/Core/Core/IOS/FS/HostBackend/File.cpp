// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/FS/HostBackend/FS.h"

#include <algorithm>
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>

#ifdef __SWITCH__
#include <unistd.h>
#endif

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#ifdef __SWITCH__
#include "Common/Timer.h"
#endif

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
  constexpr size_t NAND_READ_BUFFER_SIZE = 16 * 1024;
  if (std::setvbuf(file.GetHandle(), nullptr, _IOFBF, NAND_READ_BUFFER_SIZE) != 0)
    WARN_LOG_FMT(IOS_FS, "Failed to allocate the NAND read buffer for {}", host_path);
#endif

  // This code will be called when all references to the shared pointer below have been removed.
  auto deleter = [this, host_path](File::IOFile* ptr) {
#ifdef __SWITCH__
    if (const auto it = m_host_file_caches.find(ptr); it != m_host_file_caches.end())
    {
      FlushHostFile(*ptr, it->second);
      m_host_file_caches.erase(it);
    }
#endif
    delete ptr;                     // IOFile's deconstructor closes the file.
    m_open_files.erase(host_path);  // erase the weak pointer from the list of open files.
  };

  // Use the custom deleter from above.
  std::shared_ptr<File::IOFile> file_ptr(new File::IOFile(std::move(file)), deleter);

  // Store a weak pointer to our newly opened file in the cache.
  m_open_files[host_path] = std::weak_ptr<File::IOFile>(file_ptr);
#ifdef __SWITCH__
  const u64 size = file_ptr->GetSize();
  m_host_file_caches[file_ptr.get()] = HostFileCache{.size = size, .host_size = size};
#endif

  return file_ptr;
}

#ifdef __SWITCH__
HostFileSystem::HostFileCache* HostFileSystem::FindHostFileCache(const Handle& handle)
{
  const auto it = m_host_file_caches.find(handle.host_file.get());
  return it != m_host_file_caches.end() ? &it->second : nullptr;
}

void HostFileSystem::BufferHostWrite(File::IOFile& file, HostFileCache& cache, u64 offset,
                                     const u8* ptr, u32 count)
{
  constexpr u64 MAX_PENDING_BYTES = 8 * 1024 * 1024;

  if (cache.pending.empty())
    cache.dirty_since_ms = Common::Timer::NowMs();

  const u64 end = offset + count;
  auto first = cache.pending.upper_bound(offset);
  if (first != cache.pending.begin())
  {
    const auto prev = std::prev(first);
    if (prev->first + prev->second.size() >= offset)
      first = prev;
  }
  auto last = first;
  while (last != cache.pending.end() && last->first <= end)
    ++last;

  if (first == last)
  {
    cache.pending.emplace(offset, std::vector<u8>(ptr, ptr + count));
    cache.pending_bytes += count;
  }
  else
  {
    const u64 merged_start = std::min(offset, first->first);
    const auto final_extent = std::prev(last);
    const u64 merged_end = std::max(end, final_extent->first + final_extent->second.size());

    std::vector<u8> merged;
    auto it = first;
    if (first->first == merged_start)
    {
      merged = std::move(first->second);
      cache.pending_bytes -= merged.size();
      ++it;
    }
    merged.resize(merged_end - merged_start);
    for (; it != last; ++it)
    {
      std::ranges::copy(it->second, merged.begin() + (it->first - merged_start));
      cache.pending_bytes -= it->second.size();
    }
    std::copy(ptr, ptr + count, merged.begin() + (offset - merged_start));

    cache.pending.erase(first, last);
    cache.pending_bytes += merged.size();
    cache.pending.emplace(merged_start, std::move(merged));
  }

  cache.size = std::max(cache.size, end);
  if (cache.pending_bytes >= MAX_PENDING_BYTES)
    FlushHostFile(file, cache);
}

void HostFileSystem::FlushHostFile(File::IOFile& file, HostFileCache& cache)
{
  if (cache.pending.empty())
    return;

  cache.position_valid = false;
  file.ClearError();
  bool ok = file.Flush() && (cache.size <= cache.host_size || file.Resize(cache.size));
  for (const auto& [offset, data] : cache.pending)
  {
    if (!ok)
      break;
    ok = file.Seek(offset, File::SeekOrigin::Begin) && file.WriteBytes(data.data(), data.size());
  }
  ok = ok && file.Flush() && fsync(fileno(file.GetHandle())) == 0;

  if (!ok)
  {
    if (!cache.flush_failed)
      PanicAlertFmt("IOS_FS: Failed to write buffered data to a NAND file");
    cache.flush_failed = true;
    cache.dirty_since_ms = Common::Timer::NowMs();
    return;
  }

  cache.pending.clear();
  cache.pending_bytes = 0;
  cache.host_size = cache.size;
  cache.flush_failed = false;
}

void HostFileSystem::FlushStaleWrites()
{
  constexpr u64 MAX_DIRTY_MS = 1000;
  constexpr u64 MAX_FST_DIRTY_MS = 5000;

  u64 now = 0;
  for (auto& [file, cache] : m_host_file_caches)
  {
    if (cache.pending.empty())
      continue;
    if (now == 0)
      now = Common::Timer::NowMs();
    if (now - cache.dirty_since_ms >= MAX_DIRTY_MS)
      FlushHostFile(*file, cache);
  }

  if (m_fst_dirty && Common::Timer::NowMs() - m_last_fst_write_ms >= MAX_FST_DIRTY_MS)
    FlushFst();
}
#endif

u64 HostFileSystem::GetHostFileSize(const Handle& handle) const
{
#ifdef __SWITCH__
  if (const auto it = m_host_file_caches.find(handle.host_file.get());
      it != m_host_file_caches.end())
  {
    return it->second.size;
  }
#endif
  return handle.host_file->GetSize();
}

u64 HostFileSystem::GetHostFileSize(const std::string& host_path) const
{
#ifdef __SWITCH__
  if (const auto open = m_open_files.find(host_path); open != m_open_files.end())
  {
    if (const std::shared_ptr<File::IOFile> file = open->second.lock())
    {
      if (const auto it = m_host_file_caches.find(file.get()); it != m_host_file_caches.end())
        return it->second.size;
    }
  }
#endif
  return File::GetSize(host_path);
}

std::optional<u32> HostFileSystem::ReadHostFile(const Handle& handle, u8* ptr, u32 count)
{
#ifdef __SWITCH__
  if (HostFileCache* cache = FindHostFileCache(handle))
  {
    const u64 offset = handle.file_offset;
    u32 actually_read = count;
    if (offset < cache->host_size)
    {
      const u32 host_count = static_cast<u32>(std::min<u64>(count, cache->host_size - offset));
      if (!cache->position_valid || cache->position != offset)
        handle.host_file->Seek(offset, File::SeekOrigin::Begin);
      const u32 host_read =
          static_cast<u32>(fread(ptr, 1, host_count, handle.host_file->GetHandle()));
      cache->position = offset + host_read;
      cache->position_valid = true;
      if (host_read != host_count)
      {
        if (ferror(handle.host_file->GetHandle()))
        {
          cache->position_valid = false;
          return std::nullopt;
        }
        actually_read = host_read;
      }
    }

    const u64 end = offset + actually_read;
    auto it = cache->pending.upper_bound(offset);
    if (it != cache->pending.begin())
      --it;
    for (; it != cache->pending.end() && it->first < end; ++it)
    {
      const u64 extent_end = it->first + it->second.size();
      const u64 copy_start = std::max(offset, it->first);
      const u64 copy_end = std::min(end, extent_end);
      if (copy_start >= copy_end)
        continue;
      std::copy(it->second.begin() + (copy_start - it->first),
                it->second.begin() + (copy_end - it->first), ptr + (copy_start - offset));
    }
    return actually_read;
  }
#endif

  handle.host_file->Seek(handle.file_offset, File::SeekOrigin::Begin);
  const u32 actually_read = static_cast<u32>(fread(ptr, 1, count, handle.host_file->GetHandle()));

  if (actually_read != count && ferror(handle.host_file->GetHandle()))
    return std::nullopt;
  return actually_read;
}

bool HostFileSystem::WriteHostFile(const Handle& handle, const u8* ptr, u32 count)
{
#ifdef __SWITCH__
  if (HostFileCache* cache = FindHostFileCache(handle))
  {
    BufferHostWrite(*handle.host_file, *cache, handle.file_offset, ptr, count);
    return true;
  }
#endif

  handle.host_file->Seek(handle.file_offset, File::SeekOrigin::Begin);
  return handle.host_file->WriteBytes(ptr, count);
}

Result<FileHandle> HostFileSystem::OpenFile(Uid, Gid, const std::string& path, Mode mode)
{
  Handle* handle = AssignFreeHandle();
  if (!handle)
    return std::unexpected{ResultCode::NoFreeHandle};

  const std::string host_path = BuildFilename(path).host_path;
#ifdef __SWITCH__
  // Open files skip the host lookup.
  const auto open_file = m_open_files.find(host_path);
  const bool already_open = open_file != m_open_files.end() && !open_file->second.expired();
#else
  constexpr bool already_open = false;
#endif
  if (!already_open && File::IsDirectory(host_path))
  {
    *handle = Handle{};
    return std::unexpected{ResultCode::Invalid};
  }

  if (!already_open && !File::IsFile(host_path))
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

  const u32 file_size = static_cast<u32>(GetHostFileSize(*handle));
  if (handle->file_offset > file_size)
    return std::unexpected{ResultCode::Invalid};

  // IOS has this check in the read request handler.
  if (count > file_size - handle->file_offset)
    count = file_size - handle->file_offset;

  const std::optional<u32> read = ReadHostFile(*handle, ptr, count);
  if (!read)
    return std::unexpected{ResultCode::AccessDenied};
  const u32 actually_read = *read;

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

  if (!WriteHostFile(*handle, ptr, count))
    return std::unexpected{ResultCode::AccessDenied};

  handle->file_offset += count;
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
    new_position = GetHostFileSize(*handle) + offset;
    break;
  default:
    return std::unexpected{ResultCode::Invalid};
  }

  // This differs from POSIX behaviour which allows seeking past the end of the file.
  if (GetHostFileSize(*handle) < new_position)
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
  status.size = GetHostFileSize(*handle);
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
