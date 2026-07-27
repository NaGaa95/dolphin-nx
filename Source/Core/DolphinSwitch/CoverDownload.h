// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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

bool Initialize();
void Shutdown();
Result SearchGames(const std::string& api_key, const std::string& title,
                   std::vector<GameResult>* results);
Result FetchArtwork(const std::string& api_key, long game_id,
                    std::vector<Artwork>* artwork);
Result FetchIcons(const std::string& api_key, long game_id, std::vector<Artwork>* artwork);
Result DownloadImage(const std::string& url, const std::string& output_path);
const char* ResultMessage(Result result);
}  // namespace DolphinSwitch::CoverDownload
