// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include <switch.h>

class ControllerInterface;

namespace ciface
{
class InputBackend;

namespace Switch
{
enum class JoyConLayout
{
  Auto,
  Dual,
  Left,
  Right,
};

struct TouchscreenState
{
  bool pressed = false;
  u32 x = 0;
  u32 y = 0;
};

std::unique_ptr<ciface::InputBackend> CreateInputBackend(
    ControllerInterface* controller_interface);

JoyConLayout GetJoyConLayout(std::size_t player);
void SetJoyConLayout(std::size_t player, JoyConLayout layout);
std::string_view GetJoyConLayoutName(JoyConLayout layout);

// libnx's PadState helper does not merge the NSO controller state lifos.
void UpdatePadState(PadState* pad);
TouchscreenState GetTouchscreenState();
}
}  // namespace ciface
