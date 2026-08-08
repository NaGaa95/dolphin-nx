// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived in part from Cemu-NX SteamGridDB code (MPL-2.0).

#include "DolphinSwitch/CoverDownload.h"

#include <curl/curl.h>

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef DOLPHIN_SWITCH_RELEASE_VERSION
#define DOLPHIN_SWITCH_RELEASE_VERSION "1.0.1"
#endif

namespace DolphinSwitch::CoverDownload
{
namespace
{
constexpr std::size_t MAX_BODY_SIZE = 24 * 1024 * 1024;
bool s_initialized = false;

struct Buffer
{
  std::string data;
};

std::size_t WriteCallback(void* data, std::size_t size, std::size_t count, void* user)
{
  auto* buffer = static_cast<Buffer*>(user);
  if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count)
    return 0;
  const std::size_t bytes = size * count;
  if (bytes > MAX_BODY_SIZE - buffer->data.size())
    return 0;
  buffer->data.append(static_cast<char*>(data), bytes);
  return bytes;
}

bool HttpGet(const std::string& url, const std::string& bearer, std::string* output,
             long* status_code)
{
  if (status_code)
    *status_code = 0;
  if (!s_initialized || !output)
    return false;
  CURL* curl = curl_easy_init();
  if (!curl)
    return false;
  Buffer buffer;
  curl_slist* headers = nullptr;
  if (!bearer.empty())
  {
    const std::string authorization = "Authorization: Bearer " + bearer;
    headers = curl_slist_append(headers, authorization.c_str());
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  if (headers)
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(MAX_BODY_SIZE));
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Dolphin-NX/" DOLPHIN_SWITCH_RELEASE_VERSION);
  const CURLcode result = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  if (status_code)
    *status_code = code;
  if (headers)
    curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  output->swap(buffer.data);
  return result == CURLE_OK;
}

std::string UrlEncode(const std::string& text)
{
  static constexpr char HEX[] = "0123456789ABCDEF";
  std::string output;
  for (const unsigned char value : text)
  {
    if (std::isalnum(value) || value == '-' || value == '_' || value == '.' || value == '~')
      output += static_cast<char>(value);
    else
    {
      output += '%';
      output += HEX[value >> 4];
      output += HEX[value & 15];
    }
  }
  return output;
}

void AppendUtf8(std::string* output, unsigned codepoint)
{
  if (codepoint <= 0x7f)
    *output += static_cast<char>(codepoint);
  else if (codepoint <= 0x7ff)
  {
    *output += static_cast<char>(0xc0 | (codepoint >> 6));
    *output += static_cast<char>(0x80 | (codepoint & 0x3f));
  }
  else if (codepoint <= 0xffff)
  {
    *output += static_cast<char>(0xe0 | (codepoint >> 12));
    *output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
    *output += static_cast<char>(0x80 | (codepoint & 0x3f));
  }
  else if (codepoint <= 0x10ffff)
  {
    *output += static_cast<char>(0xf0 | (codepoint >> 18));
    *output += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
    *output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
    *output += static_cast<char>(0x80 | (codepoint & 0x3f));
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

bool ParseJsonString(const std::string& json, std::size_t position, std::string* output)
{
  if (!output || position >= json.size() || json[position] != '"')
    return false;
  std::string parsed;
  for (std::size_t index = position + 1; index < json.size(); ++index)
  {
    const unsigned char value = static_cast<unsigned char>(json[index]);
    if (value == '"')
    {
      output->swap(parsed);
      return true;
    }
    if (value != '\\')
    {
      parsed += static_cast<char>(value);
      continue;
    }
    if (++index >= json.size())
      return false;
    const char escaped = json[index];
    if (escaped == '"' || escaped == '\\' || escaped == '/')
      parsed += escaped;
    else if (escaped == 'b')
      parsed += '\b';
    else if (escaped == 'f')
      parsed += '\f';
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
      AppendUtf8(&parsed, codepoint);
    }
    else
    {
      return false;
    }
  }
  return false;
}

std::vector<std::string> DataObjects(const std::string& json)
{
  std::vector<std::string> objects;
  const std::size_t data = json.find("\"data\"");
  const std::size_t array = data == std::string::npos ? data : json.find('[', data + 6);
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
      quoted = true;
    else if (value == '{')
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
    {
      break;
    }
  }
  return objects;
}

std::size_t FieldValue(const std::string& object, const char* field)
{
  const std::string key = std::string("\"") + field + "\"";
  std::size_t position = object.find(key);
  if (position == std::string::npos)
    return position;
  position = object.find(':', position + key.size());
  if (position == std::string::npos)
    return position;
  do
  {
    ++position;
  } while (position < object.size() && std::isspace(static_cast<unsigned char>(object[position])));
  return position;
}

bool ObjectString(const std::string& object, const char* field, std::string* output)
{
  const std::size_t position = FieldValue(object, field);
  return position != std::string::npos && ParseJsonString(object, position, output);
}

bool ObjectNumber(const std::string& object, const char* field, long* output)
{
  const std::size_t position = FieldValue(object, field);
  if (position == std::string::npos || !output)
    return false;
  char* end = nullptr;
  const long value = std::strtol(object.c_str() + position, &end, 10);
  if (end == object.c_str() + position)
    return false;
  *output = value;
  return true;
}

Result StatusResult(long code)
{
  if (code == 401 || code == 403)
    return Result::NoKey;
  if (code < 200 || code >= 300)
    return Result::NotFound;
  return Result::Ok;
}
}  // namespace

bool Initialize()
{
  if (s_initialized)
    return true;
  s_initialized = curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK;
  return s_initialized;
}

void Shutdown()
{
  if (s_initialized)
    curl_global_cleanup();
  s_initialized = false;
}

Result SearchGames(const std::string& api_key, const std::string& title,
                   std::vector<GameResult>* results)
{
  if (!results)
    return Result::Error;
  results->clear();
  if (api_key.empty())
    return Result::NoKey;
  std::string response;
  long code = 0;
  if (!HttpGet("https://www.steamgriddb.com/api/v2/search/autocomplete/" + UrlEncode(title),
               api_key, &response, &code))
    return Result::NetworkError;
  const Result status = StatusResult(code);
  if (status != Result::Ok)
    return status;
  std::unordered_set<long> seen;
  for (const std::string& object : DataObjects(response))
  {
    long id = 0;
    std::string name;
    if (!ObjectNumber(object, "id", &id) || id <= 0 ||
        !ObjectString(object, "name", &name) || name.empty() || !seen.insert(id).second)
      continue;
    results->push_back({id, std::move(name)});
    if (results->size() >= 32)
      break;
  }
  return results->empty() ? Result::NotFound : Result::Ok;
}

Result FetchArtwork(const std::string& api_key, long game_id,
                    std::vector<Artwork>* artwork)
{
  if (!artwork)
    return Result::Error;
  artwork->clear();
  if (api_key.empty())
    return Result::NoKey;
  if (game_id <= 0)
    return Result::NotFound;
  char endpoint[384];
  std::snprintf(endpoint, sizeof(endpoint),
                "https://www.steamgriddb.com/api/v2/grids/game/%ld?dimensions=600x900&types=static&mimes=image/png,image/jpeg",
                game_id);
  std::string response;
  long code = 0;
  if (!HttpGet(endpoint, api_key, &response, &code))
    return Result::NetworkError;
  const Result status = StatusResult(code);
  if (status != Result::Ok)
    return status;
  std::unordered_set<std::string> seen;
  for (const std::string& object : DataObjects(response))
  {
    Artwork item;
    long width = 0;
    long height = 0;
    ObjectNumber(object, "width", &width);
    ObjectNumber(object, "height", &height);
    if (!ObjectString(object, "url", &item.url) || item.url.empty() ||
        !seen.insert(item.url).second)
      continue;
    if (!ObjectString(object, "thumb", &item.thumbnail_url) || item.thumbnail_url.empty())
      item.thumbnail_url = item.url;
    item.width = static_cast<int>(width);
    item.height = static_cast<int>(height);
    artwork->emplace_back(std::move(item));
    if (artwork->size() >= 100)
      break;
  }
  return artwork->empty() ? Result::NotFound : Result::Ok;
}

Result FetchIcons(const std::string& api_key, long game_id, std::vector<Artwork>* artwork)
{
  if (!artwork)
    return Result::Error;
  artwork->clear();
  if (api_key.empty())
    return Result::NoKey;
  if (game_id <= 0)
    return Result::NotFound;

  std::unordered_set<std::string> seen;
  const auto append_endpoint = [&](const std::string& endpoint) {
    std::string response;
    long code = 0;
    if (!HttpGet(endpoint, api_key, &response, &code))
      return Result::NetworkError;
    const Result status = StatusResult(code);
    if (status != Result::Ok)
      return status;
    for (const std::string& object : DataObjects(response))
    {
      Artwork item;
      long width = 0;
      long height = 0;
      ObjectNumber(object, "width", &width);
      ObjectNumber(object, "height", &height);
      if (!ObjectString(object, "url", &item.url) || item.url.empty() ||
          !seen.insert(item.url).second)
        continue;
      if (!ObjectString(object, "thumb", &item.thumbnail_url) || item.thumbnail_url.empty())
        item.thumbnail_url = item.url;
      item.width = static_cast<int>(width);
      item.height = static_cast<int>(height);
      artwork->emplace_back(std::move(item));
      if (artwork->size() >= 42)
        break;
    }
    return Result::Ok;
  };

  char endpoint[384];
  std::snprintf(endpoint, sizeof(endpoint),
                "https://www.steamgriddb.com/api/v2/grids/game/%ld?dimensions=1024x1024,512x512&types=static&mimes=image/png,image/jpeg",
                game_id);
  Result first = append_endpoint(endpoint);
  std::snprintf(endpoint, sizeof(endpoint),
                "https://www.steamgriddb.com/api/v2/icons/game/%ld?mimes=image/png&types=static",
                game_id);
  const Result second = append_endpoint(endpoint);
  if (!artwork->empty())
    return Result::Ok;
  if (first == Result::NetworkError || second == Result::NetworkError)
    return Result::NetworkError;
  if (first == Result::NoKey || second == Result::NoKey)
    return Result::NoKey;
  return Result::NotFound;
}

Result DownloadImage(const std::string& url, const std::string& output_path)
{
  std::string data;
  long code = 0;
  if (url.empty() || !HttpGet(url, {}, &data, &code))
    return Result::NetworkError;
  if (code < 200 || code >= 300 || data.size() < 64)
    return Result::NotFound;
  const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
  const bool png = bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
  const bool jpg = bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff;
  if (!png && !jpg)
    return Result::Error;
  const std::string temporary = output_path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file)
    return Result::Error;
  const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
  const bool closed = std::fclose(file) == 0;
  if (!written || !closed)
  {
    std::remove(temporary.c_str());
    return Result::Error;
  }

  // Horizon's filesystem rename does not replace an existing destination. Preserve the current
  // image until the completed download has been moved into place so changing covers is safe.
  const std::string backup = output_path + ".bak";
  errno = 0;
  if (std::remove(backup.c_str()) != 0 && errno != ENOENT)
  {
    std::remove(temporary.c_str());
    return Result::Error;
  }

  errno = 0;
  const bool had_existing = std::rename(output_path.c_str(), backup.c_str()) == 0;
  if (!had_existing && errno != ENOENT)
  {
    std::remove(temporary.c_str());
    return Result::Error;
  }

  if (std::rename(temporary.c_str(), output_path.c_str()) != 0)
  {
    if (had_existing)
      std::rename(backup.c_str(), output_path.c_str());
    std::remove(temporary.c_str());
    return Result::Error;
  }
  if (had_existing)
    std::remove(backup.c_str());
  return Result::Ok;
}

const char* ResultMessage(Result result)
{
  switch (result)
  {
  case Result::Ok:
    return "Complete";
  case Result::NoKey:
    return "The SteamGridDB API key is missing or was rejected.";
  case Result::NetworkError:
    return "Could not connect to SteamGridDB.";
  case Result::NotFound:
    return "No matching artwork was found.";
  case Result::Error:
    return "SteamGridDB returned invalid data or the cover could not be written.";
  }
  return "Unexpected cover download error.";
}
}  // namespace DolphinSwitch::CoverDownload
