// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <switch.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HorizonClocks.h"
#include "Common/HorizonFastmem.h"
#include "Common/IniFile.h"
#include "Common/MsgHandler.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/SwitchSaltyNX.h"
#include "Common/Thread.h"
#include "Common/WindowSystemInfo.h"
#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/ExtensionPort.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/Host.h"
#include "Core/Movie.h"
#include "Core/State.h"
#include "Core/System.h"
#include "DiscIO/GameModDescriptor.h"
#include "DiscIO/RiivolutionParser.h"
#include "DiscIO/RiivolutionPatcher.h"
#include "DolphinSwitch/Audio.h"
#include "DolphinSwitch/EmbeddedRuntime.h"
#include "DolphinSwitch/Forwarder.h"
#include "DolphinSwitch/GBAOverlay.h"
#include "DolphinSwitch/GciSync.h"
#include "DolphinSwitch/Launcher.h"
#include "DolphinSwitch/RuntimeOverlay.h"
#include "DolphinSwitch/Skylanders.h"
#include "DolphinSwitch/SystemLanguage.h"
#include "DolphinSwitch/Updater.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/Switch/Switch.h"
#include "InputCommon/InputConfig.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#ifdef HAS_DEKO3D
#include "VideoBackends/Deko3D/DKFrameGenerationShaders.h"
#endif

extern "C" {
u32 __nx_applet_type = AppletType_Application;
u32 __nx_applet_PerformanceConfiguration[2] = {0x92220007, 0};
}

namespace
{
std::atomic<bool> s_session_running{false};
std::atomic<bool> s_host_focused{true};
std::atomic<bool> s_applet_exit_requested{false};
std::atomic<std::uint64_t> s_operation_mode_generation{0};
std::atomic<std::uint64_t> s_wii_touch_pointer_state{0};
std::mutex s_alert_mutex;

struct PendingAlert
{
  std::string caption;
  std::string text;
};

std::vector<PendingAlert> s_pending_alerts;

constexpr std::string_view VULKAN_RETURN_ARGUMENT = "--dolphin-vulkan-return";

bool ScheduleLauncherRestart(const std::string& launcher_path)
{
  if (launcher_path.empty() || !envHasNextLoad())
  {
    std::fprintf(stderr,
                 "[Dolphin Switch] Vulkan return: hbloader relaunch is unavailable; exiting "
                 "instead of reusing the released display layer\n");
    return false;
  }

  const std::string arguments = launcher_path + " " + std::string(VULKAN_RETURN_ARGUMENT);
  const Result result = envSetNextLoad(launcher_path.c_str(), arguments.c_str());
  if (R_FAILED(result))
    std::fprintf(stderr, "[Dolphin Switch] failed to schedule launcher restart: %08x\n",
                 static_cast<unsigned int>(result));
  return R_SUCCEEDED(result);
}

bool ConfigureDefaultNWindow(NWindow* window, u32 width, u32 height)
{
  if (!window || !nwindowIsValid(window))
    return false;

  const Result dimensions_result = nwindowSetDimensions(window, width, height);
  const Result crop_result = nwindowSetCrop(window, 0, 0, width, height);
  const Result interval_result = nwindowSetSwapInterval(window, 1);
  const bool success =
      R_SUCCEEDED(dimensions_result) && R_SUCCEEDED(crop_result) && R_SUCCEEDED(interval_result);

  return success;
}

class ScopedEnvironmentOverrides final
{
public:
  void Set(const char* name, const char* value)
  {
    const char* const old_value = std::getenv(name);
    m_previous.emplace_back(name, old_value ? std::optional<std::string>(old_value) : std::nullopt);
    setenv(name, value, 1);
  }

  ~ScopedEnvironmentOverrides()
  {
    for (auto iterator = m_previous.rbegin(); iterator != m_previous.rend(); ++iterator)
    {
      if (iterator->second)
        setenv(iterator->first.c_str(), iterator->second->c_str(), 1);
      else
        unsetenv(iterator->first.c_str());
    }
  }

private:
  std::vector<std::pair<std::string, std::optional<std::string>>> m_previous;
};

bool IsAppletMode()
{
  const AppletType type = appletGetAppletType();
  return type != AppletType_Application && type != AppletType_SystemApplication;
}

std::optional<DolphinSwitch::LaunchRequest> GetDirectLaunchRequest(int argc, char** argv)
{
  DolphinSwitch::LaunchRequest request;
  bool found_launch_target = false;
  for (int index = 1; index < argc; ++index)
  {
    if (!argv[index] || argv[index][0] == '\0')
      continue;
    const std::string_view argument(argv[index]);
    if ((argument == "--game" || argument == "--exec" || argument == "-g") && index + 1 < argc &&
        argv[index + 1])
    {
      request.path = argv[++index];
      request.nand_title.reset();
      found_launch_target = true;
      continue;
    }
    if (argument == "--nand-title" && index + 1 < argc && argv[index + 1])
    {
      const std::string_view value(argv[++index]);
      std::uint64_t title_id = 0;
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), title_id, 16);
      if (error == std::errc{} && end == value.data() + value.size() && title_id != 0)
      {
        request.path.clear();
        request.nand_title = title_id;
        found_launch_target = true;
      }
      continue;
    }
    if (argument == "--game-config" && index + 1 < argc && argv[index + 1])
    {
      request.game_config_path = argv[++index];
      continue;
    }
    if (argument == "--library-id" && index + 1 < argc && argv[index + 1])
    {
      request.library_id = argv[++index];
      continue;
    }
    if (!found_launch_target && !argument.starts_with('-'))
    {
      request.path = argument;
      found_launch_target = true;
    }
  }
  return found_launch_target ? std::optional{std::move(request)} : std::nullopt;
}

bool SwitchMsgAlertHandler(const char* caption, const char* text, bool yes_no,
                           Common::MsgType style)
{
  const std::string safe_caption = caption && *caption ? caption : "Dolphin";
  const std::string safe_text = text ? text : "Unknown Dolphin error";
  {
    std::lock_guard lock{s_alert_mutex};
    s_pending_alerts.push_back({safe_caption, safe_text});
  }
  // Alerts are presented asynchronously on the host thread.
  return !yes_no && style == Common::MsgType::Information;
}

std::vector<PendingAlert> TakePendingAlerts()
{
  std::lock_guard lock{s_alert_mutex};
  std::vector<PendingAlert> result;
  result.swap(s_pending_alerts);
  return result;
}

std::string CollectAlertText(std::string fallback)
{
  std::string result;
  for (const PendingAlert& alert : TakePendingAlerts())
  {
    if (!result.empty())
      result += "\n\n";
    if (!alert.caption.empty())
      result += alert.caption + ": ";
    result += alert.text;
  }
  return result.empty() ? std::move(fallback) : result;
}

void AppletHook(AppletHookType hook, void*)
{
  switch (hook)
  {
  case AppletHookType_OnFocusState:
  case AppletHookType_OnResume:
  {
    const bool focused = appletGetFocusState() == AppletFocusState_InFocus;
    s_host_focused.store(focused, std::memory_order_release);
    AchievementManager::GetInstance().SetBackgroundExecutionAllowed(focused);
    if (focused)
      DolphinSwitch::Audio::ResumeSharedAudio();
    break;
  }
  case AppletHookType_OnOperationMode:
    s_operation_mode_generation.fetch_add(1, std::memory_order_acq_rel);
    break;
  case AppletHookType_OnExitRequest:
    s_applet_exit_requested.store(true, std::memory_order_release);
    s_session_running.store(false, std::memory_order_release);
    break;
  default:
    break;
  }
}

std::unique_ptr<BootParameters> GenerateBootParameters(const DolphinSwitch::LaunchRequest& request)
{
  BootSessionData session(std::nullopt, DeleteSavestateAfterBoot::No);
  if (request.nand_title)
  {
    return std::make_unique<BootParameters>(
        BootParameters::NANDTitle{static_cast<u64>(*request.nand_title)}, std::move(session));
  }
  if (request.path.empty())
    return nullptr;
  auto boot = BootParameters::GenerateFromFile(request.path, std::move(session), false);
  if (!boot)
    return nullptr;
  const auto* disc = std::get_if<BootParameters::Disc>(&boot->parameters);
  std::string extension;
  SplitPath(request.path, nullptr, nullptr, &extension);
  Common::ToLower(&extension);
  if (!disc || extension == ".json")
    return boot;

  Common::IniFile profile =
      SConfig::LoadLocalGameIni(disc->volume->GetGameID(), disc->volume->GetRevision());
  std::string preset;
  bool enabled = true;
  profile.GetIfExists("Riivolution", "Preset", &preset);
  profile.GetIfExists("Riivolution", "Enabled", &enabled, true);
  if (preset.empty() || !enabled)
    return boot;

  if (!DolphinSwitch::PrepareLaunchStorage(preset))
  {
    PanicAlertFmt("The saved Riivolution preset's storage device is unavailable.");
    return nullptr;
  }
  const auto descriptor = DiscIO::ParseGameModDescriptorFile(preset);
  if (!descriptor || !descriptor->riivolution)
  {
    PanicAlertFmt(
        "The saved Riivolution preset could not be loaded. Check the game's Riivolution settings.");
    return nullptr;
  }
  for (const auto& patch : descriptor->riivolution->patches)
  {
    if (!DolphinSwitch::PrepareLaunchStorage(patch.xml) ||
        !DolphinSwitch::PrepareLaunchStorage(patch.root))
    {
      PanicAlertFmt("The saved Riivolution mod's storage device is unavailable.");
      return nullptr;
    }
  }
  boot->riivolution_patches = DiscIO::Riivolution::GenerateRiivolutionPatchesFromGameModDescriptor(
      *descriptor->riivolution, disc->volume->GetGameID(), disc->volume->GetRevision(),
      disc->volume->GetDiscNumber());
  if (boot->riivolution_patches.empty())
  {
    PanicAlertFmt("No Riivolution patches could be loaded from the saved game profile. Check the "
                  "XML, selected options and game version.");
    return nullptr;
  }
  return boot;
}

}  // namespace

std::vector<std::string> Host_GetPreferredLocales()
{
  return DolphinSwitch::GetSystemPreferredLocales();
}

void Host_PPCSymbolsChanged()
{
}
void Host_PPCBreakpointsChanged()
{
}
bool Host_UIBlocksControllerState()
{
  return !s_host_focused.load(std::memory_order_acquire) ||
         DolphinSwitch::RuntimeOverlay::IsInputCaptured();
}
void Host_UpdateTitle(const std::string& title)
{
}
void Host_UpdateDisasmDialog()
{
}
void Host_JitCacheInvalidation()
{
}
void Host_JitProfileDataWiped()
{
}
void Host_RequestRenderWindowSize(int width, int height)
{
}
bool Host_RendererHasFocus()
{
  return s_host_focused.load(std::memory_order_acquire);
}
bool Host_RendererHasFullFocus()
{
  return s_host_focused.load(std::memory_order_acquire);
}
bool Host_RendererIsFullscreen()
{
  return true;
}
bool Host_TASInputHasFocus()
{
  return false;
}
void Host_YieldToUI()
{
  svcSleepThread(0);
}
void Host_TitleChanged()
{
}
void Host_UpdateDiscordClientID(const std::string& client_id)
{
}

bool Host_UpdateDiscordPresenceRaw(const std::string& details, const std::string& state,
                                   const std::string& large_image_key,
                                   const std::string& large_image_text,
                                   const std::string& small_image_key,
                                   const std::string& small_image_text,
                                   const int64_t start_timestamp, const int64_t end_timestamp,
                                   const int party_size, const int party_max)
{
  return false;
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return DolphinSwitch::GBAOverlay::CreateHost(std::move(core));
}

void Host_Message(HostMessageID id)
{
  if (id == HostMessageID::WMUserStop)
    s_session_running.store(false, std::memory_order_release);
}

namespace
{
struct SessionResult
{
  bool exit_application = false;
  std::string launcher_message;
  bool restart_launcher = false;
};

using RuntimeControllerMode = DolphinSwitch::RuntimeOverlay::ControllerMode;

struct RuntimeControllerSession
{
  bool is_wii = false;
  std::array<SerialInterface::SIDevices, 4> gamecube_devices{};
  std::array<RuntimeControllerMode, 4> modes{};
};

ControllerEmu::Attachments* GetWiimoteAttachments(WiimoteEmu::Wiimote* wiimote)
{
  if (!wiimote)
    return nullptr;
  return static_cast<ControllerEmu::Attachments*>(
      wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Attachments));
}

bool GetBoolSetting(const ControllerEmu::ControlGroup* group, std::string_view name, bool fallback)
{
  if (!group)
    return fallback;
  for (const auto& setting : group->numeric_settings)
  {
    if (setting->GetType() == ControllerEmu::SettingType::Bool &&
        std::string_view(setting->GetININame()) == name)
    {
      return static_cast<const ControllerEmu::NumericSetting<bool>*>(setting.get())->GetValue();
    }
  }
  return fallback;
}

void SetBoolSetting(ControllerEmu::ControlGroup* group, std::string_view name, bool value)
{
  if (!group)
    return;
  for (const auto& setting : group->numeric_settings)
  {
    if (setting->GetType() == ControllerEmu::SettingType::Bool &&
        std::string_view(setting->GetININame()) == name)
    {
      static_cast<ControllerEmu::NumericSetting<bool>*>(setting.get())->SetValue(value);
      return;
    }
  }
}

WiimoteEmu::Wiimote* GetEmulatedWiimote(int player)
{
  InputConfig* const config = Wiimote::GetConfig();
  if (!config || config->ControllersNeedToBeCreated() || player < 0 ||
      player >= config->GetControllerCount())
  {
    return nullptr;
  }
  return static_cast<WiimoteEmu::Wiimote*>(config->GetController(player));
}

void ResetWiiTouchPointerState()
{
  s_wii_touch_pointer_state.store(0, std::memory_order_release);
}

void UpdateWiiTouchPointerState(bool enabled)
{
  constexpr double touchscreen_width = 1280.0;
  constexpr double touchscreen_height = 720.0;
  constexpr std::uint64_t pressed_bit = 1ULL << 63;

  if (!enabled || Host_UIBlocksControllerState() || !g_presenter)
  {
    ResetWiiTouchPointerState();
    return;
  }

  const ciface::Switch::TouchscreenState touch = ciface::Switch::GetTouchscreenState();
  if (!touch.pressed)
  {
    ResetWiiTouchPointerState();
    return;
  }

  const int backbuffer_width = g_presenter->GetBackbufferWidth();
  const int backbuffer_height = g_presenter->GetBackbufferHeight();
  if (backbuffer_width <= 1 || backbuffer_height <= 1)
  {
    ResetWiiTouchPointerState();
    return;
  }

  MathUtil::Rectangle<int> viewport = g_presenter->GetTargetRectangle();
  if (viewport.GetWidth() <= 1 || viewport.GetHeight() <= 1)
    viewport = {0, 0, backbuffer_width, backbuffer_height};

  const int left = std::min(viewport.left, viewport.right);
  const int right = std::max(viewport.left, viewport.right);
  const int top = std::min(viewport.top, viewport.bottom);
  const int bottom = std::max(viewport.top, viewport.bottom);
  const double x_in_backbuffer =
      static_cast<double>(touch.x) * (backbuffer_width - 1) / (touchscreen_width - 1.0);
  const double y_in_backbuffer =
      static_cast<double>(touch.y) * (backbuffer_height - 1) / (touchscreen_height - 1.0);

  if (x_in_backbuffer < left || x_in_backbuffer >= right || y_in_backbuffer < top ||
      y_in_backbuffer >= bottom)
  {
    ResetWiiTouchPointerState();
    return;
  }

  const double x =
      std::clamp((x_in_backbuffer - left) / std::max(1, right - left - 1) * 2.0 - 1.0, -1.0, 1.0);
  const double y =
      std::clamp(1.0 - (y_in_backbuffer - top) / std::max(1, bottom - top - 1) * 2.0, -1.0, 1.0);
  const auto quantize = [](double value) {
    return static_cast<std::uint16_t>(std::lround((value + 1.0) * 32767.5));
  };
  const std::uint64_t packed =
      pressed_bit | quantize(x) | (static_cast<std::uint64_t>(quantize(y)) << 16);
  s_wii_touch_pointer_state.store(packed, std::memory_order_release);
}

bool InstallWiiTouchPointerOverride()
{
  WiimoteEmu::Wiimote* const wiimote = GetEmulatedWiimote(0);
  if (!wiimote)
    return false;

  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
  wiimote->SetInputOverrideFunction([](std::string_view group_name, std::string_view control_name,
                                       ControlState) -> std::optional<ControlState> {
    constexpr std::uint64_t pressed_bit = 1ULL << 63;
    const std::uint64_t packed = s_wii_touch_pointer_state.load(std::memory_order_acquire);
    if ((packed & pressed_bit) == 0 || Host_UIBlocksControllerState() ||
        group_name != WiimoteEmu::Wiimote::IR_GROUP)
    {
      return std::nullopt;
    }

    const auto decode = [](std::uint16_t value) {
      return static_cast<ControlState>(value) / 32767.5 - 1.0;
    };
    if (control_name == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
      return decode(static_cast<std::uint16_t>(packed));
    if (control_name == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
      return decode(static_cast<std::uint16_t>(packed >> 16));
    return std::nullopt;
  });
  return true;
}

void RemoveWiiTouchPointerOverride()
{
  ResetWiiTouchPointerState();
  WiimoteEmu::Wiimote* const wiimote = GetEmulatedWiimote(0);
  if (!wiimote)
    return;

  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
  wiimote->ClearInputOverrideFunction();
}

std::optional<std::string> GetLocalWiimoteSetting(int player, std::string_view key)
{
  const std::shared_ptr<Config::Layer> local_game_layer =
      Config::GetLayer(Config::LayerType::LocalGame);
  if (!local_game_layer)
    return std::nullopt;
  return local_game_layer->Get<std::string>(Config::Location{
      Config::System::WiiPad, "Wiimote" + std::to_string(player + 1), std::string(key)});
}

RuntimeControllerMode DetectRuntimeControllerMode(int player)
{
  if (Config::Get(Config::GetInfoForWiimoteSource(player)) != WiimoteSource::Emulated)
    return RuntimeControllerMode::GameCube;

  WiimoteEmu::Wiimote* const wiimote = GetEmulatedWiimote(player);
  ControllerEmu::Attachments* const attachments = GetWiimoteAttachments(wiimote);
  if (!wiimote || !attachments)
    return RuntimeControllerMode::WiiRemote;

  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();

  WiimoteEmu::ExtensionNumber extension =
      static_cast<WiimoteEmu::ExtensionNumber>(attachments->GetSelectedAttachment());
  if (const std::optional<std::string> configured = GetLocalWiimoteSetting(player, "Extension"))
  {
    if (*configured == "Nunchuk")
      extension = WiimoteEmu::ExtensionNumber::NUNCHUK;
    else if (*configured == "Classic")
      extension = WiimoteEmu::ExtensionNumber::CLASSIC;
    else
      extension = WiimoteEmu::ExtensionNumber::NONE;
  }
  if (extension == WiimoteEmu::ExtensionNumber::NUNCHUK)
    return RuntimeControllerMode::WiiRemoteNunchuk;
  if (extension == WiimoteEmu::ExtensionNumber::CLASSIC)
    return RuntimeControllerMode::ClassicController;

  const auto* const options = wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Options);
  bool sideways = GetBoolSetting(options, WiimoteEmu::Wiimote::SIDEWAYS_OPTION, false);
  if (const std::optional<std::string> configured =
          GetLocalWiimoteSetting(player, "Options/Sideways Wiimote"))
  {
    sideways = *configured == "True" || *configured == "true" || *configured == "1";
  }
  return sideways ? RuntimeControllerMode::WiiRemoteSideways : RuntimeControllerMode::WiiRemote;
}

bool CanSynchronizeRuntimeControllerMode(int player)
{
  if (const std::optional<std::string> configured = GetLocalWiimoteSetting(player, "Extension"))
    return *configured == "None" || *configured == "Nunchuk" || *configured == "Classic";

  WiimoteEmu::Wiimote* const wiimote = GetEmulatedWiimote(player);
  ControllerEmu::Attachments* const attachments = GetWiimoteAttachments(wiimote);
  if (!attachments)
    return false;
  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
  const auto extension =
      static_cast<WiimoteEmu::ExtensionNumber>(attachments->GetSelectedAttachment());
  return extension == WiimoteEmu::ExtensionNumber::NONE ||
         extension == WiimoteEmu::ExtensionNumber::NUNCHUK ||
         extension == WiimoteEmu::ExtensionNumber::CLASSIC;
}

RuntimeControllerSession CreateRuntimeControllerSession(Core::System& system)
{
  RuntimeControllerSession session;
  session.is_wii = system.IsWii();
  for (int player = 0; player < 4; ++player)
  {
    const SerialInterface::SIDevices configured = Config::Get(Config::GetInfoForSIDevice(player));
    session.gamecube_devices[player] = configured == SerialInterface::SIDEVICE_NONE ?
                                           SerialInterface::SIDEVICE_GC_CONTROLLER :
                                           configured;
    session.modes[player] =
        session.is_wii ? DetectRuntimeControllerMode(player) : RuntimeControllerMode::GameCube;
  }
  return session;
}

bool ApplyRuntimeControllerMode(RuntimeControllerSession* session, int player,
                                RuntimeControllerMode mode)
{
  if (!session || !session->is_wii || player < 0 || player >= 4)
    return false;

  WiimoteEmu::ExtensionNumber extension = WiimoteEmu::ExtensionNumber::NONE;
  bool sideways = false;
  switch (mode)
  {
  case RuntimeControllerMode::GameCube:
    break;
  case RuntimeControllerMode::WiiRemote:
    break;
  case RuntimeControllerMode::WiiRemoteSideways:
    sideways = true;
    break;
  case RuntimeControllerMode::WiiRemoteNunchuk:
    extension = WiimoteEmu::ExtensionNumber::NUNCHUK;
    break;
  case RuntimeControllerMode::ClassicController:
    extension = WiimoteEmu::ExtensionNumber::CLASSIC;
    break;
  }

  if (mode != RuntimeControllerMode::GameCube)
  {
    WiimoteEmu::Wiimote* const wiimote = GetEmulatedWiimote(player);
    ControllerEmu::Attachments* const attachments = GetWiimoteAttachments(wiimote);
    if (!wiimote || !attachments)
      return false;

    const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
    attachments->SetSelectedAttachment(extension);
    auto* const options = wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Options);
    SetBoolSetting(options, WiimoteEmu::Wiimote::SIDEWAYS_OPTION, sideways);
    SetBoolSetting(options, WiimoteEmu::Wiimote::UPRIGHT_OPTION, false);
    wiimote->UpdateReferences(g_controller_interface);
  }

  // Changing the Wii Remote source synchronously pauses the CPU and refreshes input devices.
  // Never run those callbacks while holding EmulatedController's state lock: the refresh path can
  // acquire the device mutex in the opposite order and deadlock when switching to GameCube input.
  {
    Config::ConfigChangeCallbackGuard config_guard;
    Config::SetCurrent(Config::GetInfoForSIDevice(player), mode == RuntimeControllerMode::GameCube ?
                                                               session->gamecube_devices[player] :
                                                               SerialInterface::SIDEVICE_NONE);
    Config::SetCurrent(Config::GetInfoForWiimoteSource(player),
                       mode == RuntimeControllerMode::GameCube ? WiimoteSource::None :
                                                                 WiimoteSource::Emulated);
  }

  session->modes[player] = mode;
  return true;
}

void UpdateSessionPauseState(Core::System& system, bool focused, bool user_paused,
                             bool overlay_needs_render, bool* lifecycle_owned_pause)
{
  const Core::State state = Core::GetState(system);
  if (!focused)
  {
    if (state == Core::State::Running)
    {
      Core::SetState(system, Core::State::Paused, true, true);
      *lifecycle_owned_pause = true;
    }
    return;
  }

  if (*lifecycle_owned_pause)
  {
    *lifecycle_owned_pause = false;
    if (!user_paused || overlay_needs_render)
    {
      Core::SetState(system, Core::State::Running);
    }
  }
}

void RunWithLoadingScreen(const std::function<void()>& task)
{
  Common::HorizonClocks::ScopedCpuBoost boost;
  consoleInit(nullptr);
  std::printf("Loading Riivolution...\n\n");
  consoleUpdate(nullptr);

  std::atomic_bool complete{false};
  struct Task
  {
    const std::function<void()>& run;
    std::atomic_bool& complete;
  } context{task, complete};
  Thread worker{};
  const Result created = threadCreate(
      &worker,
      [](void* userdata) {
        auto& work = *static_cast<Task*>(userdata);
        work.run();
        work.complete.store(true, std::memory_order_release);
      },
      &context, nullptr, 2 * 1024 * 1024, 0x2C, -2);
  const bool worker_created = R_SUCCEEDED(created);
  const bool worker_started = worker_created && R_SUCCEEDED(threadStart(&worker));
  if (!worker_started)
  {
    task();
    complete.store(true, std::memory_order_release);
  }
  unsigned frames = 0;
  while (!complete.load(std::memory_order_acquire))
  {
    if (!appletMainLoop())
      s_applet_exit_requested.store(true, std::memory_order_release);
    std::printf("\rPreparing mod files %c", "|/-\\"[(frames++ / 15) % 4]);
    consoleUpdate(nullptr);
    svcSleepThread(16'000'000);
  }
  if (worker_started)
    threadWaitForExit(&worker);
  if (worker_created)
    threadClose(&worker);
  consoleExit(nullptr);
}

SessionResult RunGameSession(const DolphinSwitch::LaunchRequest& request,
                             const std::string& launcher_path)
{
  Core::System& system = Core::System::GetInstance();
  DolphinSwitch::LaunchRequest resolved_request = request;

  if (!resolved_request.nand_title)
  {
    const bool path_ready =
        request.library_id.empty() ?
            DolphinSwitch::PrepareLaunchStorage(request.path, &resolved_request.path) :
            DolphinSwitch::ResolveLibraryLaunchPath(request.library_id, request.path,
                                                    &resolved_request.path);
    if (!path_ready)
      return {false, CollectAlertText("The selected game's storage device is unavailable.")};
  }

  ConfigLoaders::SetLocalGameConfigOverridePath(resolved_request.game_config_path);
  Common::ScopeGuard game_config_guard([] { ConfigLoaders::SetLocalGameConfigOverridePath({}); });
  std::string extension;
  SplitPath(resolved_request.path, nullptr, nullptr, &extension);
  Common::ToLower(&extension);
  if (extension == ".json")
  {
    const auto descriptor = DiscIO::ParseGameModDescriptorFile(resolved_request.path);
    if (descriptor)
    {
      std::vector<std::string> dependencies{descriptor->base_file};
      if (descriptor->riivolution)
      {
        for (const auto& patch : descriptor->riivolution->patches)
        {
          dependencies.push_back(patch.xml);
          dependencies.push_back(patch.root);
        }
      }
      for (const auto& dependency : dependencies)
      {
        if (!DolphinSwitch::PrepareLaunchStorage(dependency))
        {
          return {false, "The mod preset's storage device is unavailable."};
        }
      }
    }
  }

  std::unique_ptr<BootParameters> boot = GenerateBootParameters(resolved_request);
  if (!boot)
  {
    return {false,
            CollectAlertText("Dolphin could not create boot parameters for the selected title.")};
  }
  for (const auto& patch : boot->riivolution_patches)
  {
    for (const auto& memory : patch.m_memory_patches)
    {
      if (memory.m_valuefile.empty())
        continue;
      const auto size = patch.m_file_data_loader->GetExternalFileSize(memory.m_valuefile);
      if (!size || *size == 0)
      {
        return {false, "A required Riivolution mod file is missing or empty. Check the SD root and "
                       "mod files."};
      }
    }
  }

  if (s_applet_exit_requested.load(std::memory_order_acquire))
    return {false, {}};

  NWindow* const window = nwindowGetDefault();
  const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
  const u32 width = docked ? 1920 : 1280;
  const u32 height = docked ? 1080 : 720;
  if (!ConfigureDefaultNWindow(window, width, height))
    return {false, "The display could not be configured after loading the game."};
  const WindowSystemInfo wsi{WindowSystemType::Switch, nullptr, window, window};
  UICommon::InitControllers(wsi);
  Common::ScopeGuard controller_guard([] { UICommon::ShutdownControllers(); });

  std::atomic_bool ever_running{false};
  s_session_running.store(true, std::memory_order_release);
  auto state_hook = Core::AddOnStateChangedCallback([&ever_running](Core::State state) {
    if (state == Core::State::Running)
      ever_running.store(true, std::memory_order_release);
    if (state == Core::State::Uninitialized)
      s_session_running.store(false, std::memory_order_release);
  });

  // The launcher uses the same SDL device for UI sounds, but handing that long-lived device to
  // Dolphin can leave Horizon's audio output stopped even though SDL still reports a valid handle.
  // Give every game a fresh device, matching direct launches and avoiding a full application
  // restart when the launcher device has become stale.
  DolphinSwitch::Audio::ResetSharedAudioDevice();

  ScopedEnvironmentOverrides graphics_environment;
  graphics_environment.Set("MESA_SWITCH_GLTHREAD", "0");
  graphics_environment.Set("MESA_SWITCH_GL_DRIVER", "nvc0");

  DolphinSwitch::GciSyncSession gci_sync;
  std::string skylander_warning;
  Common::ScopeGuard skylander_guard([] { DolphinSwitch::Skylanders::EndSession(); });
  std::string gci_prepare_error;
  bool deko_handoff = false;
  std::string handoff_error;
  std::optional<Common::HorizonClocks::ScopedCpuBoost> boot_boost;
  std::optional<std::uint64_t> frames_when_running;
  BootParameters* const pending_boot = boot.get();
  const auto prepare_session = [&] {
    boot_boost.emplace();
#ifdef DOLPHIN_SWITCH_DEKO3D_RUNTIME
    Config::SetCurrent(Config::MAIN_GFX_BACKEND, std::string("Deko3D"));
    if (Config::Get(Config::GFX_LSFG_ENABLED) &&
        Deko3D::FrameGeneration::GetShaderStatus() ==
            Deko3D::FrameGeneration::ShaderStatus::NotPrepared)
    {
      consoleInit(nullptr);
      std::printf("Loading Deko3D LSFG...\n\n");
      consoleUpdate(nullptr);
      handoff_error = Deko3D::FrameGeneration::PrepareShaders([](u32 compiled, u32 total) {
        std::printf("\r%u%%", total ? compiled * 100 / total : 0);
        consoleUpdate(nullptr);
        if (!appletMainLoop())
          s_applet_exit_requested.store(true, std::memory_order_release);
      });
      consoleExit(nullptr);
      if (!handoff_error.empty() || s_applet_exit_requested.load(std::memory_order_acquire))
        return false;
      if (!ConfigureDefaultNWindow(window, width, height))
      {
        handoff_error = "Deko3D could not restore the display after loading LSFG.";
        return false;
      }
    }
#else
    if (Config::Get(Config::MAIN_GFX_BACKEND) == "Deko3D")
    {
      Config::Save();
      deko_handoff =
          DolphinSwitch::LaunchEmbeddedRuntime(launcher_path, resolved_request, &handoff_error);
      return false;
    }
#endif
    if (!pending_boot->riivolution_patches.empty())
    {
      RunWithLoadingScreen([&] {
        AddRiivolutionPatches(pending_boot, std::move(pending_boot->riivolution_patches));
      });
      const auto* disc = std::get_if<BootParameters::Disc>(&pending_boot->parameters);
      if (!disc || !disc->volume || !ConfigureDefaultNWindow(window, width, height))
      {
        handoff_error = "Could not prepare the Riivolution-patched game.";
        return false;
      }
      if (s_applet_exit_requested.load(std::memory_order_acquire))
        return false;
    }
    if (!gci_sync.Prepare(system, &gci_prepare_error))
      return false;
    skylander_warning = DolphinSwitch::Skylanders::BeginSession();
    return true;
  };
  if (!BootManager::BootCore(system, std::move(boot), wsi, prepare_session))
  {
    if (deko_handoff)
    {
      s_session_running.store(false, std::memory_order_release);
      return {true, {}};
    }
    const bool attempted_vulkan = Config::Get(Config::MAIN_GFX_BACKEND) == "Vulkan";
    std::fprintf(stderr, "[Dolphin Switch] BootCore failed\n");
    s_session_running.store(false, std::memory_order_release);
    std::string cleanup_warning;
    gci_sync.Finish(&cleanup_warning);
    std::string message = !handoff_error.empty() ?
                              std::move(handoff_error) :
                          gci_prepare_error.empty() ?
                              CollectAlertText("Dolphin failed to initialize the selected title.") :
                              std::move(gci_prepare_error);
    if (!cleanup_warning.empty())
    {
      if (!message.empty())
        message += "\n\n";
      message += cleanup_warning;
    }
    return {false, std::move(message), attempted_vulkan};
  }
  const std::string configured_backend = Config::Get(Config::MAIN_GFX_BACKEND);

  RuntimeControllerSession runtime_controllers = CreateRuntimeControllerSession(system);
  if (runtime_controllers.is_wii)
  {
    // Loading a controller profile changes the selected attachment, but an already initialized
    // emulated Wii Remote can retain its previous live extension until it is explicitly applied.
    // Synchronize supported extensions at session startup, matching what the runtime menu does
    // when the user re-selects the already highlighted controller mode.
    for (int player = 0; player < 4; ++player)
    {
      if (runtime_controllers.modes[player] != RuntimeControllerMode::GameCube &&
          CanSynchronizeRuntimeControllerMode(player))
      {
        ApplyRuntimeControllerMode(&runtime_controllers, player, runtime_controllers.modes[player]);
      }
    }
  }
  DolphinSwitch::RuntimeOverlay::BeginSession(resolved_request.path, resolved_request.game_id,
                                              resolved_request.revision, runtime_controllers.is_wii,
                                              runtime_controllers.modes);
  Common::ScopeGuard overlay_guard([] { DolphinSwitch::RuntimeOverlay::EndSession(); });
  if (!skylander_warning.empty())
    DolphinSwitch::RuntimeOverlay::SetStatus(skylander_warning);

  const bool touch_pointer_enabled =
      runtime_controllers.is_wii && Config::Get(Config::MAIN_SWITCH_TOUCHSCREEN_POINTER);
  const bool touch_pointer_installed = touch_pointer_enabled && InstallWiiTouchPointerOverride();
  Common::ScopeGuard touch_pointer_guard([touch_pointer_installed] {
    if (touch_pointer_installed)
      RemoveWiiTouchPointerOverride();
  });

  PadState runtime_pad{};
  const Result hid_result = hidInitialize();
  const bool host_hid_initialized = R_SUCCEEDED(hid_result);
  if (host_hid_initialized)
    padInitialize(&runtime_pad, HidNpadIdType_No1, HidNpadIdType_Handheld);
  Common::ScopeGuard hid_guard([&] {
    if (host_hid_initialized)
      hidExit();
  });

  (void)appletSetMediaPlaybackState(true);
  Common::ScopeGuard playback_guard([] { (void)appletSetMediaPlaybackState(false); });

  bool applet_alive = true;
  bool user_paused = false;
  bool lifecycle_owned_pause = false;
  bool previous_overlay_visible = false;
  std::optional<std::uint64_t> pause_after_render_generation;
  std::uint64_t handled_operation_mode_generation =
      s_operation_mode_generation.load(std::memory_order_acquire);
  while (s_session_running.load(std::memory_order_acquire) && (applet_alive = appletMainLoop()))
  {
    Core::HostDispatchJobs(system);

    if (boot_boost && ever_running.load(std::memory_order_acquire))
    {
      const std::uint64_t frames = DolphinSwitch::RuntimeOverlay::GetRenderFrameGeneration();
      if (!frames_when_running)
        frames_when_running = frames;
      else if (frames != *frames_when_running)
      {
        boot_boost.reset();
      }
    }

    for (PendingAlert& alert : TakePendingAlerts())
      DolphinSwitch::RuntimeOverlay::ShowAlert(std::move(alert.caption), std::move(alert.text));

    if (host_hid_initialized)
    {
      ciface::Switch::UpdatePadState(&runtime_pad);
      const auto left = padGetStickPos(&runtime_pad, 0);
      const auto right = padGetStickPos(&runtime_pad, 1);
      const auto axis = [](s32 value) {
        const float normalized = std::clamp(value / 32768.0f, -1.0f, 1.0f);
        return std::abs(normalized) < 0.18f ?
                   0.0f :
                   std::copysign((std::abs(normalized) - 0.18f) / 0.82f, normalized);
      };
      DolphinSwitch::RuntimeOverlay::UpdateInput(
          padGetButtonsDown(&runtime_pad), padGetButtons(&runtime_pad),
          (runtime_pad.style_set & HidNpadStyleTag_NpadGc) != 0, axis(left.x), -axis(left.y),
          axis(right.x), -axis(right.y));
    }
    UpdateWiiTouchPointerState(touch_pointer_installed);

    const bool overlay_visible = DolphinSwitch::RuntimeOverlay::IsVisible();
    if (overlay_visible && !previous_overlay_visible &&
        Core::GetState(system) == Core::State::Paused &&
        s_host_focused.load(std::memory_order_acquire))
    {
      // The overlay needs presented frames while a paused title is open.
      user_paused = true;
      Core::SetState(system, Core::State::Running);
    }

    bool stop_to_launcher = false;
    for (DolphinSwitch::RuntimeOverlay::Action& action :
         DolphinSwitch::RuntimeOverlay::TakeActions())
    {
      switch (action.type)
      {
      case DolphinSwitch::RuntimeOverlay::ActionType::StopToLauncher:
        stop_to_launcher = true;
        s_session_running.store(false, std::memory_order_release);
        break;
      case DolphinSwitch::RuntimeOverlay::ActionType::SaveState:
        if (!File::CreateFullPath(File::GetUserPath(D_STATESAVES_IDX)))
        {
          DolphinSwitch::RuntimeOverlay::SetStatus("Could not create the StateSaves folder");
        }
        else
        {
          const std::uint64_t previous_timestamp = State::GetUnixTimeOfSlot(action.value);
          State::Save(system, action.value);
          DolphinSwitch::RuntimeOverlay::TrackStateSave(action.value, previous_timestamp);
        }
        break;
      case DolphinSwitch::RuntimeOverlay::ActionType::LoadState:
        if (State::GetUnixTimeOfSlot(action.value) == 0)
        {
          DolphinSwitch::RuntimeOverlay::SetStatus("State slot " + std::to_string(action.value) +
                                                   " is empty");
        }
        else
        {
          // Register completion before the CPU thread can service the load.
          DolphinSwitch::RuntimeOverlay::TrackStateLoad(action.value);
          State::Load(system, action.value);
        }
        break;
      case DolphinSwitch::RuntimeOverlay::ActionType::Reset:
        if (system.GetMovie().IsRecordingInput())
          system.GetMovie().SetReset(true);
        system.GetProcessorInterface().ResetButton_Tap();
        break;
      case DolphinSwitch::RuntimeOverlay::ActionType::ResetGBA:
        Pad::SetGBAReset(action.value, true);
        break;
      case DolphinSwitch::RuntimeOverlay::ActionType::EjectDisc:
        system.GetDVDInterface().EjectDisc(Core::CPUThreadGuard{system}, DVD::EjectCause::User);
        break;
      case DolphinSwitch::RuntimeOverlay::ActionType::ChangeDisc:
        if (!DolphinSwitch::PrepareLaunchStorage(action.path))
        {
          DolphinSwitch::RuntimeOverlay::ShowAlert(
              "Change Disc", "The selected disc's storage device is no longer available.");
        }
        else
        {
          system.GetDVDInterface().ChangeDisc(Core::CPUThreadGuard{system}, action.path);
        }
        break;
      case DolphinSwitch::RuntimeOverlay::ActionType::ToggleCheats:
        DolphinSwitch::RuntimeOverlay::ToggleCheatsForSession();
        break;
      case DolphinSwitch::RuntimeOverlay::ActionType::ToggleCheat:
        DolphinSwitch::RuntimeOverlay::ToggleCheatForSession(action.value);
        break;
      case DolphinSwitch::RuntimeOverlay::ActionType::ToggleFrameGeneration:
        DolphinSwitch::RuntimeOverlay::ToggleFrameGenerationForSession();
        break;
      case DolphinSwitch::RuntimeOverlay::ActionType::ToggleFPS:
        DolphinSwitch::RuntimeOverlay::ToggleFPSForSession();
        break;
      case DolphinSwitch::RuntimeOverlay::ActionType::ToggleVBISkip:
        DolphinSwitch::RuntimeOverlay::ToggleVBISkipForSession();
        break;
      case DolphinSwitch::RuntimeOverlay::ActionType::ToggleSkylandersPortal:
      {
        const bool enable = !Config::Get(Config::MAIN_EMULATE_SKYLANDER_PORTAL);
        const auto error = DolphinSwitch::Skylanders::SetEnabled(enable);
        DolphinSwitch::RuntimeOverlay::SetStatus(
            error.empty() ? (enable ? "Skylanders portal enabled" : "Skylanders portal disabled") :
                            error);
        break;
      }
      case DolphinSwitch::RuntimeOverlay::ActionType::ListSkylanderFigures:
      {
        std::string error;
        auto figures = DolphinSwitch::Skylanders::ListCollection(&error);
        DolphinSwitch::RuntimeOverlay::SetSkylanderFigures(std::move(figures), std::move(error));
        break;
      }
      case DolphinSwitch::RuntimeOverlay::ActionType::LoadSkylanderFigure:
      {
        const auto error = DolphinSwitch::Skylanders::SetFigure(action.value, action.path);
        DolphinSwitch::RuntimeOverlay::SetStatus(error.empty() ? "Figure placed on portal" : error);
        break;
      }
      case DolphinSwitch::RuntimeOverlay::ActionType::RemoveSkylanderFigure:
      {
        const auto error = DolphinSwitch::Skylanders::ClearFigure(action.value);
        DolphinSwitch::RuntimeOverlay::SetStatus(error.empty() ? "Figure removed from portal" :
                                                                 error);
        break;
      }
      case DolphinSwitch::RuntimeOverlay::ActionType::SetControllerMode:
      {
        const auto mode = static_cast<RuntimeControllerMode>(action.value2);
        const bool valid_mode =
            action.value2 >= static_cast<int>(RuntimeControllerMode::GameCube) &&
            action.value2 <= static_cast<int>(RuntimeControllerMode::ClassicController);
        const bool success =
            valid_mode && ApplyRuntimeControllerMode(&runtime_controllers, action.value, mode);
        DolphinSwitch::RuntimeOverlay::SetControllerModeResult(action.value, mode, success);
        break;
      }
      }
    }

    const bool now_overlay_visible = DolphinSwitch::RuntimeOverlay::IsVisible();
    if (now_overlay_visible)
    {
      pause_after_render_generation.reset();
    }
    else if (previous_overlay_visible && user_paused)
    {
      pause_after_render_generation = DolphinSwitch::RuntimeOverlay::GetRenderFrameGeneration() + 1;
    }
    previous_overlay_visible = now_overlay_visible;

    UpdateSessionPauseState(system, s_host_focused.load(std::memory_order_acquire), user_paused,
                            now_overlay_visible || pause_after_render_generation.has_value(),
                            &lifecycle_owned_pause);

    if (pause_after_render_generation && s_host_focused.load(std::memory_order_acquire) &&
        DolphinSwitch::RuntimeOverlay::GetRenderFrameGeneration() >= *pause_after_render_generation)
    {
      if (Core::GetState(system) == Core::State::Running)
        Core::SetState(system, Core::State::Paused);
      pause_after_render_generation.reset();
    }

    const std::uint64_t operation_mode_generation =
        s_operation_mode_generation.load(std::memory_order_acquire);
    if (operation_mode_generation != handled_operation_mode_generation)
    {
      handled_operation_mode_generation = operation_mode_generation;
#ifdef HAS_DEKO3D
      const bool now_docked = appletGetOperationMode() == AppletOperationMode_Console;
      const u32 new_width = now_docked ? 1920 : 1280;
      const u32 new_height = now_docked ? 1080 : 720;
      (void)nwindowSetDimensions(window, new_width, new_height);
      (void)nwindowSetCrop(window, 0, 0, new_width, new_height);
#endif
      if (g_presenter)
      {
        g_presenter->ResizeSurface();
      }
    }

    if (stop_to_launcher)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }

  if (!applet_alive)
  {
    s_applet_exit_requested.store(true, std::memory_order_release);
  }

  DolphinSwitch::RuntimeOverlay::EndSession();
  overlay_guard.Dismiss();
  boot_boost.reset();
  Core::Stop(system);
  Core::Shutdown(system);

  std::string error_message = CollectAlertText({});
  std::string gci_sync_warning;
  gci_sync.Finish(&gci_sync_warning);
  if (!gci_sync_warning.empty())
  {
    if (!error_message.empty())
      error_message += "\n\n";
    error_message += gci_sync_warning;
  }
  if (!ever_running.load(std::memory_order_acquire) && error_message.empty())
    error_message = "The selected title stopped during boot.";

  return {s_applet_exit_requested.load(std::memory_order_acquire), std::move(error_message),
          configured_backend == "Vulkan"};
}
}  // namespace

int main(int argc, char** argv)
{
  Common::HorizonFastmem::ReserveArenaAddressSpace();

  saltynx_start();
  Common::ScopeGuard saltynx_guard([] { saltynx_stop(); });
  Common::ScopeGuard audio_guard([] { DolphinSwitch::Audio::ShutdownSharedAudio(); });

  const std::string_view self_path =
      argc > 0 && argv[0] ? std::string_view(argv[0]) : std::string_view{};

  u64 allowed_core_mask = 0;
  const Result core_mask_result =
      svcGetInfo(&allowed_core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);

  // Keep the host on core 2; emulation uses cores 0 and 1, leaving core 3 to Horizon.
  if (R_FAILED(core_mask_result) || (allowed_core_mask & (1ULL << 2)) != 0)
  {
    Common::SetCurrentThreadName("Dolphin host");
    Common::SetCurrentThreadAffinity(2);
  }

#ifdef DOLPHIN_SWITCH_DEKO3D_RUNTIME
  bool has_runtime_argument = false;
  for (int index = 1; index < argc; ++index)
  {
    has_runtime_argument |=
        argv[index] && argv[index] == DolphinSwitch::EmbeddedRuntime::LAUNCH_ARGUMENT;
  }
  const auto runtime_request =
      has_runtime_argument ? DolphinSwitch::EmbeddedRuntime::ReadRequest() : std::nullopt;
  if (!runtime_request)
  {
    (void)DolphinSwitch::EmbeddedRuntime::ReturnToLauncher(
        {}, "The Deko3D runtime must be launched from Dolphin.");
    return EXIT_FAILURE;
  }
  const std::string launcher_path = runtime_request->launcher_path;
#else
  const std::string launcher_path = DolphinSwitch::Updater::ResolveLauncherPath(self_path);
#endif
  std::string update_recovery_error;
#ifdef DOLPHIN_SWITCH_DEKO3D_RUNTIME
  const bool update_recovery_ok = true;
#else
  const bool update_recovery_ok =
      DolphinSwitch::Updater::RecoverInstallation(launcher_path, update_recovery_error);
#endif

  const Result romfs_result = romfsInit();
  bool romfs_mounted = R_SUCCEEDED(romfs_result);
  bool sockets_initialized = false;
  Common::ScopeGuard platform_guard([&] {
    DolphinSwitch::ShutdownLauncherStorage();
    if (sockets_initialized)
      socketExit();
    if (romfs_mounted)
      romfsExit();
  });
  if (!romfs_mounted)
  {
#ifdef DOLPHIN_SWITCH_DEKO3D_RUNTIME
    (void)DolphinSwitch::EmbeddedRuntime::ReturnToLauncher(
        launcher_path, "The Deko3D runtime's system data could not be mounted.");
#endif
    return EXIT_FAILURE;
  }

  File::SetSysDirectory("romfs:");
  UICommon::SetUserDirectory("sdmc:/switch/dolphin");
  (void)File::CreateDirs(File::GetUserPath(D_CONFIG_IDX));
  // The SDL launcher must remain single-threaded. A game can temporarily
  // override these defaults through its graphics settings.
  setenv("MESA_SWITCH_GLTHREAD", "0", 1);
  setenv("MESA_SWITCH_GL_DRIVER", "nvc0", 1);
  if (!self_path.empty())
    DolphinSwitch::Forwarder::SetSelfPath(launcher_path);

  if (IsAppletMode())
  {
#ifdef DOLPHIN_SWITCH_DEKO3D_RUNTIME
    (void)DolphinSwitch::EmbeddedRuntime::ReturnToLauncher(
        launcher_path, "Deko3D requires launching Dolphin with full application memory.");
    return EXIT_FAILURE;
#else
    return DolphinSwitch::RunAppletInstaller() ? EXIT_SUCCESS : EXIT_FAILURE;
#endif
  }

  sockets_initialized = R_SUCCEEDED(socketInitializeDefault());
  UICommon::Init();
  Common::ScopeGuard ui_common_guard([] { UICommon::Shutdown(); });
  if (Config::Get(Config::RA_ENABLED))
    AchievementManager::GetInstance().Init(nullptr);
  Common::ScopeGuard achievement_guard([] { AchievementManager::GetInstance().Shutdown(); });
  if (DolphinSwitch::ApplyAutoLanguageDefaults())
    Config::Save();
  (void)File::CreateFullPath(File::GetUserPath(D_CACHE_IDX));
  (void)File::CreateFullPath(File::GetUserPath(D_SHADERCACHE_IDX));

  Common::RegisterMsgAlertHandler(SwitchMsgAlertHandler);
  {
    NWindow* const launcher_window = nwindowGetDefault();
    const WindowSystemInfo launcher_wsi{WindowSystemType::Switch, nullptr, launcher_window,
                                        launcher_window};
    VideoBackendBase::PopulateBackendInfo(launcher_wsi);
  }
  s_host_focused.store(appletGetFocusState() == AppletFocusState_InFocus,
                       std::memory_order_release);
  (void)appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
  AppletHookCookie applet_hook_cookie{};
  appletHook(&applet_hook_cookie, AppletHook, nullptr);
  Common::ScopeGuard applet_hook_guard([&] { appletUnhook(&applet_hook_cookie); });

  DolphinSwitch::RuntimeOverlay::InitializeRendererHook();
  Common::ScopeGuard overlay_renderer_guard(
      [] { DolphinSwitch::RuntimeOverlay::ShutdownRendererHook(); });

#ifdef DOLPHIN_SWITCH_DEKO3D_RUNTIME
  std::optional<DolphinSwitch::LaunchRequest> direct_request = runtime_request->game;
#else
  std::optional<DolphinSwitch::LaunchRequest> direct_request = GetDirectLaunchRequest(argc, argv);
#endif
  std::string launcher_message =
      update_recovery_ok ? std::string{} : "Update recovery failed: " + update_recovery_error;
  for (int index = 1; index < argc; ++index)
  {
    if (argv[index] && argv[index] == DolphinSwitch::EmbeddedRuntime::RETURN_ARGUMENT)
      launcher_message = DolphinSwitch::EmbeddedRuntime::ReadReturnMessage();
  }
  while (!s_applet_exit_requested.load(std::memory_order_acquire))
  {
    std::optional<DolphinSwitch::LaunchRequest> request;
    if (direct_request)
    {
      request = std::move(direct_request);
      direct_request.reset();
    }
    else
    {
#ifdef DOLPHIN_SWITCH_DEKO3D_RUNTIME
      break;
#else
      request = DolphinSwitch::RunLauncher(std::move(launcher_message), launcher_path);
      launcher_message.clear();
#endif
    }

    if (!request)
    {
      if (DolphinSwitch::Updater::ConsumeInstallationRequest())
      {
        DolphinSwitch::ShutdownLauncherStorage();
        if (romfs_mounted)
        {
          romfsExit();
          romfs_mounted = false;
        }
        if (DolphinSwitch::Updater::InstallDownloaded(launcher_path))
        {
          const DolphinSwitch::Updater::Snapshot installed = DolphinSwitch::Updater::GetSnapshot();
          (void)DolphinSwitch::RecordInstalledReleaseTag(installed.release.tag);
          break;
        }

        const DolphinSwitch::Updater::Snapshot snapshot = DolphinSwitch::Updater::GetSnapshot();
        launcher_message =
            "Update installation failed: " +
            (snapshot.error.empty() ? std::string("Unknown error.") : snapshot.error);
        romfs_mounted = R_SUCCEEDED(romfsInit());
        if (romfs_mounted)
          continue;
        return EXIT_FAILURE;
      }
      break;
    }

    const SessionResult result = RunGameSession(*request, launcher_path);
    if (result.exit_application)
      break;
#ifdef DOLPHIN_SWITCH_DEKO3D_RUNTIME
    (void)DolphinSwitch::EmbeddedRuntime::ReturnToLauncher(launcher_path, result.launcher_message);
    break;
#endif
    if (result.restart_launcher)
    {
      (void)ScheduleLauncherRestart(launcher_path);
      break;
    }
    launcher_message = result.launcher_message;
  }

  return EXIT_SUCCESS;
}
