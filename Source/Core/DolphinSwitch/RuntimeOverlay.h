// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace DolphinSwitch::RuntimeOverlay
{
enum class ControllerMode
{
  GameCube,
  WiiRemote,
  WiiRemoteSideways,
  WiiRemoteNunchuk,
  ClassicController,
};

enum class ActionType
{
  TogglePause,
  StopToLauncher,
  SaveState,
  LoadState,
  Reset,
  EjectDisc,
  ChangeDisc,
  ToggleCheats,
  ToggleCheat,
  ToggleFrameGeneration,
  ToggleFPS,
  SetControllerMode,
};

struct Action
{
  ActionType type{};
  int value = 0;
  std::string path;
  int value2 = 0;
};

void InitializeRendererHook();
void ShutdownRendererHook();

void BeginSession(std::string game_path, std::string game_id, unsigned revision, bool is_wii,
                  std::array<ControllerMode, 4> controller_modes);
void EndSession();

void UpdateInput(std::uint64_t down, std::uint64_t held);
std::vector<Action> TakeActions();

bool IsVisible();
bool IsInputCaptured();
std::uint64_t GetRenderFrameGeneration();

void SetUserPaused(bool paused);
void SetStatus(std::string message);
void ShowAlert(std::string caption, std::string message);
void TrackStateSave(int slot, std::uint64_t previous_timestamp);
void TrackStateLoad(int slot);

void ToggleCheatsForSession();
void ToggleCheatForSession(int flat_index);
void ToggleFrameGenerationForSession();
void ToggleFPSForSession();
void SetControllerModeResult(int player, ControllerMode mode, bool success);

void Draw();
}  // namespace DolphinSwitch::RuntimeOverlay
