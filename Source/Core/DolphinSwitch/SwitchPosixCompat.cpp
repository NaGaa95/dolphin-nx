// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from Cemu-NX compatibility code (MPL-2.0); libnx lacks readv/writev.

#include <cerrno>
#include <limits>
#include <sys/_iovec.h>
#include <sys/types.h>
#include <unistd.h>

extern "C"
{
ssize_t readv(int fd, const struct iovec* vectors, int count);
ssize_t writev(int fd, const struct iovec* vectors, int count);
}

namespace
{
bool ValidateVectors(const struct iovec* vectors, int count)
{
  if (count < 0 || (count > 0 && !vectors))
  {
    errno = count < 0 ? EINVAL : EFAULT;
    return false;
  }
#ifdef IOV_MAX
  if (count > IOV_MAX)
  {
    errno = EINVAL;
    return false;
  }
#endif
  size_t total = 0;
  for (int index = 0; index < count; ++index)
  {
    if (vectors[index].iov_len >
        static_cast<size_t>(std::numeric_limits<ssize_t>::max()) - total)
    {
      errno = EINVAL;
      return false;
    }
    total += vectors[index].iov_len;
  }
  return true;
}
}  // namespace

extern "C" ssize_t readv(int fd, const struct iovec* vectors, int count)
{
  if (!ValidateVectors(vectors, count))
    return -1;

  ssize_t total = 0;
  for (int index = 0; index < count; ++index)
  {
    const ssize_t result = read(fd, vectors[index].iov_base, vectors[index].iov_len);
    if (result < 0)
      return total ? total : -1;
    total += result;
    if (static_cast<size_t>(result) < vectors[index].iov_len)
      break;
  }
  return total;
}

extern "C" ssize_t writev(int fd, const struct iovec* vectors, int count)
{
  if (!ValidateVectors(vectors, count))
    return -1;

  ssize_t total = 0;
  for (int index = 0; index < count; ++index)
  {
    const ssize_t result = write(fd, vectors[index].iov_base, vectors[index].iov_len);
    if (result < 0)
      return total ? total : -1;
    total += result;
    if (static_cast<size_t>(result) < vectors[index].iov_len)
      break;
  }
  return total;
}
