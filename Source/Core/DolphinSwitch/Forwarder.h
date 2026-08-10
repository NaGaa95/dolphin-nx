// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace DolphinSwitch::Forwarder
{
void SetSelfPath(std::string path);

bool Create(const std::string& game_path, const std::string& name, const std::string& author,
            const std::string& icon_image_path, const std::string& game_config_path, char* error,
            std::size_t error_size);
bool CreateNANDTitle(std::uint64_t title_id, const std::string& name, const std::string& author,
                     const std::string& icon_image_path, char* error, std::size_t error_size);
}  // namespace DolphinSwitch::Forwarder
