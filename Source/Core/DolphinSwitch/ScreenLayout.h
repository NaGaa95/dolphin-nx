// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "Common/MathUtil.h"

namespace DolphinSwitch::ScreenLayout
{
MathUtil::Rectangle<int> ApplyMain(const MathUtil::Rectangle<int>& rectangle, int width,
                                   int height);
MathUtil::Rectangle<float> ApplyGBA(std::size_t port, const MathUtil::Rectangle<float>& rectangle,
                                    float width, float height);

void BeginSession(std::string game_id, unsigned revision);
void EndSession();
void BeginEdit(const std::array<bool, 4>& gba_ports);
void CancelEdit();
bool UpdateEditor(std::uint64_t down, std::uint64_t held, float move_x = 0, float move_y = 0,
                  float size_x = 0, float size_y = 0);
void DrawEditor();
}  // namespace DolphinSwitch::ScreenLayout
