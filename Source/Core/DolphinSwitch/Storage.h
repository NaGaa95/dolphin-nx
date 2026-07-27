// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace DolphinSwitch::Storage
{
struct SmbShare
{
  std::string id;
  std::string name;
  std::string server;
  std::string share;
  std::string path;
  std::string user;
  std::string password;
  std::string domain;
  bool auto_mount = true;
};

struct Location
{
  std::string path;
  std::string label;
};

bool InitializeUsb(std::string* error = nullptr);
std::uint64_t UsbStatusGeneration();
std::vector<Location> ListUsbLocations();

bool MountSmb(const SmbShare& share, std::string* error = nullptr);
bool UnmountSmb(const std::string& id);
bool IsSmbMounted(const std::string& id);
std::string SmbRootPath(const std::string& id);
std::string SmbBrowsePath(const SmbShare& share);

void Shutdown();
}  // namespace DolphinSwitch::Storage
