// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived in part from Cemu-NX SteamGridDB code (MPL-2.0).

#include "DolphinSwitch/CoverDownload.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef DOLPHIN_SWITCH_RELEASE_VERSION
#define DOLPHIN_SWITCH_RELEASE_VERSION "1.0.3"
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

struct TransferContext
{
  const RequestOptions* options = nullptr;
  TransferStage stage = TransferStage::Image;
};

bool IsCancelled(const RequestOptions* options)
{
  return options && options->cancel && options->cancel->load(std::memory_order_relaxed);
}

int TransferCallback(void* user, curl_off_t download_total, curl_off_t downloaded, curl_off_t,
                     curl_off_t)
{
  const auto* context = static_cast<const TransferContext*>(user);
  if (!context || IsCancelled(context->options))
    return 1;
  if (context->options && context->options->progress)
  {
    const auto value = [](curl_off_t size) {
      return size > 0 ? static_cast<std::uint64_t>(size) : std::uint64_t{0};
    };
    context->options->progress(context->stage, value(downloaded), value(download_total));
  }
  return IsCancelled(context->options) ? 1 : 0;
}

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

enum class HttpResult
{
  Ok,
  Failed,
  Cancelled,
};

HttpResult HttpGet(const std::string& url, const std::string& bearer, std::string* output,
                   long* status_code, TransferStage stage, const RequestOptions* options)
{
  if (status_code)
    *status_code = 0;
  if (IsCancelled(options))
    return HttpResult::Cancelled;
  if (!s_initialized || !output)
    return HttpResult::Failed;
  CURL* curl = curl_easy_init();
  if (!curl)
    return HttpResult::Failed;
  Buffer buffer;
  const TransferContext transfer{options, stage};
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
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, TransferCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
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
  if (IsCancelled(options))
    return HttpResult::Cancelled;
  return result == CURLE_OK ? HttpResult::Ok : HttpResult::Failed;
}

Result RequestResult(HttpResult result)
{
  return result == HttpResult::Cancelled ? Result::Cancelled : Result::NetworkError;
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

std::string NormalizeTitle(const std::string& title)
{
  std::string normalized;
  bool separator = true;
  for (const unsigned char value : title)
  {
    if (std::isalnum(value))
    {
      normalized += static_cast<char>(std::tolower(value));
      separator = false;
    }
    else if (!separator && !normalized.empty())
    {
      normalized += ' ';
      separator = true;
    }
  }
  if (!normalized.empty() && normalized.back() == ' ')
    normalized.pop_back();
  return normalized;
}

std::vector<std::string> TitleWords(const std::string& normalized)
{
  std::vector<std::string> words;
  std::size_t start = 0;
  while (start < normalized.size())
  {
    const std::size_t end = normalized.find(' ', start);
    words.emplace_back(normalized.substr(start, end - start));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return words;
}

int GameMatchScore(const std::string& wanted, const GameResult& candidate)
{
  const std::string actual = NormalizeTitle(candidate.name);
  if (actual == wanted)
    return 100000;
  if (actual.empty() || wanted.empty())
    return 0;

  int score = 0;
  if (actual.starts_with(wanted) || wanted.starts_with(actual))
    score += 10000;
  else if (actual.find(wanted) != std::string::npos || wanted.find(actual) != std::string::npos)
    score += 6000;

  const std::vector<std::string> wanted_words = TitleWords(wanted);
  const std::vector<std::string> actual_words = TitleWords(actual);
  for (const std::string& word : wanted_words)
  {
    if (std::find(actual_words.begin(), actual_words.end(), word) != actual_words.end())
      score += 500;
  }
  score -= static_cast<int>(
      std::abs(static_cast<long long>(actual.size()) - static_cast<long long>(wanted.size())));
  return score;
}

int ArtworkScore(const Artwork& artwork)
{
  if (artwork.width <= 0 || artwork.height <= 0)
    return std::numeric_limits<int>::min();
  const double aspect_error =
      std::abs(static_cast<double>(artwork.width) / artwork.height - 2.0 / 3.0);
  const long long pixels = static_cast<long long>(artwork.width) * artwork.height;
  return static_cast<int>(std::min<long long>(pixels / 1000, 5000)) -
         static_cast<int>(aspect_error * 10000.0);
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
                   std::vector<GameResult>* results, const RequestOptions* options)
{
  if (!results)
    return Result::Error;
  results->clear();
  if (api_key.empty())
    return Result::NoKey;
  std::string response;
  long code = 0;
  const HttpResult request =
      HttpGet("https://www.steamgriddb.com/api/v2/search/autocomplete/" + UrlEncode(title), api_key,
              &response, &code, TransferStage::Search, options);
  if (request != HttpResult::Ok)
    return RequestResult(request);
  const Result status = StatusResult(code);
  if (status != Result::Ok)
    return status;
  std::unordered_set<long> seen;
  for (const std::string& object : DataObjects(response))
  {
    long id = 0;
    std::string name;
    if (!ObjectNumber(object, "id", &id) || id <= 0 || !ObjectString(object, "name", &name) ||
        name.empty() || !seen.insert(id).second)
      continue;
    results->push_back({id, std::move(name)});
    if (results->size() >= 32)
      break;
  }
  return results->empty() ? Result::NotFound : Result::Ok;
}

Result FetchArtwork(const std::string& api_key, long game_id, std::vector<Artwork>* artwork,
                    const RequestOptions* options)
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
                "https://www.steamgriddb.com/api/v2/grids/game/"
                "%ld?dimensions=600x900&types=static&mimes=image/png,image/jpeg",
                game_id);
  std::string response;
  long code = 0;
  const HttpResult request =
      HttpGet(endpoint, api_key, &response, &code, TransferStage::Artwork, options);
  if (request != HttpResult::Ok)
    return RequestResult(request);
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

Result FetchIcons(const std::string& api_key, long game_id, std::vector<Artwork>* artwork,
                  const RequestOptions* options)
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
    const HttpResult request =
        HttpGet(endpoint, api_key, &response, &code, TransferStage::Artwork, options);
    if (request != HttpResult::Ok)
      return RequestResult(request);
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
                "https://www.steamgriddb.com/api/v2/grids/game/"
                "%ld?dimensions=1024x1024,512x512&types=static&mimes=image/png,image/jpeg",
                game_id);
  Result first = append_endpoint(endpoint);
  std::snprintf(endpoint, sizeof(endpoint),
                "https://www.steamgriddb.com/api/v2/icons/game/%ld?mimes=image/png&types=static",
                game_id);
  const Result second = append_endpoint(endpoint);
  if (!artwork->empty())
    return Result::Ok;
  if (first == Result::Cancelled || second == Result::Cancelled)
    return Result::Cancelled;
  if (first == Result::NetworkError || second == Result::NetworkError)
    return Result::NetworkError;
  if (first == Result::NoKey || second == Result::NoKey)
    return Result::NoKey;
  return Result::NotFound;
}

Result DownloadImage(const std::string& url, const std::string& output_path,
                     const RequestOptions* options)
{
  std::string data;
  long code = 0;
  if (url.empty())
    return Result::NetworkError;
  const HttpResult request = HttpGet(url, {}, &data, &code, TransferStage::Image, options);
  if (request != HttpResult::Ok)
    return RequestResult(request);
  if (IsCancelled(options))
    return Result::Cancelled;
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

Result DownloadBestCover(const std::string& api_key, const std::string& title,
                         const std::string& output_path, CoverSelection* selection,
                         const RequestOptions* options)
{
  if (selection)
    *selection = {};
  if (IsCancelled(options))
    return Result::Cancelled;

  std::vector<GameResult> games;
  Result result = SearchGames(api_key, title, &games, options);
  if (result != Result::Ok)
    return result;

  const std::string wanted = NormalizeTitle(title);
  std::stable_sort(games.begin(), games.end(),
                   [&](const GameResult& left, const GameResult& right) {
                     return GameMatchScore(wanted, left) > GameMatchScore(wanted, right);
                   });

  // A search occasionally returns a similarly named game without any portrait grids. Try a few
  // close matches before treating the title as missing, while keeping batch requests bounded.
  const std::size_t attempts = std::min<std::size_t>(games.size(), 5);
  Result last_result = Result::NotFound;
  for (std::size_t index = 0; index < attempts; ++index)
  {
    if (IsCancelled(options))
      return Result::Cancelled;
    std::vector<Artwork> artwork;
    result = FetchArtwork(api_key, games[index].id, &artwork, options);
    if (result == Result::Cancelled || result == Result::NoKey || result == Result::NetworkError)
      return result;
    if (result != Result::Ok)
    {
      last_result = result;
      continue;
    }

    const auto best = std::max_element(artwork.begin(), artwork.end(),
                                       [](const Artwork& left, const Artwork& right) {
                                         return ArtworkScore(left) < ArtworkScore(right);
                                       });
    if (best == artwork.end())
      continue;

    result = DownloadImage(best->url, output_path, options);
    if (result != Result::Ok)
      return result;
    if (selection)
      *selection = {games[index].id, games[index].name, *best};
    return Result::Ok;
  }
  return last_result;
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
  case Result::Cancelled:
    return "Cover download cancelled.";
  }
  return "Unexpected cover download error.";
}
}  // namespace DolphinSwitch::CoverDownload
