// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/GameShare.h"

#if defined(__SWITCH2__)

#include <algorithm>
#include <array>
#include <atomic>
#include <string>

#include "Common/Config/Config.h"
#include "Common/nx2_gameshare.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Core.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "DolphinSwitch/GBAOverlay.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/AbstractGfx.h"

namespace DolphinSwitch::GameShare
{
namespace
{
constexpr char GUEST_DEVICE[] = "GameShare/0/Joypad";
constexpr int PORT_COUNT = 4;
// The GBA role's port (GameCube port 2)
constexpr int GBA_PORT = 1;

bool s_requested = false;
bool s_is_wii = false;
std::atomic<Role> s_role{Role::Player2};
bool s_devices_saved = false;
std::array<std::string, PORT_COUNT> s_gc_devices;
std::array<std::string, PORT_COUNT> s_wii_devices;
std::array<std::string, PORT_COUNT> s_gba_devices;
std::array<SerialInterface::SIDevices, PORT_COUNT> s_si_devices{};
std::array<WiimoteSource, PORT_COUNT> s_wiimote_sources{};

// The backend's screen on the guest's window, made before matchmaking with the emulation paused
bool OpenScreen(void* window, int, int, void*)
{
  return g_gfx && g_gfx->OpenGuestScreen(window);
}

void CloseScreen(void*)
{
  // (a backend that already shut down closed its screen there)
  if (g_gfx)
    g_gfx->CloseGuestScreen();
}

std::string DeviceOf(InputConfig* config, int port)
{
  const auto* controller = config->GetController(port);
  return controller ? controller->GetDefaultDevice().ToString() : std::string{};
}

void SetDevice(InputConfig* config, int port, const std::string& device)
{
  auto* const controller = config->GetController(port);
  if (!controller || controller->GetDefaultDevice().ToString() == device)
    return;
  controller->SetDefaultDevice(device);
  controller->UpdateReferences(g_controller_interface);
}

void SaveDevices()
{
  if (s_devices_saved)
    return;
  for (int port = 0; port < PORT_COUNT; ++port)
  {
    s_gc_devices[port] = DeviceOf(Pad::GetConfig(), port);
    s_wii_devices[port] = DeviceOf(Wiimote::GetConfig(), port);
    s_gba_devices[port] = DeviceOf(Pad::GetGBAConfig(), port);
    s_si_devices[port] = Config::Get(Config::GetInfoForSIDevice(port));
    s_wiimote_sources[port] = Config::Get(Config::GetInfoForWiimoteSource(port));
  }
  s_devices_saved = true;
}

bool GBARole()
{
  return s_role.load() == Role::GBA && !s_is_wii;
}

// The guest takes its player's controller and the console's controllers move up behind it, in
// order. In the GBA role, GameCube port 2 holds a GBA that the guest plays.
void ApplyRoles()
{
  const bool gba = GBARole();
  const int guest = s_role.load() == Role::Player1 ? 0 : 1;
  const auto source = [guest](const std::array<std::string, PORT_COUNT>& original, int port) {
    return port < guest ? original[port] :
           port == guest ? std::string(GUEST_DEVICE) :
                           original[port - 1];
  };
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    for (int port = 0; port < PORT_COUNT; ++port)
    {
      SetDevice(Pad::GetConfig(), port, source(s_gc_devices, port));
      SetDevice(Wiimote::GetConfig(), port, source(s_wii_devices, port));
      SetDevice(Pad::GetGBAConfig(), port,
                gba && port == GBA_PORT ? std::string(GUEST_DEVICE) : s_gba_devices[port]);
    }
  }

  // The guest's port is plugged in, and the ports behind it follow their controllers. Never under
  // the state lock: a Wii Remote source change refreshes the devices and takes the device mutex.
  Config::ConfigChangeCallbackGuard config_guard;
  for (int port = 0; port < PORT_COUNT; ++port)
  {
    if (s_is_wii)
    {
      Config::SetCurrent(Config::GetInfoForWiimoteSource(port),
                         port < guest   ? s_wiimote_sources[port] :
                         port == guest ? WiimoteSource::Emulated :
                                         s_wiimote_sources[port - 1]);
      continue;
    }
    SerialInterface::SIDevices device = port < guest   ? s_si_devices[port] :
                                        port == guest ? SerialInterface::SIDEVICE_GC_CONTROLLER :
                                                        s_si_devices[port - 1];
    if (port == guest && s_si_devices[port] != SerialInterface::SIDEVICE_NONE &&
        s_si_devices[port] != SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      device = s_si_devices[port];
    if (gba && port == GBA_PORT)
      device = SerialInterface::SIDEVICE_GC_GBA_EMULATED;
    Config::SetCurrent(Config::GetInfoForSIDevice(port), device);
  }
  // (the guest's GBA is on the guest's own screen, not over this console's picture)
  GBAOverlay::SetGuestPort(gba && nx2GameShareSeparateActive() ? GBA_PORT : -1);
}

void RestoreDevices()
{
  if (!s_devices_saved)
    return;
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    for (int port = 0; port < PORT_COUNT; ++port)
    {
      SetDevice(Pad::GetConfig(), port, s_gc_devices[port]);
      SetDevice(Wiimote::GetConfig(), port, s_wii_devices[port]);
      SetDevice(Pad::GetGBAConfig(), port, s_gba_devices[port]);
    }
  }
  s_devices_saved = false;
}
}  // namespace

void Configure(bool requested)
{
  s_requested = requested;
  nx2GameShareConfigure(requested, false);
}

bool Requested()
{
  return s_requested;
}

bool Active()
{
  return nx2GameShareActive();
}

BeginResult Begin(Core::System& system, bool is_wii, std::string* error)
{
  if (!s_requested || nx2GameShareActive())
    return BeginResult::Started;
  s_is_wii = is_wii;
  Role role = static_cast<Role>(std::clamp(Config::Get(Config::MAIN_SWITCH_GAMESHARE_PLAYER), 0, 2));
  if (role == Role::GBA && is_wii)
    role = Role::Player2;
  s_role.store(role);
  nx2GameShareSetSwapRoles(role == Role::Player1);
  SaveDevices();

  // (the game waits, and nothing presents, while the matchmaking applet runs)
  Core::CPUThreadGuard guard(system);
  static const Nx2GameShareScreen screen = {OpenScreen, CloseScreen, nullptr};
  const bool separate = g_gfx && g_gfx->SupportsGuestScreen();
  char text[256] = {};
  switch (nx2GameShareBegin(separate ? &screen : nullptr, text, sizeof(text)))
  {
  case Nx2GameShare_Started:
    ApplyRoles();
    return BeginResult::Started;
  case Nx2GameShare_Cancelled:
    return BeginResult::Cancelled;
  default:
    if (error)
      *error = std::string("GameShare could not start:\n") + text;
    return BeginResult::Failed;
  }
}

void Update()
{
  nx2GameShareUpdate();
}

Role GetRole()
{
  return s_role.load();
}

bool RoleAvailable(Role role)
{
  return role != Role::GBA || !s_is_wii;
}

void SetRole(Core::System& system, Role role)
{
  if (!nx2GameShareActive() || !RoleAvailable(role))
    return;
  s_role.store(role);
  nx2GameShareSetSwapRoles(role == Role::Player1);
  Core::CPUThreadGuard guard(system);
  ApplyRoles();
}

void End()
{
  GBAOverlay::SetGuestPort(-1);
  RestoreDevices();
  nx2GameShareEnd();
  s_requested = false;
}

void CopyFrameToGuest()
{
  if (!g_gfx || !nx2GameShareSeparateActive())
    return;
  // The guest's GBA, once it shows something; else this console's picture
  const AbstractTexture* texture = GBARole() ? GBAOverlay::GetTexture(GBA_PORT) : nullptr;
  g_gfx->CopyToGuestScreen(texture);
}
}  // namespace DolphinSwitch::GameShare

#else

namespace DolphinSwitch::GameShare
{
void Configure(bool)
{
}
bool Requested()
{
  return false;
}
bool Active()
{
  return false;
}
BeginResult Begin(Core::System&, bool, std::string* error)
{
  if (error)
    *error = "GameShare needs a Nintendo Switch 2";
  return BeginResult::Failed;
}
void Update()
{
}
Role GetRole()
{
  return Role::Player2;
}
bool RoleAvailable(Role)
{
  return false;
}
void SetRole(Core::System&, Role)
{
}
void End()
{
}
void CopyFrameToGuest()
{
}
}  // namespace DolphinSwitch::GameShare

#endif
