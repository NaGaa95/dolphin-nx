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
#include <deque>
#include <cstring>
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
#include "Core/State.h"
#include "VideoBackends/Vulkan/LSFGControl.h"
#include "VideoCommon/OnScreenUI.h"

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
constexpr int MAIN_ITEM_COUNT = 12;
constexpr int DISC_ITEM_COUNT = 3;
constexpr int CONTROLLER_ITEM_COUNT = 5;
constexpr int CONTROLLER_MODE_ITEM_COUNT = 6;

std::mutex s_mutex;
std::atomic_bool s_visible{false};
std::atomic_bool s_capture_until_release{false};
std::atomic<std::uint64_t> s_render_frame_generation{0};
bool s_initialized = false;
bool s_user_paused = false;
bool s_show_fps = false;
bool s_is_wii = false;
Page s_page = Page::Main;
int s_selection = 0;
int s_controller_player = 0;
std::array<ControllerMode, 4> s_controller_modes{};
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
std::string s_status;
std::chrono::steady_clock::time_point s_status_until{};
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

std::string Shorten(std::string value, std::size_t limit)
{
  if (value.size() <= limit)
    return value;
  if (limit <= 3)
    return value.substr(0, limit);
  value.resize(limit - 3);
  value += "...";
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
  s_achievement_summary =
      std::to_string(summary.num_unlocked_achievements) + "/" +
      std::to_string(summary.num_core_achievements) + " achievements · " +
      std::to_string(summary.points_unlocked) + "/" + std::to_string(summary.points_core) +
      " points · " + (manager.IsHardcoreModeActive() ? "Hardcore" : "Softcore");
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

int ItemCount()
{
  switch (s_page)
  {
  case Page::Main:
    return MAIN_ITEM_COUNT;
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
  case Page::Alert:
    return 1;
  }
  return 1;
}

bool ItemAvailable(int index)
{
  if (s_page != Page::Main)
    return true;
  if (index == 3 && IsHardcoreActive())
    return false;
  if (index == 7 && !s_is_wii)
    return false;
  return index != 8 || Vulkan::LSFG::IsAvailable();
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
  case Page::Controllers:
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

void ActivateSelection()
{
  switch (s_page)
  {
  case Page::Main:
    switch (s_selection)
    {
    case 0:
      CloseMenu();
      break;
    case 1:
      QueueAction({ActionType::TogglePause});
      break;
    case 2:
      SetPage(Page::SaveStates);
      break;
    case 3:
      SetPage(Page::LoadStates);
      break;
    case 4:
      SetPage(Page::Disc);
      break;
    case 5:
      SetPage(Page::Cheats);
      break;
    case 6:
      SetPage(Page::Achievements);
      RefreshAchievements();
      break;
    case 7:
      if (s_is_wii)
        SetPage(Page::Controllers);
      break;
    case 8:
      QueueAction({ActionType::ToggleFrameGeneration});
      break;
    case 9:
      QueueAction({ActionType::ToggleFPS});
      break;
    case 10:
      QueueAction({ActionType::Reset});
      CloseMenu();
      break;
    case 11:
      QueueAction({ActionType::StopToLauncher});
      CloseMenu();
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
  case Page::Alert:
    CloseMenu();
    break;
  }
}

void CenteredText(std::string_view text)
{
  const float width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                (ImGui::GetWindowWidth() - width) * 0.5f));
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void SelectableRow(const std::string& label, int index, float height = 48.0f)
{
  const bool selected = s_selection == index;
  ImGui::Selectable((label + "##row" + std::to_string(index)).c_str(), selected,
                    ImGuiSelectableFlags_None, {0.0f, height});
  if (selected)
    ImGui::SetScrollHereY(0.5f);
}

void RenderMainPage()
{
  const bool hardcore = IsHardcoreActive();
  SelectableRow("Resume game", 0);
  SelectableRow(s_user_paused ? "Resume emulation" : "Pause emulation", 1);
  SelectableRow("Save state                                      >", 2);
  if (hardcore)
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 145, 158, 255));
  SelectableRow(hardcore ? "Load state                         Hardcore disabled" :
                           "Load state                                      >",
                3);
  if (hardcore)
    ImGui::PopStyleColor();
  SelectableRow("Disc management                                 >", 4);
  SelectableRow("Cheats                                          >", 5);
  SelectableRow(std::string("RetroAchievements                 ") + AchievementStatus() + "  >",
                6);
  if (!s_is_wii)
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 145, 158, 255));
  SelectableRow(s_is_wii ? "Controller modes                                >" :
                           "Controller modes                  Wii games only",
                7);
  if (!s_is_wii)
    ImGui::PopStyleColor();
  if (!Vulkan::LSFG::IsAvailable())
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 145, 158, 255));
  SelectableRow(std::string("Frame Generation                 <  ") +
                    (Vulkan::LSFG::IsAvailable() ?
                         (Vulkan::LSFG::IsEnabled() ?
                              (Vulkan::LSFG::IsHighFPSPassthrough() ?
                                   "On (native FPS protected)" :
                                   "On") :
                              "Off") :
                         "Disabled") +
                    "  >",
                8);
  if (!Vulkan::LSFG::IsAvailable())
    ImGui::PopStyleColor();
  SelectableRow(std::string("Show FPS                         <  ") +
                    (s_show_fps ? "Enabled" : "Disabled") + "  >",
                9);
  SelectableRow("Reset console", 10);
  SelectableRow("Return to Dolphin launcher", 11);
}

void RenderControllersPage()
{
  for (int player = 0; player < 4; ++player)
  {
    SelectableRow("Player " + std::to_string(player + 1) + "                     " +
                      std::string(ControllerModeName(s_controller_modes[player])) + "  >",
                  player, 58.0f);
  }
  SelectableRow("Back", CONTROLLER_ITEM_COUNT - 1, 58.0f);
  ImGui::Separator();
  CenteredText("Changes apply only to the current game session");
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
    SelectableRow(std::string(active ? "[Active]  " : "          ") +
                      std::string(ControllerModeName(modes[index])),
                  index, 54.0f);
  }
  SelectableRow("Back", CONTROLLER_MODE_ITEM_COUNT - 1, 54.0f);
  ImGui::Separator();
  CenteredText("A  Apply     B  Back");
}

void RenderStatePage(bool saving)
{
  const float child_height = std::max(220.0f, ImGui::GetContentRegionAvail().y - 58.0f);
  ImGui::BeginChild("state slots", {0.0f, child_height}, false, ImGuiWindowFlags_NoInputs);
  for (int slot = 0; slot < static_cast<int>(State::NUM_STATES); ++slot)
  {
    SelectableRow("Slot " + std::to_string(slot + 1) + "    " +
                      Shorten(s_state_info[slot], 66),
                  slot);
  }
  SelectableRow("Back", static_cast<int>(State::NUM_STATES));
  ImGui::EndChild();
  ImGui::Separator();
  CenteredText(saving ? "A  Save     B  Back" : "A  Load     B  Back");
}

void RenderDiscPage()
{
  SelectableRow("Eject current disc", 0, 64.0f);
  SelectableRow("Change disc from current game folder           >", 1, 64.0f);
  SelectableRow("Back", 2, 64.0f);
  ImGui::Separator();
  CenteredText("Disc images: ISO, GCM, TGC, WBFS, CISO, GCZ, WIA and RVZ");
}

void RenderDiscBrowser()
{
  ImGui::TextUnformatted(Shorten(s_browser_directory, 88).c_str());
  ImGui::Separator();
  const float child_height = std::max(200.0f, ImGui::GetContentRegionAvail().y - 58.0f);
  ImGui::BeginChild("disc files", {0.0f, child_height}, false, ImGuiWindowFlags_NoInputs);
  if (s_browser_entries.empty())
  {
    const std::string message =
        s_browser_scanning ? "Scanning folder..." :
                             (s_browser_error.empty() ? "No disc images in this folder" :
                                                        s_browser_error);
    SelectableRow(message, 0, 64.0f);
  }
  else
  {
    for (int index = 0; index < static_cast<int>(s_browser_entries.size()); ++index)
    {
      const BrowserEntry& entry = s_browser_entries[index];
      SelectableRow(std::string(entry.directory ? "[Folder]  " : "[Disc]    ") +
                        Shorten(entry.name, 70),
                    index);
    }
  }
  ImGui::EndChild();
  ImGui::Separator();
  CenteredText("A  Open / insert     B  Parent folder");
}

void RenderCheatsPage()
{
  const float child_height = std::max(220.0f, ImGui::GetContentRegionAvail().y - 58.0f);
  ImGui::BeginChild("cheat list", {0.0f, child_height}, false, ImGuiWindowFlags_NoInputs);
  SelectableRow(std::string("Cheat engine                 <  ") +
                    (s_cheats_enabled ? "Enabled" : "Disabled") + "  >",
                0);
  int row = 1;
  for (const ActionReplay::ARCode& code : s_ar_codes)
  {
    SelectableRow(std::string(code.enabled ? "[On]  AR     " : "[Off] AR     ") +
                      Shorten(code.name, 62),
                  row++);
  }
  for (const Gecko::GeckoCode& code : s_gecko_codes)
  {
    SelectableRow(std::string(code.enabled ? "[On]  Gecko  " : "[Off] Gecko  ") +
                      Shorten(code.name, 59),
                  row++);
  }
  SelectableRow("Back", row);
  ImGui::EndChild();
  ImGui::Separator();
  if (s_game_id.empty())
    CenteredText("Cheat lists are unavailable for this executable.");
  else if (s_ar_codes.empty() && s_gecko_codes.empty())
    CenteredText("No AR or Gecko codes are installed for this game.");
  else
    CenteredText("A  Toggle     B  Back");
}

void RenderAchievementsPage()
{
  ImGui::TextWrapped("%s", s_achievement_summary.c_str());
  if (!s_achievement_presence.empty())
    ImGui::TextColored({0.55f, 0.92f, 1.0f, 1.0f}, "%s", s_achievement_presence.c_str());
  ImGui::Separator();
  const float child_height = std::max(210.0f, ImGui::GetContentRegionAvail().y - 116.0f);
  ImGui::BeginChild("achievement list", {0.0f, child_height}, false,
                    ImGuiWindowFlags_NoInputs);
  int row = 0;
  for (const AchievementEntry& achievement : s_achievements)
  {
    SelectableRow(std::string(achievement.unlocked ? "[Unlocked] " : "[Locked]   ") +
                      Shorten(achievement.title, 58) + " · " +
                      std::to_string(achievement.points) + " pts",
                  row++);
  }
  SelectableRow("Back", row);
  ImGui::EndChild();
  ImGui::Separator();
  if (s_selection >= 0 && s_selection < static_cast<int>(s_achievements.size()))
  {
    const AchievementEntry& selected = s_achievements[s_selection];
    std::string detail = selected.description;
    if (!selected.unlocked && !selected.progress.empty())
      detail += (detail.empty() ? "" : " · ") + selected.progress;
    ImGui::TextWrapped("%s", detail.c_str());
  }
  else
  {
    CenteredText("B  Back");
  }
}

void RenderAlert()
{
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 205, 116, 255));
  CenteredText(s_alert_caption.empty() ? "Dolphin message" : s_alert_caption);
  ImGui::PopStyleColor();
  ImGui::Spacing();
  ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 30.0f);
  ImGui::TextWrapped("%s", s_alert_message.c_str());
  ImGui::PopTextWrapPos();
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 86.0f);
  ImGui::Separator();
  CenteredText("A / B  Close");
}
}  // namespace

void InitializeRendererHook()
{
  StartBrowserScanWorker();
  StartStateSaveMonitorWorker();
  State::SetOnAfterLoadCallback(&OnStateLoadCompleted);
  VideoCommon::SetImGuiRenderCallback(&Draw);
}

void ShutdownRendererHook()
{
  VideoCommon::SetImGuiRenderCallback(nullptr);
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
  if (s_game_id.empty())
  {
    s_game_id = SConfig::GetInstance().GetGameID();
    s_revision = SConfig::GetInstance().GetRevision();
  }
  s_visible.store(false, std::memory_order_release);
  s_capture_until_release.store(false, std::memory_order_release);
  s_render_frame_generation.store(0, std::memory_order_release);
  s_page = Page::Main;
  s_selection = 0;
  s_actions.clear();
  s_user_paused = false;
  s_show_fps = Config::Get(Config::GFX_SHOW_FPS);
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
}

void UpdateInput(std::uint64_t down, std::uint64_t held)
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized)
    return;

  if (s_capture_until_release.load(std::memory_order_relaxed))
  {
    if (held != 0)
      return;
    s_capture_until_release.store(false, std::memory_order_release);
  }

  if ((held & MENU_CHORD) == MENU_CHORD && (down & MENU_CHORD) != 0)
  {
    if (s_visible.load(std::memory_order_relaxed))
      CloseMenu();
    else
      OpenMenu();
    return;
  }

  if (!s_visible.load(std::memory_order_acquire))
    return;
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

void SetUserPaused(bool paused)
{
  std::lock_guard lock{s_mutex};
  s_user_paused = paused;
}

void SetStatus(std::string message)
{
  std::lock_guard lock{s_mutex};
  s_status = std::move(message);
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
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

void ToggleFrameGenerationForSession()
{
  std::lock_guard lock{s_mutex};
  if (!s_initialized)
    return;
  if (!Vulkan::LSFG::IsAvailable())
  {
    s_status = Vulkan::LSFG::GetStatus();
    s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    return;
  }

  const bool enabled = !Vulkan::LSFG::IsEnabled();
  if (!Vulkan::LSFG::RequestEnabled(enabled))
  {
    s_status = Vulkan::LSFG::GetStatus();
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

void Draw()
{
  // Advance once per final-present frame so pause waits for a clean game frame.
  s_render_frame_generation.fetch_add(1, std::memory_order_acq_rel);
  if (!s_visible.load(std::memory_order_acquire))
    return;
  std::lock_guard lock{s_mutex};
  if (!s_initialized || !s_visible.load(std::memory_order_relaxed))
    return;
  if (s_page == Page::Achievements &&
      std::chrono::steady_clock::now() >= s_achievement_refresh_at)
  {
    RefreshAchievements();
    s_selection = std::clamp(s_selection, 0, static_cast<int>(s_achievements.size()));
  }

  const ImGuiIO& io = ImGui::GetIO();
  ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, io.DisplaySize,
                                                IM_COL32(0, 0, 0, 132));
  const float scale = std::clamp(io.DisplaySize.y / 1080.0f, 0.72f, 1.0f);
  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always,
                          {0.5f, 0.5f});
  ImGui::SetNextWindowSize({880.0f * scale, 760.0f * scale}, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.96f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {30.0f * scale, 22.0f * scale});
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10.0f * scale, 8.0f * scale});
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(7, 28, 44, 248));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(71, 190, 235, 255));
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(15, 89, 126, 245));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(15, 89, 126, 245));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(15, 89, 126, 245));
  ImGui::PushFont(nullptr, 25.0f * scale);

  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                     ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("Dolphin Switch quick menu", nullptr, flags))
  {
    switch (s_page)
    {
    case Page::Main:
      CenteredText("Dolphin Quick Menu");
      break;
    case Page::SaveStates:
      CenteredText("Save State");
      break;
    case Page::LoadStates:
      CenteredText("Load State");
      break;
    case Page::Disc:
    case Page::DiscBrowser:
      CenteredText("Disc Management");
      break;
    case Page::Cheats:
      CenteredText("Action Replay & Gecko Codes");
      break;
    case Page::Achievements:
      CenteredText("RetroAchievements");
      break;
    case Page::Controllers:
      CenteredText("Controller Modes");
      break;
    case Page::ControllerModes:
      CenteredText(("Player " + std::to_string(s_controller_player + 1) + " Controller").c_str());
      break;
    case Page::Alert:
      break;
    }
    ImGui::Separator();

    switch (s_page)
    {
    case Page::Main:
      RenderMainPage();
      ImGui::Separator();
      CenteredText("D-Pad  Navigate     A  Select     B  Close     L + R + Plus  Toggle");
      break;
    case Page::SaveStates:
      RenderStatePage(true);
      break;
    case Page::LoadStates:
      RenderStatePage(false);
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
    }

    if (!s_status.empty() && std::chrono::steady_clock::now() < s_status_until)
    {
      ImGui::SetCursorPosY(18.0f * scale);
      ImGui::SetCursorPosX(20.0f * scale);
      ImGui::TextColored({0.55f, 0.92f, 1.0f, 1.0f}, "%s", s_status.c_str());
    }
  }
  ImGui::End();
  ImGui::PopFont();
  ImGui::PopStyleColor(5);
  ImGui::PopStyleVar(3);
}
}  // namespace DolphinSwitch::RuntimeOverlay
