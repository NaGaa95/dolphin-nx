// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: MPL-2.0

// The Switch 2 build's Forwarder (Forwarder.h): HOME menu shortcuts are not available there. Installing one needs
// ns, ncm and spl, which kaiHEN does not provide to homebrew, and a forwarder title would boot without kaiHEN anyway.
// Every call fails with a message the launcher shows; nothing touches the system's services.

#include "DolphinSwitch/Forwarder.h"

#include <cstdio>
#include <string>

#if !defined(__SWITCH2__)
#error "ForwarderSwitch2.cpp is only for the Switch 2 build"
#endif

namespace DolphinSwitch::Forwarder
{
namespace
{
std::string s_self_path;

bool Refuse(char* error, std::size_t error_size)
{
  if (error && error_size)
    std::snprintf(error, error_size, "HOME menu shortcuts are not available on the Switch 2.");
  return false;
}
}  // namespace

void SetSelfPath(std::string path)
{
  s_self_path = std::move(path);
}

bool CreateLauncher(char* error, std::size_t error_size)
{
  return Refuse(error, error_size);
}

bool Create(const std::string&, const std::string&, const std::string&, const std::string&,
            const std::string&, const std::string&, const std::vector<std::string>&, char* error,
            std::size_t error_size)
{
  return Refuse(error, error_size);
}

bool CreateNANDTitle(std::uint64_t, const std::string&, const std::string&, const std::string&,
                     const std::string&, char* error, std::size_t error_size)
{
  return Refuse(error, error_size);
}
}  // namespace DolphinSwitch::Forwarder
