// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/RuntimeOverlay.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <imgui.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Thread.h"
#include "Core/AchievementManager.h"
#include "Core/ActionReplay.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/GeckoCode.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/State.h"
#include "DolphinSwitch/FrameGeneration.h"
#include "DolphinSwitch/GBAOverlay.h"
#include "DolphinSwitch/ScreenLayout.h"
#include "InputCommon/ControllerInterface/Switch/Switch.h"
#include "VideoCommon/OnScreenUI.h"
#include "VideoCommon/Present.h"

namespace DolphinSwitch::RuntimeOverlay
{
namespace
{
enum class Page
{
  Main,
  SaveStates,
  LoadStates,
  Disc,
  DiscBrowser,
  Cheats,
  Achievements,
  Controllers,
  ControllerModes,
  GBA,
  ScreenLayout,
  Skylanders,
  SkylanderFigures,
  Alert,
};

struct AchievementEntry
{
  std::string title;
  std::string description;
  std::string progress;
  std::uint32_t points = 0;
  bool unlocked = false;
};

struct BrowserEntry
{
  std::string name;
  std::string path;
  bool directory = false;
};

struct BrowserScanRequest
{
  std::string directory;
  std::uint64_t generation = 0;
};

struct BrowserScanResult
{
  std::string directory;
  std::vector<BrowserEntry> entries;
  std::string error;
  bool cancelled = false;
};

struct StateSaveMonitorRequest
{
  int slot = 0;
  std::uint64_t previous_timestamp = 0;
  std::uint64_t generation = 0;
};

constexpr std::uint64_t MENU_CHORD =
    HidNpadButton_L | HidNpadButton_R | HidNpadButton_Plus;
// The main page's rows, in the order they are drawn.
enum MainItem : int
{
  MAIN_SAVE_STATE,
  MAIN_LOAD_STATE,
  MAIN_DISC,
  MAIN_CHEATS,
  MAIN_ACHIEVEMENTS,
  MAIN_CONTROLLERS,
  MAIN_SCREEN_LAYOUT,
  MAIN_FRAME_GENERATION,
  MAIN_FPS,
  MAIN_VBI_SKIP,
  MAIN_CPU_BOOST,
  MAIN_SKYLANDERS,
  MAIN_RESET,
  MAIN_STOP,
  // Only while a Game Boy Advance is connected.
  MAIN_GBA,
};
constexpr int MAIN_ITEM_COUNT = MAIN_GBA;
constexpr int DISC_ITEM_COUNT = 3;
constexpr int CONTROLLER_ITEM_COUNT = 6;
constexpr int CONTROLLER_CALIBRATE_ITEM = 4;
constexpr int CONTROLLER_MODE_ITEM_COUNT = 6;

std::mutex s_mutex;
std::atomic_bool s_visible{false};
std::atomic_bool s_capture_until_release{false};
std::atomic<std::uint64_t> s_render_frame_generation{0};
bool s_initialized = false;
bool s_show_fps = false;
bool s_vbi_skip = false;
#ifdef __SWITCH2__
bool s_cpu_boost = false;
#endif
bool s_is_wii = false;
// The quick menu's scale for the frame being drawn (OverlayScale): row and list heights follow it.
float s_ui_scale = 1.0f;
// The room the status line takes at the bottom of the menu.
float s_footer_height = 0.0f;
Page s_page = Page::Main;
int s_selection = 0;
int s_controller_player = 0;
std::array<ControllerMode, 4> s_controller_modes{};
std::array<bool, 4> s_gba_ports{};
std::deque<Action> s_actions;
std::string s_game_path;
std::string s_game_id;
unsigned s_revision = 0;
std::array<std::string, State::NUM_STATES> s_state_info;
std::vector<ActionReplay::ARCode> s_ar_codes;
std::vector<Gecko::GeckoCode> s_gecko_codes;
bool s_cheats_enabled = false;
std::vector<AchievementEntry> s_achievements;
std::string s_achievement_summary;
std::string s_achievement_presence;
std::chrono::steady_clock::time_point s_achievement_refresh_at{};
std::string s_browser_directory;
std::vector<BrowserEntry> s_browser_entries;
std::string s_browser_error;
bool s_browser_scanning = false;
int s_skylander_slot = 0;
std::vector<Skylanders::FigureFile> s_skylander_figures;
std::string s_skylander_error;
bool s_skylander_scanning = false;
std::string s_status;
std::chrono::steady_clock::time_point s_status_until{};
bool s_calibration_active = false;
float s_calibration_x = 0.0f;
float s_calibration_y = 0.0f;
std::string s_calibration_text;
std::string s_banner;
std::chrono::steady_clock::time_point s_banner_until{};
std::string s_alert_caption;
std::string s_alert_message;
int s_pending_load_slot = 0;

std::mutex s_browser_worker_mutex;
std::condition_variable s_browser_worker_cv;
std::optional<BrowserScanRequest> s_pending_browser_scan;
std::thread s_browser_worker;
bool s_browser_worker_shutdown = false;
std::atomic<std::uint64_t> s_browser_scan_generation{0};

std::mutex s_state_worker_mutex;
std::condition_variable s_state_worker_cv;
std::optional<StateSaveMonitorRequest> s_pending_state_save_monitor;
std::thread s_state_worker;
bool s_state_worker_shutdown = false;
std::atomic<std::uint64_t> s_state_worker_generation{0};

std::string Lower(std::string value)
{
  std::ranges::transform(value, value.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool IsDiscImage(const std::filesystem::path& path)
{
  const std::string filename = Lower(path.filename().string());
  if (filename == "hif_000000.nfs")
    return true;
  const std::string extension = Lower(path.extension().string());
  constexpr std::array<std::string_view, 8> extensions = {
      ".iso", ".gcm", ".tgc", ".wbfs", ".ciso", ".gcz", ".wia", ".rvz"};
  return std::ranges::find(extensions, extension) != extensions.end();
}

void QueueAction(Action action)
{
  s_actions.emplace_back(std::move(action));
}

void SetPage(Page page)
{
  if (s_page == Page::ScreenLayout)
    ScreenLayout::CancelEdit();
  s_page = page;
  s_selection = 0;
}

std::string_view ControllerModeName(ControllerMode mode)
{
  switch (mode)
  {
  case ControllerMode::GameCube:
    return "GameCube Controller";
  case ControllerMode::WiiRemote:
    return "Wii Remote";
  case ControllerMode::WiiRemoteSideways:
    return "Wii Remote Sideways";
  case ControllerMode::WiiRemoteNunchuk:
    return "Wii Remote + Nunchuk";
  case ControllerMode::ClassicController:
    return "Classic Controller";
  }
  return "Wii Remote";
}

void OpenMenu()
{
  s_visible.store(true, std::memory_order_release);
  SetPage(Page::Main);
}

void CloseMenu()
{
  s_visible.store(false, std::memory_order_release);
  s_capture_until_release.store(true, std::memory_order_release);
  SetPage(Page::Main);
}

void RefreshStateInfo()
{
  for (int slot = 1; slot <= static_cast<int>(State::NUM_STATES); ++slot)
  {
    std::string info = State::GetInfoStringOfSlot(slot, false);
    s_state_info[slot - 1] = info.empty() ? "Empty" : std::move(info);
  }
}

void LoadCheats()
{
  s_ar_codes.clear();
  s_gecko_codes.clear();
  s_cheats_enabled = Config::Get(Config::MAIN_ENABLE_CHEATS);
  if (s_game_id.empty())
    return;

  Common::IniFile local_ini;
  local_ini.Load(File::GetUserPath(D_GAMESETTINGS_IDX) + s_game_id + ".ini");
  const Common::IniFile default_ini =
      SConfig::LoadDefaultGameIni(s_game_id, static_cast<u16>(s_revision));
  s_ar_codes = ActionReplay::LoadCodes(default_ini, local_ini);
  s_gecko_codes = Gecko::LoadCodes(default_ini, local_ini);
}

bool IsHardcoreActive()
{
  auto& manager = AchievementManager::GetInstance();
  return manager.GetClient() && manager.IsHardcoreModeActive();
}

std::string AchievementStatus()
{
  if (!Config::Get(Config::RA_ENABLED))
    return "Disabled";
  auto& manager = AchievementManager::GetInstance();
  if (!manager.HasAPIToken())
    return "Sign in from launcher";
  if (!manager.GetClient())
    return "Starting";
  std::lock_guard lock{manager.GetLock()};
  if (!manager.IsGameLoaded())
    return "Ready";
  return manager.IsHardcoreModeActive() ? "Hardcore" : "Softcore";
}

void RefreshAchievements()
{
  s_achievements.clear();
  s_achievement_presence.clear();
  s_achievement_refresh_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  if (!Config::Get(Config::RA_ENABLED))
  {
    s_achievement_summary = "RetroAchievements is disabled. Enable it from the launcher.";
    return;
  }

  auto& manager = AchievementManager::GetInstance();
  if (!manager.HasAPIToken())
  {
    s_achievement_summary = "Sign in from Settings > RetroAchievements.";
    return;
  }
  if (!manager.GetClient())
  {
    s_achievement_summary = "RetroAchievements is starting.";
    return;
  }

  std::lock_guard lock{manager.GetLock()};
  if (!manager.IsGameLoaded())
  {
    const rc_client_game_t* game = rc_client_get_game_info(manager.GetClient());
    if (game && game->hash && game->hash[0])
    {
      s_achievement_summary = "Achievements did not load for this game image.\nDetected hash: " +
                              std::string(game->hash);
    }
    else
    {
      s_achievement_summary = "Achievements are still identifying this game image.";
    }
    return;
  }

  rc_client_user_game_summary_t summary{};
  rc_client_get_user_game_summary(manager.GetClient(), &summary);
  s_achievement_summary = std::to_string(summary.num_unlocked_achievements) + "/" +
                          std::to_string(summary.num_core_achievements) + " achievements · " +
                          std::to_string(summary.points_unlocked) + "/" +
                          std::to_string(summary.points_core) + " points · " +
                          (manager.IsHardcoreModeActive() ? "Hardcore" : "Softcore");
  const auto presence = manager.GetRichPresence();
  s_achievement_presence = presence.data();

  rc_client_achievement_list_t* list = rc_client_create_achievement_list(
      manager.GetClient(), RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
      RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
  if (!list)
    return;
  for (std::uint32_t bucket_index = 0; bucket_index < list->num_buckets; ++bucket_index)
  {
    const rc_client_achievement_bucket_t& bucket = list->buckets[bucket_index];
    for (std::uint32_t index = 0; index < bucket.num_achievements; ++index)
    {
      const rc_client_achievement_t* achievement = bucket.achievements[index];
      if (!achievement)
        continue;
      AchievementEntry entry;
      entry.title = achievement->title ? achievement->title : "Achievement";
      entry.description = achievement->description ? achievement->description : "";
      entry.progress.assign(achievement->measured_progress,
                            strnlen(achievement->measured_progress,
                                    sizeof(achievement->measured_progress)));
      entry.points = achievement->points;
      entry.unlocked = achievement->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE;
      s_achievements.emplace_back(std::move(entry));
    }
  }
  rc_client_destroy_achievement_list(list);
}

bool SaveAndApplyCheats(bool action_replay)
{
  if (s_game_id.empty())
  {
    s_status = "Cheat configuration is unavailable for this executable";
    s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    return false;
  }

  const std::string& settings_directory = File::GetUserPath(D_GAMESETTINGS_IDX);
  const std::string ini_path = settings_directory + s_game_id + ".ini";
  if (!File::CreateFullPath(settings_directory))
  {
    s_status = "Could not create the GameSettings folder";
    s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    return false;
  }

  Common::IniFile local_ini;
  if (File::Exists(ini_path))
    local_ini.Load(ini_path);
  if (action_replay)
    ActionReplay::SaveCodes(&local_ini, s_ar_codes);
  else
    Gecko::SaveCodes(local_ini, s_gecko_codes);

  if (!local_ini.Save(ini_path))
  {
    s_status = "Could not save the game cheat configuration";
    s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    return false;
  }

  // Update live codes only after their INI changes reach storage.
  if (action_replay)
    ActionReplay::ApplyCodes(s_ar_codes, s_game_id, static_cast<u16>(s_revision));
  else
    Gecko::SetActiveCodes(s_gecko_codes, s_game_id, static_cast<u16>(s_revision));
  s_status = "Cheat configuration updated";
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  return true;
}

BrowserScanResult ScanBrowserDirectory(const BrowserScanRequest& request)
{
  BrowserScanResult result;
  std::filesystem::path directory(request.directory);
  result.directory = directory.string();
  std::error_code error;
  if (directory.empty() || !std::filesystem::is_directory(directory, error))
  {
    result.error = "That folder is not available.";
  }
  else
  {
    std::filesystem::directory_iterator iterator(
        directory, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end)
    {
      if (s_browser_scan_generation.load(std::memory_order_acquire) != request.generation)
      {
        result.cancelled = true;
        return result;
      }

      const std::filesystem::directory_entry& entry = *iterator;
      std::error_code type_error;
      const bool is_directory = entry.is_directory(type_error);
      const bool is_file = !type_error && entry.is_regular_file(type_error);
      if (!type_error && (is_directory || (is_file && IsDiscImage(entry.path()))))
      {
        result.entries.push_back(
            {entry.path().filename().string(), entry.path().string(), is_directory});
      }
      iterator.increment(error);
    }
    if (error)
      result.error = "Some files in this folder could not be read.";
  }

  if (s_browser_scan_generation.load(std::memory_order_acquire) != request.generation)
  {
    result.cancelled = true;
    return result;
  }

  std::ranges::sort(result.entries, [](const BrowserEntry& lhs, const BrowserEntry& rhs) {
    if (lhs.directory != rhs.directory)
      return lhs.directory > rhs.directory;
    return Lower(lhs.name) < Lower(rhs.name);
  });

  return result;
}

void BrowserScanWorker()
{
  Common::SetCurrentThreadName("Dolphin disc browser");

  while (true)
  {
    BrowserScanRequest request;
    {
      std::unique_lock lock{s_browser_worker_mutex};
      s_browser_worker_cv.wait(
          lock, [] { return s_browser_worker_shutdown || s_pending_browser_scan.has_value(); });
      if (s_browser_worker_shutdown)
        return;
      request = std::move(*s_pending_browser_scan);
      s_pending_browser_scan.reset();
    }

    BrowserScanResult result = ScanBrowserDirectory(request);
    if (result.cancelled ||
        s_browser_scan_generation.load(std::memory_order_acquire) != request.generation)
    {
      continue;
    }

    std::lock_guard lock{s_mutex};
    if (!s_initialized ||
        s_browser_scan_generation.load(std::memory_order_acquire) != request.generation)
    {
      continue;
    }

    s_browser_directory = std::move(result.directory);
    s_browser_entries = std::move(result.entries);
    s_browser_error = std::move(result.error);
    s_browser_scanning = false;
    s_selection = 0;
  }
}

void StartBrowserScanWorker()
{
  std::lock_guard lock{s_browser_worker_mutex};
  if (s_browser_worker.joinable())
    return;

  s_browser_worker_shutdown = false;
  s_browser_worker = std::thread(BrowserScanWorker);
}

void InvalidateBrowserScan()
{
  s_browser_scan_generation.fetch_add(1, std::memory_order_acq_rel);
  std::lock_guard lock{s_browser_worker_mutex};
  s_pending_browser_scan.reset();
}

void StopBrowserScanWorker()
{
  s_browser_scan_generation.fetch_add(1, std::memory_order_acq_rel);
  {
    std::lock_guard lock{s_browser_worker_mutex};
    s_browser_worker_shutdown = true;
    s_pending_browser_scan.reset();
  }
  s_browser_worker_cv.notify_all();
  if (s_browser_worker.joinable())
    s_browser_worker.join();
}

void StateSaveMonitorWorker()
{
  Common::SetCurrentThreadName("Dolphin state monitor");

  while (true)
  {
    StateSaveMonitorRequest request;
    {
      std::unique_lock lock{s_state_worker_mutex};
      s_state_worker_cv.wait(
          lock, [] { return s_state_worker_shutdown || s_pending_state_save_monitor.has_value(); });
      if (s_state_worker_shutdown)
        return;
      request = std::move(*s_pending_state_save_monitor);
      s_pending_state_save_monitor.reset();
    }

    // Wait for the asynchronous state write to complete.
    const std::uint64_t timestamp = State::GetUnixTimeOfSlot(request.slot);
    const std::string info = timestamp == 0 ? std::string{} :
                                               State::GetInfoStringOfSlot(request.slot, false);
    if (s_state_worker_generation.load(std::memory_order_acquire) != request.generation)
      continue;

    std::lock_guard lock{s_mutex};
    if (!s_initialized ||
        s_state_worker_generation.load(std::memory_order_acquire) != request.generation)
    {
      continue;
    }

    const bool success = timestamp != 0 && timestamp != request.previous_timestamp &&
                         !info.empty() && info != "Empty" && info != "Unknown";
    if (success)
    {
      if (request.slot >= 1 && request.slot <= static_cast<int>(State::NUM_STATES))
        s_state_info[request.slot - 1] = info;
      s_status = "Saved state to slot " + std::to_string(request.slot);
    }
    else
    {
      s_status = "Failed to save state to slot " + std::to_string(request.slot);
    }
    s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  }
}

void StartStateSaveMonitorWorker()
{
  std::lock_guard lock{s_state_worker_mutex};
  if (s_state_worker.joinable())
    return;
  s_state_worker_shutdown = false;
  s_state_worker = std::thread(StateSaveMonitorWorker);
}

void InvalidateStateSaveMonitor()
{
  s_state_worker_generation.fetch_add(1, std::memory_order_acq_rel);
  std::lock_guard lock{s_state_worker_mutex};
  s_pending_state_save_monitor.reset();
}

void StopStateSaveMonitorWorker()
{
  s_state_worker_generation.fetch_add(1, std::memory_order_acq_rel);
  {
    std::lock_guard lock{s_state_worker_mutex};
    s_state_worker_shutdown = true;
    s_pending_state_save_monitor.reset();
  }
  s_state_worker_cv.notify_all();
  if (s_state_worker.joinable())
    s_state_worker.join();
}

void OnStateLoadCompleted()
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized || s_pending_load_slot <= 0)
    return;
  s_status = "State load finished for slot " + std::to_string(s_pending_load_slot);
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  s_pending_load_slot = 0;
}

// The caller owns s_mutex; filesystem work runs on the persistent worker.
void RequestBrowserDirectoryScan(std::string requested_directory)
{
  const std::uint64_t generation =
      s_browser_scan_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  s_browser_directory = requested_directory;
  s_browser_entries.clear();
  s_browser_error.clear();
  s_browser_scanning = true;
  s_selection = 0;

  {
    std::lock_guard lock{s_browser_worker_mutex};
    if (s_browser_worker_shutdown)
    {
      s_browser_scanning = false;
      s_browser_error = "The disc browser worker is unavailable.";
      return;
    }
    // Keep only the newest USB/SMB scan request.
    s_pending_browser_scan = BrowserScanRequest{std::move(requested_directory), generation};
  }
  s_browser_worker_cv.notify_one();
}

void EnterDiscBrowser()
{
  std::filesystem::path initial(s_game_path);
  initial = initial.parent_path();
  if (initial.empty())
    initial = "sdmc:/";
  SetPage(Page::DiscBrowser);
  RequestBrowserDirectoryScan(initial.string());
}

bool HasGBA()
{
  return std::ranges::any_of(s_gba_ports, [](bool connected) { return connected; });
}

int ItemCount()
{
  switch (s_page)
  {
  case Page::Main:
    return MAIN_ITEM_COUNT + (HasGBA() ? 1 : 0);
  case Page::SaveStates:
  case Page::LoadStates:
    return static_cast<int>(State::NUM_STATES) + 1;
  case Page::Disc:
    return DISC_ITEM_COUNT;
  case Page::DiscBrowser:
    return std::max(1, static_cast<int>(s_browser_entries.size()));
  case Page::Cheats:
    return 2 + static_cast<int>(s_ar_codes.size() + s_gecko_codes.size());
  case Page::Achievements:
    return static_cast<int>(s_achievements.size()) + 1;
  case Page::Controllers:
    return CONTROLLER_ITEM_COUNT;
  case Page::ControllerModes:
    return CONTROLLER_MODE_ITEM_COUNT;
  case Page::Skylanders:
    return static_cast<int>(Skylanders::SLOT_COUNT) + 2;
  case Page::SkylanderFigures:
    return static_cast<int>(s_skylander_figures.size()) + 2;
  case Page::GBA:
    return 7;
  case Page::Alert:
  case Page::ScreenLayout:
    return 1;
  }
  return 1;
}

bool ItemAvailable(int index)
{
  if (s_page == Page::GBA && index >= 2 && index < 6)
    return s_gba_ports[index - 2];
  if (s_page == Page::Controllers && index == CONTROLLER_CALIBRATE_ITEM)
    return s_is_wii;
  if (s_page != Page::Main)
    return true;
  switch (index)
  {
  case MAIN_LOAD_STATE:
    return !IsHardcoreActive();
  case MAIN_CONTROLLERS:
  case MAIN_SKYLANDERS:
    return s_is_wii;
  case MAIN_FRAME_GENERATION:
    return DolphinSwitch::FrameGeneration::IsAvailable();
  case MAIN_CPU_BOOST:
#ifdef __SWITCH2__
    return true;
#else
    return false;
#endif
  default:
    return true;
  }
}

void HandleBack()
{
  switch (s_page)
  {
  case Page::Main:
  case Page::Alert:
    CloseMenu();
    return;
  case Page::DiscBrowser:
  {
    const std::filesystem::path current(s_browser_directory);
    const std::filesystem::path parent = current.parent_path();
    if (!parent.empty() && parent != current)
      RequestBrowserDirectoryScan(parent.string());
    else
      SetPage(Page::Disc);
    return;
  }
  case Page::SkylanderFigures:
    SetPage(Page::Skylanders);
    s_selection = s_skylander_slot + 1;
    return;
  case Page::Skylanders:
  case Page::Controllers:
  case Page::GBA:
    SetPage(Page::Main);
    return;
  case Page::ControllerModes:
    SetPage(Page::Controllers);
    s_selection = std::clamp(s_controller_player, 0, 3);
    return;
  default:
    SetPage(Page::Main);
    return;
  }
}

void ChangeGBASetting(int delta)
{
  if (s_selection == 0)
  {
    const int current =
        std::clamp(static_cast<int>(Config::Get(Config::MAIN_SWITCH_GBA_SCREENS)), 0, 2);
    Config::SetBase(Config::MAIN_SWITCH_GBA_SCREENS,
                    static_cast<Config::GBAScreens>((current + delta + 3) % 3));
  }
  else if (s_selection == 1)
  {
    const int current =
        std::clamp(static_cast<int>(Config::Get(Config::MAIN_SWITCH_GBA_SCREEN_CORNER)), 0, 3);
    Config::SetBase(Config::MAIN_SWITCH_GBA_SCREEN_CORNER,
                    static_cast<Config::GBAScreenCorner>((current + delta + 4) % 4));
  }
}

void ActivateSelection()
{
  switch (s_page)
  {
  case Page::Main:
    switch (s_selection)
    {
    case MAIN_SAVE_STATE:
      SetPage(Page::SaveStates);
      break;
    case MAIN_LOAD_STATE:
      SetPage(Page::LoadStates);
      break;
    case MAIN_DISC:
      SetPage(Page::Disc);
      break;
    case MAIN_CHEATS:
      SetPage(Page::Cheats);
      break;
    case MAIN_ACHIEVEMENTS:
      SetPage(Page::Achievements);
      RefreshAchievements();
      break;
    case MAIN_CONTROLLERS:
      if (s_is_wii)
        SetPage(Page::Controllers);
      break;
    case MAIN_SCREEN_LAYOUT:
      SetPage(Page::ScreenLayout);
      ScreenLayout::BeginEdit(s_gba_ports);
      break;
    case MAIN_FRAME_GENERATION:
      QueueAction({ActionType::ToggleFrameGeneration});
      break;
    case MAIN_FPS:
      QueueAction({ActionType::ToggleFPS});
      break;
    case MAIN_VBI_SKIP:
      QueueAction({ActionType::ToggleVBISkip});
      break;
    case MAIN_CPU_BOOST:
#ifdef __SWITCH2__
      QueueAction({ActionType::ToggleCpuBoost});
#endif
      break;
    case MAIN_SKYLANDERS:
      if (s_is_wii)
        SetPage(Page::Skylanders);
      break;
    case MAIN_RESET:
      QueueAction({ActionType::Reset});
      CloseMenu();
      break;
    case MAIN_STOP:
      QueueAction({ActionType::StopToLauncher});
      CloseMenu();
      break;
    case MAIN_GBA:
      SetPage(Page::GBA);
      break;
    }
    break;
  case Page::SaveStates:
  case Page::LoadStates:
    if (s_selection == static_cast<int>(State::NUM_STATES))
    {
      SetPage(Page::Main);
    }
    else
    {
      QueueAction({s_page == Page::SaveStates ? ActionType::SaveState : ActionType::LoadState,
                   s_selection + 1});
    }
    break;
  case Page::Disc:
    if (s_selection == 0)
    {
      QueueAction({ActionType::EjectDisc});
      CloseMenu();
    }
    else if (s_selection == 1)
    {
      EnterDiscBrowser();
    }
    else
    {
      SetPage(Page::Main);
    }
    break;
  case Page::DiscBrowser:
    if (s_browser_entries.empty())
    {
      if (!s_browser_scanning)
        SetPage(Page::Disc);
      break;
    }
    if (const BrowserEntry selected = s_browser_entries[s_selection]; selected.directory)
    {
      RequestBrowserDirectoryScan(selected.path);
    }
    else
    {
      QueueAction({ActionType::ChangeDisc, 0, selected.path});
      CloseMenu();
    }
    break;
  case Page::Cheats:
    if (s_selection == 0)
      QueueAction({ActionType::ToggleCheats});
    else if (s_selection == ItemCount() - 1)
      SetPage(Page::Main);
    else
      QueueAction({ActionType::ToggleCheat, s_selection - 1});
    break;
  case Page::Achievements:
    if (s_selection == static_cast<int>(s_achievements.size()))
      SetPage(Page::Main);
    break;
  case Page::Controllers:
    if (s_selection == CONTROLLER_ITEM_COUNT - 1)
    {
      SetPage(Page::Main);
    }
    else if (s_selection == CONTROLLER_CALIBRATE_ITEM)
    {
      if (s_is_wii)
      {
        QueueAction({ActionType::CalibrateTouchPointer});
        CloseMenu();
      }
    }
    else
    {
      s_controller_player = s_selection;
      SetPage(Page::ControllerModes);
      s_selection = static_cast<int>(s_controller_modes[s_controller_player]);
    }
    break;
  case Page::ControllerModes:
    if (s_selection == CONTROLLER_MODE_ITEM_COUNT - 1)
    {
      SetPage(Page::Controllers);
      s_selection = s_controller_player;
    }
    else
    {
      Action action{ActionType::SetControllerMode};
      action.value = s_controller_player;
      action.value2 = s_selection;
      QueueAction(std::move(action));
      SetPage(Page::Controllers);
      s_selection = s_controller_player;
    }
    break;
  case Page::Skylanders:
    if (s_selection == 0)
      QueueAction({ActionType::ToggleSkylandersPortal});
    else if (s_selection <= static_cast<int>(Skylanders::SLOT_COUNT))
    {
      s_skylander_slot = s_selection - 1;
      s_skylander_figures.clear();
      s_skylander_error.clear();
      s_skylander_scanning = true;
      SetPage(Page::SkylanderFigures);
      QueueAction({ActionType::ListSkylanderFigures});
    }
    else
      SetPage(Page::Main);
    break;
  case Page::SkylanderFigures:
    if (s_selection == 0)
      QueueAction({ActionType::RemoveSkylanderFigure, s_skylander_slot});
    else if (s_selection <= static_cast<int>(s_skylander_figures.size()))
      QueueAction({ActionType::LoadSkylanderFigure, s_skylander_slot,
                   s_skylander_figures[s_selection - 1].path});
    SetPage(Page::Skylanders);
    s_selection = s_skylander_slot + 1;
    break;
  case Page::Alert:
    CloseMenu();
    break;
  case Page::ScreenLayout:
    break;
  case Page::GBA:
    if (s_selection < 2)
      ChangeGBASetting(1);
    else if (s_selection < 6 && s_gba_ports[s_selection - 2])
    {
      QueueAction({ActionType::ResetGBA, s_selection - 2});
      CloseMenu();
    }
    else
      SetPage(Page::Main);
    break;
  }
}

constexpr ImU32 TEXT_COLOR = IM_COL32(235, 248, 255, 255);
constexpr ImU32 VALUE_COLOR = IM_COL32(150, 205, 230, 255);
constexpr ImU32 DIM_COLOR = IM_COL32(120, 145, 158, 255);
constexpr ImU32 ACCENT_COLOR = IM_COL32(61, 183, 235, 255);
constexpr ImU32 HIGHLIGHT_COLOR = IM_COL32(15, 89, 126, 245);

// What a row shows on its right.
enum class RowKind
{
  Action,
  // A page: a chevron, after the value if there is one.
  Submenu,
  // A switch, after the value (a note) if there is one.
  Toggle,
  // The value between left and right chevrons.
  Choice,
  // The value alone.
  Value,
};

void CenteredText(std::string_view text)
{
  const float width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
  ImGui::SetCursorPosX(
      std::floor(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - width) * 0.5f)));
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void DimCenteredText(std::string_view text)
{
  ImGui::PushStyleColor(ImGuiCol_Text, DIM_COLOR);
  CenteredText(text);
  ImGui::PopStyleColor();
}

// The overlay's sizes are laid out for 1080 lines: scaled to the output's height (2x on a 4K TV).
float OverlayScale(const ImGuiIO& io)
{
  return std::max(0.72f, io.DisplaySize.y / 1080.0f);
}

// The text, cut on a character boundary and ended with "..." when it is wider than max_width.
std::string FitText(std::string text, float max_width)
{
  if (ImGui::CalcTextSize(text.c_str()).x <= max_width)
    return text;
  while (!text.empty())
  {
    do
      text.pop_back();
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80);
    if (ImGui::CalcTextSize((text + "...").c_str()).x <= max_width)
      break;
  }
  return text + "...";
}

void DrawChevron(ImDrawList* draw, float right, float center_y, float size, bool pointing_right,
                 ImU32 color)
{
  const float half = size * 0.5f;
  const float tip = pointing_right ? right : right - half;
  const float back = pointing_right ? right - half : right;
  const float thickness = std::max(2.0f, size / 6.0f);
  draw->AddLine({back, center_y - half}, {tip, center_y}, color, thickness);
  draw->AddLine({tip, center_y}, {back, center_y + half}, color, thickness);
}

// An On/Off switch, as the launcher draws it: an accent pill with its knob on the right when on.
void DrawSwitch(ImDrawList* draw, float right, float center_y, float height, bool on, bool enabled)
{
  const float width = std::round(height * 1.8f);
  const ImVec2 min{std::floor(right - width), std::floor(center_y - height * 0.5f)};
  const ImVec2 max{min.x + width, min.y + height};
  const ImU32 track = !enabled ? IM_COL32(44, 52, 62, 255) :
                      on       ? ACCENT_COLOR :
                                 IM_COL32(58, 68, 82, 255);
  draw->AddRectFilled(min, max, track, height * 0.5f);
  const float inset = std::max(2.0f, std::round(height / 8.0f));
  const float radius = height * 0.5f - inset;
  draw->AddCircleFilled({on ? max.x - inset - radius : min.x + inset + radius, min.y + height * 0.5f},
                        radius,
                        enabled ? IM_COL32(245, 250, 255, 255) : IM_COL32(140, 150, 160, 255), 32);
}

// A row of the menu: the selection highlight, the label on its left and, by kind, a value, a switch
// or a chevron on its right, all centred on the highlight.
void MenuRow(int index, std::string_view label, RowKind kind = RowKind::Action,
             std::string_view value = {}, bool on = false, bool enabled = true,
             float height = 46.0f)
{
  const float scale = s_ui_scale;
  const float row_height = std::round(height * scale);
  const ImVec2 origin{std::floor(ImGui::GetCursorScreenPos().x),
                      std::floor(ImGui::GetCursorScreenPos().y)};
  const float width = std::floor(ImGui::GetContentRegionAvail().x);
  ImGui::Dummy({width, row_height});
  const bool selected = s_selection == index;
  if (selected)
    ImGui::SetScrollHereY(0.5f);

  ImDrawList* const draw = ImGui::GetWindowDrawList();
  if (selected)
  {
    draw->AddRectFilled(origin, {origin.x + width, origin.y + row_height}, HIGHLIGHT_COLOR,
                        std::round(8.0f * scale));
    draw->AddRectFilled({origin.x, std::floor(origin.y + row_height * 0.2f)},
                        {origin.x + std::round(3.0f * scale),
                         std::floor(origin.y + row_height * 0.8f)},
                        ACCENT_COLOR);
  }

  const float padding = std::round(18.0f * scale);
  const float gap = std::round(14.0f * scale);
  const float center_y = origin.y + row_height * 0.5f;
  const float text_y = std::floor(center_y - ImGui::GetFontSize() * 0.5f);
  const float chevron = std::round(14.0f * scale);
  const ImU32 value_color = enabled ? VALUE_COLOR : DIM_COLOR;
  float right = origin.x + width - padding;

  const auto draw_value = [&](std::string_view text) {
    const std::string fitted = FitText(std::string(text), width * 0.45f);
    const float text_width = ImGui::CalcTextSize(fitted.c_str()).x;
    right -= text_width;
    draw->AddText({std::floor(right), text_y}, value_color, fitted.c_str());
    right -= gap;
  };
  switch (kind)
  {
  case RowKind::Action:
    break;
  case RowKind::Submenu:
    DrawChevron(draw, right, center_y, chevron, true, value_color);
    right -= chevron * 0.5f + gap;
    if (!value.empty())
      draw_value(value);
    break;
  case RowKind::Toggle:
  {
    const float switch_height = std::round(26.0f * scale);
    DrawSwitch(draw, right, center_y, switch_height, on, enabled);
    right -= std::round(switch_height * 1.8f) + gap;
    if (!value.empty())
      draw_value(value);
    break;
  }
  case RowKind::Choice:
    DrawChevron(draw, right, center_y, chevron, true, value_color);
    right -= chevron * 0.5f + gap;
    draw_value(value);
    DrawChevron(draw, right, center_y, chevron, false, value_color);
    right -= chevron * 0.5f + gap;
    break;
  case RowKind::Value:
    draw_value(value);
    break;
  }

  const float label_x = origin.x + padding;
  const std::string fitted = FitText(std::string(label), right - label_x);
  draw->AddText({label_x, text_y}, enabled ? TEXT_COLOR : DIM_COLOR, fitted.c_str());
}

void RenderMainPage()
{
  const bool hardcore = IsHardcoreActive();
  MenuRow(MAIN_SAVE_STATE, "Save state", RowKind::Submenu);
  if (hardcore)
    MenuRow(MAIN_LOAD_STATE, "Load state", RowKind::Value, "Hardcore disabled", false, false);
  else
    MenuRow(MAIN_LOAD_STATE, "Load state", RowKind::Submenu);
  MenuRow(MAIN_DISC, "Disc management", RowKind::Submenu);
  MenuRow(MAIN_CHEATS, "Cheats", RowKind::Submenu);
  MenuRow(MAIN_ACHIEVEMENTS, "RetroAchievements", RowKind::Submenu, AchievementStatus());
  if (s_is_wii)
    MenuRow(MAIN_CONTROLLERS, "Controller modes", RowKind::Submenu);
  MenuRow(MAIN_SCREEN_LAYOUT, "Screen layout", RowKind::Submenu);
  if (DolphinSwitch::FrameGeneration::IsAvailable())
  {
    const bool enabled = DolphinSwitch::FrameGeneration::IsEnabled();
    MenuRow(MAIN_FRAME_GENERATION, "Frame Generation", RowKind::Toggle,
            enabled && DolphinSwitch::FrameGeneration::IsHighFPSPassthrough() ?
                "Native FPS protected" :
                "",
            enabled);
  }
  else
  {
    MenuRow(MAIN_FRAME_GENERATION, "Frame Generation", RowKind::Value, "Unavailable", false, false);
  }
  MenuRow(MAIN_FPS, "Show FPS", RowKind::Toggle, {}, s_show_fps);
  MenuRow(MAIN_VBI_SKIP, "VBI Skip", RowKind::Toggle, {}, s_vbi_skip);
#ifdef __SWITCH2__
  MenuRow(MAIN_CPU_BOOST, "CPU Boost", RowKind::Toggle, {}, s_cpu_boost);
#endif
  if (s_is_wii)
    MenuRow(MAIN_SKYLANDERS, "Skylanders portal", RowKind::Submenu);
  MenuRow(MAIN_RESET, "Reset console");
  MenuRow(MAIN_STOP, "Return to Dolphin launcher");
  if (HasGBA())
    MenuRow(MAIN_GBA, "Game Boy Advance", RowKind::Submenu);
}

// The rows RenderMainPage draws.
int MainPageRowCount()
{
  int rows = 11 + (s_is_wii ? 2 : 0) + (HasGBA() ? 1 : 0);
#ifdef __SWITCH2__
  ++rows;
#endif
  return rows;
}

void RenderSkylandersPage()
{
  MenuRow(0, "Emulated portal", RowKind::Toggle, {},
          Config::Get(Config::MAIN_EMULATE_SKYLANDER_PORTAL));
  const auto slots = Skylanders::GetSlots();
  for (std::size_t slot = 0; slot < slots.size(); ++slot)
  {
    MenuRow(static_cast<int>(slot + 1), "Slot " + std::to_string(slot + 1), RowKind::Submenu,
            slots[slot].name);
  }
  MenuRow(static_cast<int>(slots.size() + 1), "Back");
}

void RenderSkylanderFigures()
{
  ImGui::TextWrapped("Portal slot %d", s_skylander_slot + 1);
  if (s_skylander_scanning)
    ImGui::TextUnformatted("Reading figure collection...");
  else if (!s_skylander_error.empty())
    ImGui::TextWrapped("%s", s_skylander_error.c_str());
  else if (s_skylander_figures.empty())
    ImGui::TextWrapped("No figure files found. Import or create figures in the launcher's "
                       "Skylanders portal settings.");
  MenuRow(0, "Remove current figure");
  for (std::size_t index = 0; index < s_skylander_figures.size(); ++index)
    MenuRow(static_cast<int>(index + 1), s_skylander_figures[index].name);
  MenuRow(static_cast<int>(s_skylander_figures.size() + 1), "Back");
}

void RenderGBAPage()
{
  static constexpr std::array<std::string_view, 3> sizes = {"Hidden", "Small", "Large"};
  static constexpr std::array<std::string_view, 4> corners = {"Top left", "Top right",
                                                              "Bottom left", "Bottom right"};
  const int size = std::clamp(static_cast<int>(Config::Get(Config::MAIN_SWITCH_GBA_SCREENS)), 0, 2);
  const int corner =
      std::clamp(static_cast<int>(Config::Get(Config::MAIN_SWITCH_GBA_SCREEN_CORNER)), 0, 3);
  MenuRow(0, "Screens", RowKind::Choice, sizes[size]);
  MenuRow(1, "Position", RowKind::Choice, corners[corner]);
  for (int port = 0; port < 4; ++port)
  {
    MenuRow(port + 2, "Reset GBA " + std::to_string(port + 1), RowKind::Action, {}, false,
            s_gba_ports[port]);
  }
  MenuRow(6, "Back");
}

void RenderControllersPage()
{
  for (int player = 0; player < 4; ++player)
  {
    MenuRow(player, "Player " + std::to_string(player + 1), RowKind::Submenu,
            ControllerModeName(s_controller_modes[player]));
  }
  MenuRow(CONTROLLER_CALIBRATE_ITEM, "Calibrate touch pointer", RowKind::Action, {}, false,
          s_is_wii);
  MenuRow(CONTROLLER_ITEM_COUNT - 1, "Back");
  ImGui::Spacing();
  DimCenteredText("Changes apply only to the current game session");
}

void RenderControllerModesPage()
{
  static constexpr std::array<ControllerMode, 5> modes = {
      ControllerMode::GameCube, ControllerMode::WiiRemote,
      ControllerMode::WiiRemoteSideways, ControllerMode::WiiRemoteNunchuk,
      ControllerMode::ClassicController};
  for (int index = 0; index < static_cast<int>(modes.size()); ++index)
  {
    const bool active = s_controller_modes[s_controller_player] == modes[index];
    MenuRow(index, ControllerModeName(modes[index]), active ? RowKind::Value : RowKind::Action,
            active ? "Active" : "");
  }
  MenuRow(CONTROLLER_MODE_ITEM_COUNT - 1, "Back");
}

// The height a page's scrolling list can take above the status line.
float ListHeight(float minimum, float below = 0.0f)
{
  return std::max(minimum * s_ui_scale,
                  ImGui::GetContentRegionAvail().y - s_footer_height - below * s_ui_scale);
}

void RenderStatePage()
{
  ImGui::BeginChild("state slots", {0.0f, ListHeight(220.0f)}, false, ImGuiWindowFlags_NoInputs);
  for (int slot = 0; slot < static_cast<int>(State::NUM_STATES); ++slot)
    MenuRow(slot, "Slot " + std::to_string(slot + 1), RowKind::Value, s_state_info[slot]);
  MenuRow(static_cast<int>(State::NUM_STATES), "Back");
  ImGui::EndChild();
}

void RenderDiscPage()
{
  MenuRow(0, "Eject current disc");
  MenuRow(1, "Change disc from current game folder", RowKind::Submenu);
  MenuRow(2, "Back");
  ImGui::Spacing();
  DimCenteredText("Disc images: ISO, GCM, TGC, WBFS, CISO, GCZ, WIA and RVZ");
}

void RenderDiscBrowser()
{
  ImGui::PushStyleColor(ImGuiCol_Text, DIM_COLOR);
  ImGui::TextUnformatted(FitText(s_browser_directory, ImGui::GetContentRegionAvail().x).c_str());
  ImGui::PopStyleColor();
  ImGui::Separator();
  ImGui::BeginChild("disc files", {0.0f, ListHeight(200.0f)}, false, ImGuiWindowFlags_NoInputs);
  if (s_browser_entries.empty())
  {
    MenuRow(0,
            s_browser_scanning ? "Scanning folder..." :
                                 (s_browser_error.empty() ? "No disc images in this folder" :
                                                            s_browser_error));
  }
  else
  {
    for (int index = 0; index < static_cast<int>(s_browser_entries.size()); ++index)
    {
      const BrowserEntry& entry = s_browser_entries[index];
      MenuRow(index, entry.name, entry.directory ? RowKind::Submenu : RowKind::Action);
    }
  }
  ImGui::EndChild();
}

void RenderCheatsPage()
{
  const bool no_codes = s_game_id.empty() || (s_ar_codes.empty() && s_gecko_codes.empty());
  ImGui::BeginChild("cheat list", {0.0f, ListHeight(220.0f, no_codes ? 40.0f : 0.0f)}, false,
                    ImGuiWindowFlags_NoInputs);
  MenuRow(0, "Cheat engine", RowKind::Toggle, {}, s_cheats_enabled);
  int row = 1;
  for (const ActionReplay::ARCode& code : s_ar_codes)
    MenuRow(row++, code.name, RowKind::Toggle, "AR", code.enabled);
  for (const Gecko::GeckoCode& code : s_gecko_codes)
    MenuRow(row++, code.name, RowKind::Toggle, "Gecko", code.enabled);
  MenuRow(row, "Back");
  ImGui::EndChild();
  if (s_game_id.empty())
    DimCenteredText("Cheat lists are unavailable for this executable.");
  else if (no_codes)
    DimCenteredText("No AR or Gecko codes are installed for this game.");
}

void RenderAchievementsPage()
{
  ImGui::TextWrapped("%s", s_achievement_summary.c_str());
  if (!s_achievement_presence.empty())
    ImGui::TextColored({0.55f, 0.92f, 1.0f, 1.0f}, "%s", s_achievement_presence.c_str());
  ImGui::Separator();
  ImGui::BeginChild("achievement list", {0.0f, ListHeight(210.0f, 64.0f)}, false,
                    ImGuiWindowFlags_NoInputs);
  int row = 0;
  for (const AchievementEntry& achievement : s_achievements)
  {
    MenuRow(row++, achievement.title, RowKind::Value,
            (achievement.unlocked ? "Unlocked · " : "") + std::to_string(achievement.points) +
                " pts",
            false, achievement.unlocked);
  }
  MenuRow(row, "Back");
  ImGui::EndChild();
  if (s_selection >= 0 && s_selection < static_cast<int>(s_achievements.size()))
  {
    const AchievementEntry& selected = s_achievements[s_selection];
    std::string detail = selected.description;
    if (!selected.unlocked && !selected.progress.empty())
      detail += (detail.empty() ? "" : " · ") + selected.progress;
    ImGui::TextWrapped("%s", detail.c_str());
  }
}

void RenderAlert()
{
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 205, 116, 255));
  CenteredText(s_alert_caption.empty() ? "Dolphin message" : s_alert_caption);
  ImGui::PopStyleColor();
  ImGui::Spacing();
  ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 30.0f * s_ui_scale);
  ImGui::TextWrapped("%s", s_alert_message.c_str());
  ImGui::PopTextWrapPos();
}
}  // namespace

void InitializeRendererHook()
{
  StartBrowserScanWorker();
  StartStateSaveMonitorWorker();
  State::SetOnAfterLoadCallback(&OnStateLoadCompleted);
  VideoCommon::SetImGuiRenderCallback(&Draw);
  VideoCommon::SetImGuiReleaseCallback(&GBAOverlay::ReleaseTextures);
}

void ShutdownRendererHook()
{
  VideoCommon::SetImGuiRenderCallback(nullptr);
  VideoCommon::SetImGuiReleaseCallback(nullptr);
  State::SetOnAfterLoadCallback({});
  StopBrowserScanWorker();
  StopStateSaveMonitorWorker();
}

void BeginSession(std::string game_path, std::string game_id, unsigned revision, bool is_wii,
                  std::array<ControllerMode, 4> controller_modes)
{
  StartBrowserScanWorker();
  StartStateSaveMonitorWorker();
  InvalidateBrowserScan();
  InvalidateStateSaveMonitor();
  std::lock_guard lock{s_mutex};
  s_game_path = std::move(game_path);
  s_game_id = std::move(game_id);
  s_revision = revision;
  for (int port = 0; port < 4; ++port)
    s_gba_ports[port] =
        Config::Get(Config::GetInfoForSIDevice(port)) == SerialInterface::SIDEVICE_GC_GBA_EMULATED;
  if (s_game_id.empty())
  {
    s_game_id = SConfig::GetInstance().GetGameID();
    s_revision = SConfig::GetInstance().GetRevision();
  }
  ScreenLayout::BeginSession(s_game_id, s_revision);
  s_visible.store(false, std::memory_order_release);
  s_capture_until_release.store(false, std::memory_order_release);
  s_render_frame_generation.store(0, std::memory_order_release);
  s_page = Page::Main;
  s_selection = 0;
  s_actions.clear();
  s_show_fps = Config::Get(Config::GFX_SHOW_FPS);
  s_vbi_skip = Config::Get(Config::GFX_HACK_VI_SKIP);
  s_is_wii = is_wii;
  s_controller_modes = controller_modes;
  s_controller_player = 0;
  s_browser_directory.clear();
  s_browser_entries.clear();
  s_browser_error.clear();
  s_browser_scanning = false;
  s_achievements.clear();
  s_achievement_summary.clear();
  s_achievement_presence.clear();
  s_achievement_refresh_at = {};
  s_status.clear();
  s_alert_caption.clear();
  s_alert_message.clear();
  s_pending_load_slot = 0;
  RefreshStateInfo();
  LoadCheats();
  s_initialized = true;
}

void EndSession()
{
  ScreenLayout::EndSession();
  InvalidateBrowserScan();
  InvalidateStateSaveMonitor();
  std::lock_guard lock{s_mutex};
  s_initialized = false;
  s_visible.store(false, std::memory_order_release);
  s_capture_until_release.store(false, std::memory_order_release);
  s_actions.clear();
  s_browser_entries.clear();
  s_browser_scanning = false;
  s_ar_codes.clear();
  s_gecko_codes.clear();
  s_achievements.clear();
  s_achievement_summary.clear();
  s_achievement_presence.clear();
  s_pending_load_slot = 0;
  s_is_wii = false;
  s_gba_ports.fill(false);
}

void UpdateInput(std::uint64_t down, std::uint64_t held, bool gamecube_adapter, float move_x,
                 float move_y, float size_x, float size_y)
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized)
    return;

  if (s_capture_until_release.load(std::memory_order_relaxed))
  {
    if (held != 0 || ciface::Switch::GetTouchscreenState().pressed)
      return;
    s_capture_until_release.store(false, std::memory_order_release);
  }

  const std::uint64_t chord =
      gamecube_adapter ? (HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Plus) : MENU_CHORD;
  if ((held & chord) == chord && (down & chord) != 0)
  {
    if (s_visible.load(std::memory_order_relaxed))
      CloseMenu();
    else
      OpenMenu();
    return;
  }

  if (!s_visible.load(std::memory_order_acquire))
    return;
  if (s_page == Page::ScreenLayout)
  {
    if (down & HidNpadButton_B)
      SetPage(Page::Main);
    else if (ScreenLayout::UpdateEditor(down, held, move_x, move_y, size_x, size_y))
      SetPage(Page::Main);
    return;
  }
  const int count = std::max(1, ItemCount());
  if (down & HidNpadButton_Up)
  {
    do
      s_selection = (s_selection + count - 1) % count;
    while (!ItemAvailable(s_selection));
  }
  else if (down & HidNpadButton_Down)
  {
    do
      s_selection = (s_selection + 1) % count;
    while (!ItemAvailable(s_selection));
  }

  if (down & HidNpadButton_B)
  {
    HandleBack();
    return;
  }
  if (down & HidNpadButton_A)
    ActivateSelection();
  else if (s_page == Page::GBA && (down & (HidNpadButton_Left | HidNpadButton_Right)))
    ChangeGBASetting((down & HidNpadButton_Left) ? -1 : 1);
}

std::vector<Action> TakeActions()
{
  std::lock_guard lock{s_mutex};
  std::vector<Action> result;
  result.reserve(s_actions.size());
  while (!s_actions.empty())
  {
    result.emplace_back(std::move(s_actions.front()));
    s_actions.pop_front();
  }
  return result;
}

bool IsVisible()
{
  return s_visible.load(std::memory_order_acquire);
}

bool IsInputCaptured()
{
  return s_visible.load(std::memory_order_acquire) ||
          s_capture_until_release.load(std::memory_order_acquire);
}

std::uint64_t GetRenderFrameGeneration()
{
  return s_render_frame_generation.load(std::memory_order_acquire);
}

void SetSkylanderFigures(std::vector<Skylanders::FigureFile> figures, std::string error)
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized || s_page != Page::SkylanderFigures)
    return;
  s_skylander_figures = std::move(figures);
  s_skylander_error = std::move(error);
  s_skylander_scanning = false;
  s_selection = std::clamp(s_selection, 0, static_cast<int>(s_skylander_figures.size()) + 1);
}

void SetStatus(std::string message)
{
  std::lock_guard lock{s_mutex};
  s_status = std::move(message);
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

void ShowBanner(std::string message)
{
  std::lock_guard lock{s_mutex};
  s_banner = std::move(message);
  s_banner_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

void SetTouchCalibration(bool active, float target_x, float target_y, std::string instructions)
{
  std::lock_guard lock{s_mutex};
  s_calibration_active = active;
  s_calibration_x = target_x;
  s_calibration_y = target_y;
  s_calibration_text = std::move(instructions);
}

void ShowAlert(std::string caption, std::string message)
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized)
    return;
  s_alert_caption = std::move(caption);
  s_alert_message = std::move(message);
  SetPage(Page::Alert);
  s_visible.store(true, std::memory_order_release);
}

void TrackStateSave(int slot, std::uint64_t previous_timestamp)
{
  const std::uint64_t generation =
      s_state_worker_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  {
    std::lock_guard lock{s_mutex};
    if (!s_initialized)
      return;
    s_status = "Saving state to slot " + std::to_string(slot) + "...";
    s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  }
  {
    std::lock_guard lock{s_state_worker_mutex};
    if (s_state_worker_shutdown)
    {
      SetStatus("State save monitor is unavailable");
      return;
    }
    s_pending_state_save_monitor = StateSaveMonitorRequest{slot, previous_timestamp, generation};
  }
  s_state_worker_cv.notify_one();
}

void TrackStateLoad(int slot)
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized)
    return;
  s_pending_load_slot = slot;
  s_status = "Loading state from slot " + std::to_string(slot) + "...";
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
}

void ToggleCheatsForSession()
{
  std::lock_guard lock{s_mutex};
  s_cheats_enabled = !s_cheats_enabled;
  if (s_cheats_enabled)
  {
    // Open the session gate before rebuilding live codes.
    Config::SetCurrent(Config::MAIN_ENABLE_CHEATS, true);
    ActionReplay::ApplyCodes(s_ar_codes, s_game_id, static_cast<u16>(s_revision));
    Gecko::SetActiveCodes(s_gecko_codes, s_game_id, static_cast<u16>(s_revision));
  }
  else
  {
    // Clear live codes before closing the session gate.
    Config::SetCurrent(Config::MAIN_ENABLE_CHEATS, true);
    ActionReplay::ApplyCodes({}, s_game_id, static_cast<u16>(s_revision));
    Gecko::SetActiveCodes({}, s_game_id, static_cast<u16>(s_revision));
    Config::SetCurrent(Config::MAIN_ENABLE_CHEATS, false);
  }
  s_status = s_cheats_enabled ? "Cheat engine enabled" : "Cheat engine disabled";
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

void ToggleCheatForSession(int flat_index)
{
  std::lock_guard lock{s_mutex};
  if (flat_index < 0)
    return;
  if (flat_index < static_cast<int>(s_ar_codes.size()))
  {
    s_ar_codes[flat_index].enabled = !s_ar_codes[flat_index].enabled;
    if (!SaveAndApplyCheats(true))
      s_ar_codes[flat_index].enabled = !s_ar_codes[flat_index].enabled;
    return;
  }
  flat_index -= static_cast<int>(s_ar_codes.size());
  if (flat_index >= static_cast<int>(s_gecko_codes.size()))
    return;
  s_gecko_codes[flat_index].enabled = !s_gecko_codes[flat_index].enabled;
  if (!SaveAndApplyCheats(false))
    s_gecko_codes[flat_index].enabled = !s_gecko_codes[flat_index].enabled;
}

void ToggleFPSForSession()
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized)
    return;
  s_show_fps = !s_show_fps;
  Config::SetCurrent(Config::GFX_SHOW_FPS, s_show_fps);
  s_status = s_show_fps ? "FPS overlay enabled" : "FPS overlay disabled";
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

void ToggleVBISkipForSession()
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized)
    return;
  s_vbi_skip = !s_vbi_skip;
  Config::SetCurrent(Config::GFX_HACK_VI_SKIP, s_vbi_skip);
  s_status = s_vbi_skip ? "VBI Skip enabled for this session - may cause freezes" :
                          "VBI Skip disabled for this session";
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
}

#ifdef __SWITCH2__
void SetCpuBoost(bool on, bool announce)
{
  std::lock_guard lock{s_mutex};
  s_cpu_boost = on;
  if (!announce)
    return;
  s_status = on ? "CPU Boost on for this session" : "CPU Boost off for this session";
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}
#endif

void ToggleFrameGenerationForSession()
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized)
    return;
  if (!DolphinSwitch::FrameGeneration::IsAvailable())
  {
    s_status = DolphinSwitch::FrameGeneration::GetStatus();
    s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    return;
  }

  const bool enabled = !DolphinSwitch::FrameGeneration::IsEnabled();
  if (!DolphinSwitch::FrameGeneration::RequestEnabled(enabled))
  {
    s_status = DolphinSwitch::FrameGeneration::GetStatus();
    s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    return;
  }
  s_status = enabled ? "Frame generation enabled for this session" :
                       "Frame generation disabled for this session";
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

void SetControllerModeResult(int player, ControllerMode mode, bool success)
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized || player < 0 || player >= static_cast<int>(s_controller_modes.size()))
    return;
  if (success)
    s_controller_modes[player] = mode;
  s_status = success ? "Player " + std::to_string(player + 1) + ": " +
                           std::string(ControllerModeName(mode)) :
                       "Controller mode could not be changed";
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

void DrawBanner(const std::string& text)
{
  const ImGuiIO& io = ImGui::GetIO();
  const float scale = OverlayScale(io);
  const float font_size = 26.0f * scale;
  const ImVec2 text_size = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str());
  const ImVec2 text_pos{(io.DisplaySize.x - text_size.x) * 0.5f, 24.0f * scale};
  const float padding = 14.0f * scale;
  ImDrawList* const draw = ImGui::GetForegroundDrawList();
  draw->AddRectFilled({text_pos.x - padding, text_pos.y - padding},
                      {text_pos.x + text_size.x + padding, text_pos.y + text_size.y + padding},
                      IM_COL32(7, 28, 44, 230), 12.0f * scale);
  draw->AddText(ImGui::GetFont(), font_size, text_pos, IM_COL32(255, 255, 255, 255), text.c_str());
}

void DrawGameplayBanner()
{
  std::lock_guard lock{s_mutex};
  if (s_calibration_active || s_banner.empty() ||
      std::chrono::steady_clock::now() >= s_banner_until)
  {
    return;
  }
  DrawBanner(s_banner);
}

void DrawTouchCalibration()
{
  std::lock_guard lock{s_mutex};
  if (!s_calibration_active || !g_presenter)
    return;

  const MathUtil::Rectangle<int> rect = g_presenter->GetTargetRectangle();
  const float left = static_cast<float>(std::min(rect.left, rect.right));
  const float top = static_cast<float>(std::min(rect.top, rect.bottom));
  const float width = static_cast<float>(std::abs(rect.right - rect.left));
  const float height = static_cast<float>(std::abs(rect.bottom - rect.top));
  if (width <= 1.0f || height <= 1.0f)
    return;

  const ImGuiIO& io = ImGui::GetIO();
  const float scale = OverlayScale(io);
  const ImVec2 center{left + s_calibration_x * width, top + s_calibration_y * height};
  const float radius = 26.0f * scale;
  ImDrawList* const draw = ImGui::GetForegroundDrawList();
  constexpr ImU32 shadow = IM_COL32(0, 0, 0, 200);
  constexpr ImU32 color = IM_COL32(255, 70, 70, 255);
  draw->AddCircle(center, radius, shadow, 48, 7.0f * scale);
  draw->AddCircle(center, radius, color, 48, 3.0f * scale);
  draw->AddLine({center.x - radius * 1.8f, center.y}, {center.x + radius * 1.8f, center.y}, color,
                3.0f * scale);
  draw->AddLine({center.x, center.y - radius * 1.8f}, {center.x, center.y + radius * 1.8f}, color,
                3.0f * scale);
  DrawBanner(s_calibration_text);
}

void Draw()
{
  GBAOverlay::Draw();
  // Advance once per final-present frame so pause waits for a clean game frame.
  s_render_frame_generation.fetch_add(1, std::memory_order_acq_rel);
  if (!s_visible.load(std::memory_order_acquire))
  {
    DrawTouchCalibration();
    DrawGameplayBanner();
    return;
  }
  std::lock_guard lock{s_mutex};
  if (!s_initialized || !s_visible.load(std::memory_order_relaxed))
    return;
  if (s_page == Page::ScreenLayout)
  {
    ScreenLayout::DrawEditor();
    return;
  }
  if (s_page == Page::Achievements &&
      std::chrono::steady_clock::now() >= s_achievement_refresh_at)
  {
    RefreshAchievements();
    s_selection = std::clamp(s_selection, 0, static_cast<int>(s_achievements.size()));
  }

  const ImGuiIO& io = ImGui::GetIO();
  ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, io.DisplaySize,
                                                IM_COL32(0, 0, 0, 132));
  const float scale = OverlayScale(io);
  s_ui_scale = scale;
  std::string title;
  switch (s_page)
  {
  case Page::Main:
    title = "Dolphin Quick Menu";
    break;
  case Page::SaveStates:
    title = "Save State";
    break;
  case Page::LoadStates:
    title = "Load State";
    break;
  case Page::Disc:
  case Page::DiscBrowser:
    title = "Disc Management";
    break;
  case Page::Cheats:
    title = "Action Replay & Gecko Codes";
    break;
  case Page::Achievements:
    title = "RetroAchievements";
    break;
  case Page::Controllers:
    title = "Controller Modes";
    break;
  case Page::ControllerModes:
    title = "Player " + std::to_string(s_controller_player + 1) + " Controller";
    break;
  case Page::Skylanders:
  case Page::SkylanderFigures:
    title = "Skylanders Portal";
    break;
  case Page::GBA:
    title = "Game Boy Advance";
    break;
  case Page::Alert:
  case Page::ScreenLayout:
    break;
  }

  // Laid out in 1080-line units: the title and text sizes, the window padding, a row with the spacing
  // below it, and the status line under the rows.
  constexpr float TITLE_SIZE = 30.0f;
  constexpr float TEXT_SIZE = 24.0f;
  constexpr float PADDING_X = 24.0f;
  constexpr float PADDING_Y = 22.0f;
  constexpr float ROW_STEP = 46.0f + 4.0f;
  constexpr float FOOTER = 8.0f + TEXT_SIZE + 4.0f;
  s_footer_height = std::round(FOOTER * scale);
  const float main_page_height =
      (PADDING_Y * 2 + TITLE_SIZE + 9.0f + MainPageRowCount() * ROW_STEP + FOOTER) * scale;
  const float height = std::min(io.DisplaySize.y - 40.0f * scale,
                                std::max(620.0f * scale, main_page_height));
  const float width = std::round(720.0f * scale);
  ImGui::SetNextWindowPos({std::floor((io.DisplaySize.x - width) * 0.5f),
                           std::floor((io.DisplaySize.y - height) * 0.5f)},
                          ImGuiCond_Always);
  ImGui::SetNextWindowSize({width, std::round(height)}, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.96f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      {std::round(PADDING_X * scale), std::round(PADDING_Y * scale)});
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10.0f * scale, std::round(4.0f * scale)});
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(7, 28, 44, 248));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(71, 190, 235, 150));
  ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(255, 255, 255, 34));
  ImGui::PushStyleColor(ImGuiCol_Text, TEXT_COLOR);
  ImGui::PushFont(nullptr, TEXT_SIZE * scale);

  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                     ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("Dolphin Switch quick menu", nullptr, flags))
  {
    if (!title.empty())
    {
      ImGui::PushFont(nullptr, TITLE_SIZE * scale);
      CenteredText(title);
      ImGui::PopFont();
      ImGui::Separator();
    }

    switch (s_page)
    {
    case Page::Main:
      RenderMainPage();
      break;
    case Page::SaveStates:
    case Page::LoadStates:
      RenderStatePage();
      break;
    case Page::Skylanders:
      RenderSkylandersPage();
      break;
    case Page::SkylanderFigures:
      RenderSkylanderFigures();
      break;
    case Page::GBA:
      RenderGBAPage();
      break;
    case Page::Disc:
      RenderDiscPage();
      break;
    case Page::DiscBrowser:
      RenderDiscBrowser();
      break;
    case Page::Cheats:
      RenderCheatsPage();
      break;
    case Page::Achievements:
      RenderAchievementsPage();
      break;
    case Page::Controllers:
      RenderControllersPage();
      break;
    case Page::ControllerModes:
      RenderControllerModesPage();
      break;
    case Page::Alert:
      RenderAlert();
      break;
    case Page::ScreenLayout:
      break;
    }

    // The result of the last action, on its own line at the bottom.
    if (!s_status.empty() && std::chrono::steady_clock::now() < s_status_until)
    {
      ImGui::SetCursorPosY(
          std::floor(ImGui::GetWindowHeight() - PADDING_Y * scale - ImGui::GetTextLineHeight()));
      ImGui::PushStyleColor(ImGuiCol_Text, ACCENT_COLOR);
      CenteredText(FitText(s_status, ImGui::GetWindowWidth() - 2.0f * PADDING_X * scale));
      ImGui::PopStyleColor();
    }
  }
  ImGui::End();
  ImGui::PopFont();
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(3);
}
}  // namespace DolphinSwitch::RuntimeOverlay
