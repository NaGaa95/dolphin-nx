// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/Updater.h"

#include <curl/curl.h>
#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifndef DOLPHIN_SWITCH_RELEASE_VERSION
#define DOLPHIN_SWITCH_RELEASE_VERSION "1.0.2"
#endif

namespace DolphinSwitch::Updater
{
namespace
{
constexpr char kLatestReleaseUrl[] =
    "https://api.github.com/repos/NaGaa95/dolphin-nx/releases/latest";
constexpr char kReleaseAssetPrefix[] = "https://github.com/NaGaa95/dolphin-nx/releases/download/";
constexpr char kDefaultLauncherPath[] = "sdmc:/switch/dolphin/dolphin.nro";
constexpr std::size_t kMaxReleaseResponse = 2 * 1024 * 1024;
constexpr std::uint64_t kMinimumNroSize = 1024 * 1024;
constexpr std::uint64_t kMaximumNroSize = 512ULL * 1024 * 1024;

std::mutex s_mutex;
std::thread s_worker;
State s_state{State::Idle};
ReleaseInfo s_release;
std::string s_error;
std::atomic_bool s_cancel{false};
std::atomic_bool s_install_requested{false};
std::atomic_uint64_t s_downloaded{0};
std::atomic_uint64_t s_total{0};

bool StartsWith(std::string_view value, std::string_view prefix)
{
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

void AppendUtf8(std::string& output, unsigned codepoint)
{
  if (codepoint <= 0x7f)
    output += static_cast<char>(codepoint);
  else if (codepoint <= 0x7ff)
  {
    output += static_cast<char>(0xc0 | (codepoint >> 6));
    output += static_cast<char>(0x80 | (codepoint & 0x3f));
  }
  else if (codepoint <= 0xffff)
  {
    output += static_cast<char>(0xe0 | (codepoint >> 12));
    output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
    output += static_cast<char>(0x80 | (codepoint & 0x3f));
  }
  else if (codepoint <= 0x10ffff)
  {
    output += static_cast<char>(0xf0 | (codepoint >> 18));
    output += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
    output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
    output += static_cast<char>(0x80 | (codepoint & 0x3f));
  }
}

int HexDigit(char value)
{
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

bool ParseJsonString(const std::string& json, std::size_t position, std::string& output)
{
  if (position >= json.size() || json[position] != '"')
    return false;
  std::string parsed;
  for (std::size_t index = position + 1; index < json.size(); ++index)
  {
    const unsigned char value = static_cast<unsigned char>(json[index]);
    if (value == '"')
    {
      output.swap(parsed);
      return true;
    }
    if (value != '\\')
    {
      if (value != 0)
        parsed += static_cast<char>(value);
      continue;
    }
    if (++index >= json.size())
      return false;
    const char escaped = json[index];
    if (escaped == '"' || escaped == '\\' || escaped == '/')
      parsed += escaped;
    else if (escaped == 'b')
      parsed += ' ';
    else if (escaped == 'f')
      parsed += ' ';
    else if (escaped == 'n')
      parsed += '\n';
    else if (escaped == 'r')
      parsed += '\r';
    else if (escaped == 't')
      parsed += '\t';
    else if (escaped == 'u')
    {
      if (index + 4 >= json.size())
        return false;
      unsigned codepoint = 0;
      for (int digit = 0; digit < 4; ++digit)
      {
        const int part = HexDigit(json[++index]);
        if (part < 0)
          return false;
        codepoint = (codepoint << 4) | static_cast<unsigned>(part);
      }
      if (codepoint >= 0xd800 && codepoint <= 0xdbff && index + 6 < json.size() &&
          json[index + 1] == '\\' && json[index + 2] == 'u')
      {
        unsigned low = 0;
        bool valid = true;
        for (int digit = 0; digit < 4; ++digit)
        {
          const int part = HexDigit(json[index + 3 + digit]);
          if (part < 0)
          {
            valid = false;
            break;
          }
          low = (low << 4) | static_cast<unsigned>(part);
        }
        if (valid && low >= 0xdc00 && low <= 0xdfff)
        {
          codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
          index += 6;
        }
      }
      AppendUtf8(parsed, codepoint);
    }
    else
      return false;
  }
  return false;
}

std::size_t JsonFieldValue(const std::string& object, const char* field)
{
  const std::string key = std::string("\"") + field + "\"";
  std::size_t position = object.find(key);
  if (position == std::string::npos)
    return position;
  position = object.find(':', position + key.size());
  if (position == std::string::npos)
    return position;
  do
    ++position;
  while (position < object.size() && std::isspace(static_cast<unsigned char>(object[position])));
  return position;
}

bool JsonString(const std::string& object, const char* field, std::string& output)
{
  const std::size_t position = JsonFieldValue(object, field);
  return position != std::string::npos && ParseJsonString(object, position, output);
}

bool JsonUnsigned(const std::string& object, const char* field, std::uint64_t& output)
{
  const std::size_t position = JsonFieldValue(object, field);
  if (position == std::string::npos)
    return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long value = std::strtoull(object.c_str() + position, &end, 10);
  if (errno != 0 || end == object.c_str() + position)
    return false;
  output = static_cast<std::uint64_t>(value);
  return true;
}

std::vector<std::string> JsonArrayObjects(const std::string& json, const char* field)
{
  std::vector<std::string> objects;
  const std::size_t fieldPosition = json.find(std::string("\"") + field + "\"");
  if (fieldPosition == std::string::npos)
    return objects;
  const std::size_t array = json.find('[', fieldPosition);
  if (array == std::string::npos)
    return objects;
  bool quoted = false;
  bool escaped = false;
  int depth = 0;
  std::size_t start = std::string::npos;
  for (std::size_t index = array + 1; index < json.size(); ++index)
  {
    const char value = json[index];
    if (quoted)
    {
      if (escaped)
        escaped = false;
      else if (value == '\\')
        escaped = true;
      else if (value == '"')
        quoted = false;
      continue;
    }
    if (value == '"')
    {
      quoted = true;
      continue;
    }
    if (value == '{')
    {
      if (depth++ == 0)
        start = index;
    }
    else if (value == '}' && depth > 0)
    {
      if (--depth == 0 && start != std::string::npos)
      {
        objects.emplace_back(json.substr(start, index - start + 1));
        start = std::string::npos;
      }
    }
    else if (value == ']' && depth == 0)
      break;
  }
  return objects;
}

bool ParseRelease(const std::string& json, ReleaseInfo& release, std::string& error)
{
  if (!JsonString(json, "tag_name", release.tag) || release.tag.empty())
  {
    error = "GitHub returned a release without a valid tag.";
    return false;
  }
  JsonString(json, "name", release.name);
  JsonString(json, "body", release.notes);
  JsonString(json, "html_url", release.page_url);
  if (release.name.empty())
    release.name = release.tag;
  if (release.notes.empty())
    release.notes = "No release notes were provided.";

  int bestAsset = -1;
  const auto assets = JsonArrayObjects(json, "assets");
  for (std::size_t index = 0; index < assets.size(); ++index)
  {
    std::string name;
    std::string url;
    std::uint64_t size = 0;
    if (!JsonString(assets[index], "name", name) ||
        !JsonString(assets[index], "browser_download_url", url) ||
        !JsonUnsigned(assets[index], "size", size))
      continue;
    const std::string lowerName = Lower(name);
    if (lowerName.size() < 4 || lowerName.substr(lowerName.size() - 4) != ".nro" ||
        size < kMinimumNroSize || size > kMaximumNroSize || !StartsWith(url, kReleaseAssetPrefix))
      continue;
    if (bestAsset < 0 || lowerName == "dolphin.nro")
    {
      bestAsset = static_cast<int>(index);
      release.asset_name = std::move(name);
      release.asset_url = std::move(url);
      release.asset_size = size;
      JsonString(assets[index], "digest", release.asset_digest);
      if (lowerName == "dolphin.nro")
        break;
    }
  }
  return true;
}

struct MemoryDownload
{
  std::string data;
  std::size_t limit{};
};

size_t MemoryWrite(void* pointer, size_t size, size_t count, void* userdata)
{
  if (count != 0 && size > std::numeric_limits<size_t>::max() / count)
    return 0;
  const size_t bytes = size * count;
  auto& download = *static_cast<MemoryDownload*>(userdata);
  if (s_cancel.load(std::memory_order_relaxed) || bytes > download.limit - download.data.size())
    return 0;
  download.data.append(static_cast<const char*>(pointer), bytes);
  return bytes;
}

int TransferProgress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
  return s_cancel.load(std::memory_order_relaxed) ? 1 : 0;
}

void SetCommonCurlOptions(CURL* curl)
{
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Dolphin-NX-Updater/" DOLPHIN_SWITCH_RELEASE_VERSION);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, TransferProgress);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
}

bool FetchLatestRelease(ReleaseInfo& release, std::string& error)
{
  CURL* curl = curl_easy_init();
  if (!curl)
  {
    error = "Could not initialize the network request.";
    return false;
  }
  MemoryDownload download{{}, kMaxReleaseResponse};
  char curlError[CURL_ERROR_SIZE]{};
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
  headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2026-03-10");
  curl_easy_setopt(curl, CURLOPT_URL, kLatestReleaseUrl);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, MemoryWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMaxReleaseResponse));
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
  SetCommonCurlOptions(curl);

  const CURLcode result = curl_easy_perform(curl);
  long responseCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (s_cancel.load(std::memory_order_relaxed))
  {
    error = "Update check cancelled.";
    return false;
  }
  if (result != CURLE_OK)
  {
    error = curlError[0] ? curlError : curl_easy_strerror(result);
    return false;
  }
  if (responseCode == 403)
  {
    error = "GitHub refused the request or its anonymous rate limit was reached.";
    return false;
  }
  if (responseCode < 200 || responseCode >= 300)
  {
    error = "GitHub returned HTTP " + std::to_string(responseCode) + ".";
    return false;
  }
  return ParseRelease(download.data, release, error);
}

bool ParseSha256(const std::string& digest, std::array<u8, SHA256_HASH_SIZE>& output)
{
  constexpr std::string_view prefix = "sha256:";
  if (digest.size() != prefix.size() + SHA256_HASH_SIZE * 2 || !StartsWith(Lower(digest), prefix))
    return false;
  for (std::size_t index = 0; index < output.size(); ++index)
  {
    const int high = HexDigit(digest[prefix.size() + index * 2]);
    const int low = HexDigit(digest[prefix.size() + index * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    output[index] = static_cast<u8>((high << 4) | low);
  }
  return true;
}

bool ValidNro(const std::string& path, std::uint64_t expectedSize)
{
  struct stat fileStat{};
  if (stat(path.c_str(), &fileStat) != 0 || fileStat.st_size <= 0 ||
      (expectedSize != 0 && static_cast<std::uint64_t>(fileStat.st_size) != expectedSize))
    return false;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return false;
  NroStart start{};
  NroHeader header{};
  const bool read = std::fread(&start, 1, sizeof(start), file) == sizeof(start) &&
                    std::fread(&header, 1, sizeof(header), file) == sizeof(header);
  std::fclose(file);
  if (!read || header.magic != NROHEADER_MAGIC || header.size < sizeof(start) + sizeof(header) ||
      header.size > static_cast<u64>(fileStat.st_size))
    return false;
  for (const auto& segment : header.segments)
    if (segment.file_off > header.size || segment.size > header.size - segment.file_off)
      return false;
  return true;
}

struct FileDownload
{
  FILE* file{};
  Sha256Context hash{};
  std::uint64_t written{};
  std::uint64_t maximum{};
  bool writeFailed{};
};

size_t FileWrite(void* pointer, size_t size, size_t count, void* userdata)
{
  if (count != 0 && size > std::numeric_limits<size_t>::max() / count)
    return 0;
  const size_t bytes = size * count;
  auto& download = *static_cast<FileDownload*>(userdata);
  if (s_cancel.load(std::memory_order_relaxed) || bytes > download.maximum - download.written ||
      std::fwrite(pointer, 1, bytes, download.file) != bytes)
  {
    download.writeFailed = true;
    return 0;
  }
  sha256ContextUpdate(&download.hash, pointer, bytes);
  download.written += bytes;
  s_downloaded.store(download.written, std::memory_order_relaxed);
  return bytes;
}

int DownloadProgress(void*, curl_off_t total, curl_off_t current, curl_off_t, curl_off_t)
{
  if (total > 0)
    s_total.store(static_cast<std::uint64_t>(total), std::memory_order_relaxed);
  if (current >= 0)
    s_downloaded.store(static_cast<std::uint64_t>(current), std::memory_order_relaxed);
  return s_cancel.load(std::memory_order_relaxed) ? 1 : 0;
}

bool HasDownloadSpace(const std::string& path, std::uint64_t size)
{
  struct statvfs info{};
  if (statvfs(path.c_str(), &info) != 0)
    return true;
  const unsigned __int128 available = static_cast<unsigned __int128>(info.f_bavail) * info.f_frsize;
  return available >= static_cast<unsigned __int128>(size) + 8 * 1024 * 1024;
}

bool ReplaceLauncher(const std::string& launcherPath, const std::string& temporary,
                     std::string& error)
{
  const std::string backup = launcherPath + ".update.old";
  struct stat currentStat{};
  const bool haveCurrent = lstat(launcherPath.c_str(), &currentStat) == 0 &&
                           S_ISREG(currentStat.st_mode) && !S_ISLNK(currentStat.st_mode);
  if (!haveCurrent)
  {
    error = "The installed launcher is missing or is not a regular file.";
    return false;
  }
  struct stat backupStat{};
  if (lstat(backup.c_str(), &backupStat) == 0 && std::remove(backup.c_str()) != 0)
  {
    error = "Could not remove the previous update backup.";
    return false;
  }
  if (std::rename(launcherPath.c_str(), backup.c_str()) != 0)
  {
    error = "Could not preserve the current launcher.";
    return false;
  }
  if (std::rename(temporary.c_str(), launcherPath.c_str()) != 0)
  {
    std::rename(backup.c_str(), launcherPath.c_str());
    fsdevCommitDevice("sdmc");
    error = "Could not activate the downloaded launcher.";
    return false;
  }
  fsdevCommitDevice("sdmc");
  if (!ValidNro(launcherPath, 0))
  {
    std::remove(launcherPath.c_str());
    std::rename(backup.c_str(), launcherPath.c_str());
    fsdevCommitDevice("sdmc");
    error = "The installed launcher failed final validation.";
    return false;
  }
  return true;
}

bool DownloadRelease(const ReleaseInfo& release, const std::string& launcherPath,
                     std::string& error)
{
  std::array<u8, SHA256_HASH_SIZE> expectedHash{};
  if (!ParseSha256(release.asset_digest, expectedHash))
  {
    error = "The GitHub release asset does not provide a valid SHA-256 digest.";
    return false;
  }
  if (!HasDownloadSpace(launcherPath, release.asset_size))
  {
    error = "There is not enough free SD-card space for this update.";
    return false;
  }
  const std::string temporary = launcherPath + ".update.tmp";
  std::remove(temporary.c_str());
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file)
  {
    error = "Could not create the update file on the SD card.";
    return false;
  }
  FileDownload download{};
  download.file = file;
  download.maximum = release.asset_size;
  sha256ContextCreate(&download.hash);
  CURL* curl = curl_easy_init();
  if (!curl)
  {
    std::fclose(file);
    std::remove(temporary.c_str());
    error = "Could not initialize the update download.";
    return false;
  }
  char curlError[CURL_ERROR_SIZE]{};
  curl_easy_setopt(curl, CURLOPT_URL, release.asset_url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FileWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(release.asset_size));
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
  SetCommonCurlOptions(curl);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, DownloadProgress);

  const CURLcode result = curl_easy_perform(curl);
  long responseCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
  curl_easy_cleanup(curl);
  bool fileOk = !download.writeFailed;
  if (std::fflush(file) != 0 || fsync(fileno(file)) != 0)
    fileOk = false;
  if (std::fclose(file) != 0)
    fileOk = false;
  fsdevCommitDevice("sdmc");

  if (s_cancel.load(std::memory_order_relaxed))
  {
    std::remove(temporary.c_str());
    error = "Update download cancelled.";
    return false;
  }
  if (result != CURLE_OK || responseCode < 200 || responseCode >= 300 || !fileOk)
  {
    std::remove(temporary.c_str());
    error = curlError[0] ? curlError : "The update download failed.";
    return false;
  }

  std::array<u8, SHA256_HASH_SIZE> actualHash{};
  sha256ContextGetHash(&download.hash, actualHash.data());
  if (actualHash != expectedHash || !ValidNro(temporary, release.asset_size))
  {
    std::remove(temporary.c_str());
    error = actualHash != expectedHash ? "The downloaded update failed SHA-256 verification." :
                                         "The downloaded update is not a valid NRO.";
    return false;
  }
  return true;
}

void JoinWorker()
{
  if (s_worker.joinable())
    s_worker.join();
}

std::string NormalizeTag(std::string tag)
{
  while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.front())))
    tag.erase(tag.begin());
  while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.back())))
    tag.pop_back();
  if (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V'))
    tag.erase(tag.begin());
  return tag;
}

bool ParseVersion(const std::string& tag, std::vector<std::uint64_t>& parts)
{
  const std::string normalized = NormalizeTag(tag);
  std::size_t position = 0;
  while (position < normalized.size())
  {
    if (!std::isdigit(static_cast<unsigned char>(normalized[position])))
      break;
    std::uint64_t value = 0;
    while (position < normalized.size() &&
           std::isdigit(static_cast<unsigned char>(normalized[position])))
    {
      const unsigned digit = normalized[position++] - '0';
      if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
        return false;
      value = value * 10 + digit;
    }
    parts.push_back(value);
    if (position == normalized.size() || normalized[position] == '-' || normalized[position] == '+')
      break;
    if (normalized[position++] != '.')
      return false;
  }
  return !parts.empty();
}
}  // namespace

std::string ResolveLauncherPath(std::string_view path)
{
  std::string candidate(path);
  std::replace(candidate.begin(), candidate.end(), '\\', '/');
  const std::string lower = Lower(candidate);
  const bool relative_component =
      candidate.find("/../") != std::string::npos || candidate.ends_with("/..") ||
      candidate.find("/./") != std::string::npos || candidate.ends_with("/.");
  if (candidate.find('\0') != std::string::npos || !candidate.starts_with("sdmc:/") ||
      relative_component || !lower.ends_with(".nro"))
  {
    return kDefaultLauncherPath;
  }
  return candidate;
}

const char* BuiltReleaseTag()
{
  return DOLPHIN_SWITCH_RELEASE_VERSION;
}

bool IsNewer(const std::string& candidate, const std::string& installed)
{
  const std::string candidateTag = NormalizeTag(candidate);
  const std::string installedTag = NormalizeTag(installed);
  if (candidateTag == installedTag)
    return false;
  std::vector<std::uint64_t> candidateParts;
  std::vector<std::uint64_t> installedParts;
  if (!ParseVersion(candidateTag, candidateParts) || !ParseVersion(installedTag, installedParts))
    return true;
  const std::size_t count = std::max(candidateParts.size(), installedParts.size());
  for (std::size_t index = 0; index < count; ++index)
  {
    const std::uint64_t candidatePart = index < candidateParts.size() ? candidateParts[index] : 0;
    const std::uint64_t installedPart = index < installedParts.size() ? installedParts[index] : 0;
    if (candidatePart != installedPart)
      return candidatePart > installedPart;
  }
  return true;
}

bool StartCheck(const std::string& installedTag)
{
  {
    std::scoped_lock lock(s_mutex);
    if (s_state == State::Checking || s_state == State::Downloading ||
        s_state == State::ReadyToInstall || s_state == State::Installing)
      return false;
  }
  JoinWorker();
  s_cancel.store(false, std::memory_order_relaxed);
  s_downloaded.store(0, std::memory_order_relaxed);
  s_total.store(0, std::memory_order_relaxed);
  {
    std::scoped_lock lock(s_mutex);
    s_state = State::Checking;
    s_release = {};
    s_error.clear();
  }
  s_worker = std::thread([installedTag] {
    ReleaseInfo release;
    std::string error;
    const bool success = FetchLatestRelease(release, error);
    std::scoped_lock lock(s_mutex);
    if (!success)
    {
      s_error = std::move(error);
      s_state = s_cancel.load(std::memory_order_relaxed) ? State::Cancelled : State::Error;
      return;
    }
    s_release = std::move(release);
    if (IsNewer(s_release.tag, installedTag))
    {
      if (s_release.asset_url.empty())
      {
        s_error = "The latest release does not contain a downloadable NRO asset.";
        s_state = State::Error;
      }
      else
        s_state = State::UpdateAvailable;
    }
    else
      s_state = State::UpToDate;
  });
  return true;
}

bool StartDownload(const std::string& launcherPath)
{
  ReleaseInfo release;
  {
    std::scoped_lock lock(s_mutex);
    if (s_state != State::UpdateAvailable)
      return false;
    release = s_release;
  }
  JoinWorker();
  s_cancel.store(false, std::memory_order_relaxed);
  s_downloaded.store(0, std::memory_order_relaxed);
  s_total.store(release.asset_size, std::memory_order_relaxed);
  {
    std::scoped_lock lock(s_mutex);
    s_error.clear();
    s_state = State::Downloading;
  }
  s_worker = std::thread([release = std::move(release), launcherPath] {
    std::string error;
    const bool success = DownloadRelease(release, launcherPath, error);
    std::scoped_lock lock(s_mutex);
    if (success)
      s_state = State::ReadyToInstall;
    else
    {
      s_error = std::move(error);
      s_state = s_cancel.load(std::memory_order_relaxed) ? State::Cancelled : State::Error;
    }
  });
  return true;
}

bool InstallDownloaded(const std::string& launcherPath)
{
  {
    std::scoped_lock lock(s_mutex);
    if (s_state != State::ReadyToInstall)
      return false;
    s_state = State::Installing;
  }
  JoinWorker();
  std::string error;
  const bool success = ReplaceLauncher(launcherPath, launcherPath + ".update.tmp", error);
  {
    std::scoped_lock lock(s_mutex);
    if (success)
      s_state = State::Installed;
    else
    {
      s_error = std::move(error);
      s_state = State::Error;
    }
  }
  return success;
}

void Cancel()
{
  s_cancel.store(true, std::memory_order_relaxed);
}

void RequestInstallation()
{
  s_install_requested.store(true, std::memory_order_release);
}

bool ConsumeInstallationRequest()
{
  return s_install_requested.exchange(false, std::memory_order_acq_rel);
}

Snapshot GetSnapshot()
{
  Snapshot snapshot;
  {
    std::scoped_lock lock(s_mutex);
    snapshot.state = s_state;
    snapshot.release = s_release;
    snapshot.error = s_error;
  }
  snapshot.downloaded = s_downloaded.load(std::memory_order_relaxed);
  snapshot.total = s_total.load(std::memory_order_relaxed);
  return snapshot;
}

void Shutdown()
{
  s_cancel.store(true, std::memory_order_relaxed);
  JoinWorker();
}

bool RecoverInstallation(const std::string& launcherPath, std::string& error)
{
  const std::string backup = launcherPath + ".update.old";
  const std::string temporary = launcherPath + ".update.tmp";
  struct stat currentStat{};
  struct stat backupStat{};
  const bool haveCurrent = lstat(launcherPath.c_str(), &currentStat) == 0;
  const bool haveBackup = lstat(backup.c_str(), &backupStat) == 0;
  if (lstat(temporary.c_str(), &currentStat) == 0 && std::remove(temporary.c_str()) != 0)
  {
    error = "Could not remove an incomplete update download.";
    return false;
  }
  if (!haveCurrent && haveBackup)
  {
    if (!ValidNro(backup, 0) || std::rename(backup.c_str(), launcherPath.c_str()) != 0)
    {
      error = "Could not restore the previous launcher after an interrupted update.";
      return false;
    }
    fsdevCommitDevice("sdmc");
    return true;
  }
  if (haveCurrent && haveBackup)
  {
    if (ValidNro(launcherPath, 0))
    {
      if (std::remove(backup.c_str()) != 0)
      {
        error = "Could not remove the previous launcher backup.";
        return false;
      }
    }
    else
    {
      if (!ValidNro(backup, 0) || std::remove(launcherPath.c_str()) != 0 ||
          std::rename(backup.c_str(), launcherPath.c_str()) != 0)
      {
        error = "Could not roll back an invalid launcher update.";
        return false;
      }
    }
    fsdevCommitDevice("sdmc");
  }
  return true;
}
}  // namespace DolphinSwitch::Updater
