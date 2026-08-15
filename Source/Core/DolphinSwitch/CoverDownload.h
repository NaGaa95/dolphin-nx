// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace DolphinSwitch::CoverDownload
{
enum class Result
{
  Ok,
  NoKey,
  NetworkError,
  NotFound,
  Error,
  Cancelled,
};

enum class TransferStage
{
  Search,
  Artwork,
  Image,
};

// RequestOptions is only observed for the duration of a synchronous operation. The caller owns
// cancel and must keep it alive until that operation returns. Progress is invoked on the calling
// thread (which is normally a launcher worker thread).
struct RequestOptions
{
  const std::atomic_bool* cancel = nullptr;
  std::function<void(TransferStage stage, std::uint64_t received, std::uint64_t total)> progress;
};

struct GameResult
{
  long id = 0;
  std::string name;
};

struct Artwork
{
  std::string url;
  std::string thumbnail_url;
  int width = 0;
  int height = 0;
};

struct CoverSelection
{
  long game_id = 0;
  std::string game_name;
  Artwork artwork;
};

bool Initialize();
void Shutdown();
Result SearchGames(const std::string& api_key, const std::string& title,
                   std::vector<GameResult>* results, const RequestOptions* options = nullptr);
Result FetchArtwork(const std::string& api_key, long game_id, std::vector<Artwork>* artwork,
                    const RequestOptions* options = nullptr);
Result FetchIcons(const std::string& api_key, long game_id, std::vector<Artwork>* artwork,
                  const RequestOptions* options = nullptr);
Result DownloadImage(const std::string& url, const std::string& output_path,
                     const RequestOptions* options = nullptr);

// Finds the closest SteamGridDB game/title match, chooses a portrait cover, and atomically writes
// it to output_path. This is intended for cancellable batch "missing cover" jobs.
Result DownloadBestCover(const std::string& api_key, const std::string& title,
                         const std::string& output_path, CoverSelection* selection = nullptr,
                         const RequestOptions* options = nullptr);
const char* ResultMessage(Result result);
}  // namespace DolphinSwitch::CoverDownload
