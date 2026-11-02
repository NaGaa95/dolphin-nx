// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace DolphinSwitch::Skylanders
{
constexpr std::size_t SLOT_COUNT = 16;

struct FigureFile
{
  std::string path;
  std::string name;
};

struct Slot
{
  std::string path;
  std::string name = "Empty";
  bool loaded = false;
};

struct CatalogEntry
{
  std::uint16_t id;
  std::uint16_t variant;
  int game;
  std::string name;
};

std::string CollectionDirectory();
std::string Refresh();
std::array<Slot, SLOT_COUNT> GetSlots();
std::vector<FigureFile> ListCollection(std::string* error);
const std::vector<CatalogEntry>& Catalog();
std::string SetEnabled(bool enabled);
std::string SetFigure(std::size_t slot, const std::string& path);
std::string ClearFigure(std::size_t slot);
std::string CreateFigure(std::uint16_t id, std::uint16_t variant, const std::string& path);
std::string BeginSession();
void EndSession();
}  // namespace DolphinSwitch::Skylanders
