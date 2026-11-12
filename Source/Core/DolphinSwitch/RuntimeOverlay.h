// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "DolphinSwitch/Skylanders.h"

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
  ToggleVBISkip,
  SetControllerMode,
  ResetGBA,
  ToggleSkylandersPortal,
  ListSkylanderFigures,
  LoadSkylanderFigure,
  RemoveSkylanderFigure,
  CalibrateTouchPointer,
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

void UpdateInput(std::uint64_t down, std::uint64_t held, bool gamecube_adapter = false,
                 float move_x = 0, float move_y = 0, float size_x = 0, float size_y = 0);
std::vector<Action> TakeActions();

bool IsVisible();
bool IsInputCaptured();
std::uint64_t GetRenderFrameGeneration();

void SetSkylanderFigures(std::vector<Skylanders::FigureFile> figures, std::string error);
void SetStatus(std::string message);
// Shows a short message over the running game while the menu is closed.
void ShowBanner(std::string message);
// Draws a calibration target at a position normalized to the game image, over the running game.
void SetTouchCalibration(bool active, float target_x = 0, float target_y = 0,
                         std::string instructions = {});
void ShowAlert(std::string caption, std::string message);
void TrackStateSave(int slot, std::uint64_t previous_timestamp);
void TrackStateLoad(int slot);

void ToggleCheatsForSession();
void ToggleCheatForSession(int flat_index);
void ToggleFrameGenerationForSession();
void ToggleFPSForSession();
void ToggleVBISkipForSession();
void SetControllerModeResult(int player, ControllerMode mode, bool success);

void Draw();
}  // namespace DolphinSwitch::RuntimeOverlay
