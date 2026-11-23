// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// GameShare (Switch 2): another Nintendo Switch or Switch 2 console joins a local GameShare session
// (Common/nx2_gameshare) and plays the game with this console. It gets a screen of its own on the
// Vulkan and Deko3D backends (a copy of this console's picture, or the screen of the GBA it plays),
// or mirrors this console's screen on OpenGL, and its controller drives one player
// ("GameShare/0/Joypad"). On the Switch these do nothing.

#include <string>

namespace Core
{
class System;
}

namespace DolphinSwitch::GameShare
{
// The player the guest console plays (Switch/GameSharePlayer)
enum class Role
{
  Player2,  // GameCube port 2 or Wii Remote 2
  Player1,  // port 1 or Wii Remote 1; this console's controllers move to the next players
  GBA,      // a Game Boy Advance on GameCube port 2, its screen on the guest's (GameCube games)
};

enum class BeginResult
{
  Started,
  Cancelled,
  Failed,
};

// The launcher asked for this game to run with GameShare
void Configure(bool requested);
bool Requested();
bool Active();
// Once the game runs: the emulation pauses, the backend opens the guest's screen, the system's
// matchmaking applet runs and the guest takes its player
BeginResult Begin(Core::System& system, bool is_wii, std::string* error);
// Every host loop iteration
void Update();
Role GetRole();
bool RoleAvailable(Role role);
// The quick menu's GameShare player
void SetRole(Core::System& system, Role role);
// Once the core has shut down
void End();
// The presenter, after the frame's drawing (quick menu included) and before its present
void CopyFrameToGuest();
}  // namespace DolphinSwitch::GameShare
