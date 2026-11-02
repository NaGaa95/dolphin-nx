// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/ScreenLayout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <locale>
#include <mutex>
#include <sstream>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <switch.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "InputCommon/ControllerInterface/Switch/Switch.h"

namespace DolphinSwitch::ScreenLayout
{
namespace
{
constexpr std::size_t SCREEN_COUNT = 5;

struct Placement
{
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool operator==(const Placement&) const = default;
};

using Layout = std::array<Placement, SCREEN_COUNT>;

Layout Parse(std::string_view text)
{
  Layout layout{};
  if (text.empty())
    return layout;
  std::istringstream stream{std::string(text)};
  stream.imbue(std::locale::classic());
  int version;
  if (!(stream >> version) || (version != 1 && version != 2))
    return {};
  for (Placement& placement : layout)
  {
    if (!(stream >> placement.x >> placement.y >> placement.width) ||
        (version == 2 && !(stream >> placement.height)) || placement.x < 0 || placement.x > 10000 ||
        placement.y < 0 || placement.y > 10000 || placement.width < 0 || placement.width > 10000 ||
        placement.height < 0 || placement.height > 10000 ||
        (placement.width == 0 && placement.height != 0))
    {
      return {};
    }
  }
  stream >> std::ws;
  return stream.eof() ? layout : Layout{};
}

std::string Serialize(const Layout& layout)
{
  if (layout == Layout{})
    return {};
  std::string text = "2";
  for (const Placement& placement : layout)
  {
    text += " " + std::to_string(placement.x) + " " + std::to_string(placement.y) + " " +
            std::to_string(placement.width) + " " + std::to_string(placement.height);
  }
  return text;
}

MathUtil::Rectangle<float> Resolve(Placement placement, float aspect, float width, float height)
{
  if (!(width > 0 && height > 0 && aspect > 0) || !std::isfinite(aspect))
    return {};
  const bool locked = placement.height == 0;
  const float maximum = locked ? std::min(width, height * aspect) : width;
  const float screen_width =
      std::clamp(width * placement.width / 10000.0f, maximum * 0.05f, maximum);
  const float screen_height =
      locked ? screen_width / aspect :
               std::clamp(height * placement.height / 10000.0f, height * 0.05f, height);
  const float x =
      std::clamp(width * placement.x / 10000.0f, 0.0f, std::max(0.0f, width - screen_width));
  const float y =
      std::clamp(height * placement.y / 10000.0f, 0.0f, std::max(0.0f, height - screen_height));
  return {x, y, x + screen_width, y + screen_height};
}

using Rectangle = MathUtil::Rectangle<float>;
std::mutex s_mutex;
Layout s_layout{};
Layout s_before_edit{};
std::array<Rectangle, SCREEN_COUNT> s_defaults{};
std::array<Rectangle, SCREEN_COUNT> s_rectangles{};
std::array<bool, SCREEN_COUNT> s_available{};
std::string s_config;
std::string s_game_ini;
std::string s_error;
float s_width = 1280;
float s_height = 720;
bool s_editing = false;
std::size_t s_selected = 0;
bool s_touch_down = false;
bool s_resizing = false;
bool s_touch_pressed = false;
bool s_aspect_lock = true;
float s_touch_x = 0;
float s_touch_y = 0;
Rectangle s_touch_start{};
std::chrono::steady_clock::time_point s_last_input{};

Rectangle EditorCanvas()
{
  const float scale = s_height / 720;
  const float zoom =
      std::min((s_width - 48 * scale) / s_width, (s_height - 224 * scale) / s_height);
  const float width = s_width * zoom;
  const float height = s_height * zoom;
  const float left = (s_width - width) * 0.5f;
  const float top = 104 * scale;
  return {left, top, left + width, top + height};
}

Rectangle ToCanvas(const Rectangle& rectangle)
{
  const auto canvas = EditorCanvas();
  const float zoom = canvas.GetWidth() / s_width;
  return {canvas.left + rectangle.left * zoom, canvas.top + rectangle.top * zoom,
          canvas.left + rectangle.right * zoom, canvas.top + rectangle.bottom * zoom};
}

std::string ScreenName(std::size_t index)
{
  return index == 0 ? "Game screen" : "GBA " + std::to_string(index);
}

enum class EditorAction
{
  Select,
  Aspect,
  Align,
  Reset,
  ResetAll,
  Cancel,
  Save
};
struct EditorButton
{
  Rectangle rectangle;
  EditorAction action;
  std::size_t index;
  std::string label;
};

std::vector<EditorButton> EditorButtons()
{
  const float scale = s_height / 720;
  std::vector<EditorButton> buttons;
  float x = 24 * scale;
  for (std::size_t index = 0; index < SCREEN_COUNT; ++index)
  {
    if (!s_available[index])
      continue;
    buttons.push_back({{x, 52 * scale, x + 148 * scale, 90 * scale},
                       EditorAction::Select,
                       index,
                       ScreenName(index)});
    x += 160 * scale;
  }
  x = 24 * scale;
  const float top = s_height - 100 * scale;
  const auto add = [&](EditorAction action, const std::string& label, float width) {
    buttons.push_back({{x, top, x + width * scale, top + 44 * scale}, action, 0, label});
    x += (width + 12) * scale;
  };
  add(EditorAction::Align, "X  Align", 144);
  add(EditorAction::Reset, "Y  Reset", 144);
  add(EditorAction::ResetAll, "-  Reset all", 164);
  add(EditorAction::Aspect, s_aspect_lock ? "R3  Aspect: locked" : "R3  Aspect: free", 240);
  add(EditorAction::Cancel, "B  Cancel", 164);
  add(EditorAction::Save, "+  Save game", 188);
  return buttons;
}

void SelectScreen(std::size_t index)
{
  s_selected = index;
  s_aspect_lock = s_layout[index].height == 0;
  s_touch_down = false;
}

Rectangle Apply(std::size_t index, const Rectangle& rectangle, float width, float height)
{
  if (width <= 0 || height <= 0 || rectangle.GetWidth() <= 0 || rectangle.GetHeight() <= 0)
    return rectangle;
  std::lock_guard lock(s_mutex);
  if (!s_editing)
  {
    const std::string config = Config::Get(Config::GFX_SWITCH_SCREEN_LAYOUT);
    if (config != s_config)
    {
      s_config = config;
      s_layout = Parse(config);
    }
  }
  s_width = width;
  s_height = height;
  s_defaults[index] = rectangle;
  s_rectangles[index] =
      s_layout[index].width == 0 ?
          rectangle :
          Resolve(s_layout[index], rectangle.GetWidth() / rectangle.GetHeight(), width, height);
  return s_editing ? ToCanvas(s_rectangles[index]) : s_rectangles[index];
}

void SetRectangle(Rectangle rectangle, bool snap_to_edges = false)
{
  if (s_defaults[s_selected].GetHeight() <= 0 || s_width <= 0 || s_height <= 0)
    return;
  const float aspect = s_defaults[s_selected].GetWidth() / s_defaults[s_selected].GetHeight();
  Placement placement{
      static_cast<int>(std::lround(rectangle.left * 10000 / s_width)),
      static_cast<int>(std::lround(rectangle.top * 10000 / s_height)),
      static_cast<int>(
          std::lround(std::max(0.0f, rectangle.right - rectangle.left) * 10000 / s_width)),
      s_aspect_lock ?
          0 :
          std::max(1, static_cast<int>(std::lround(rectangle.GetHeight() * 10000 / s_height)))};
  rectangle = Resolve(placement, aspect, s_width, s_height);
  const float snap = snap_to_edges ? 6.0f * s_height / 720 : 0.0f;
  float x = rectangle.left;
  float y = rectangle.top;
  if (x < snap)
    x = 0;
  else if (s_width - rectangle.right < snap)
    x = s_width - rectangle.GetWidth();
  if (y < snap)
    y = 0;
  else if (s_height - rectangle.bottom < snap)
    y = s_height - rectangle.GetHeight();
  s_layout[s_selected] = {
      static_cast<int>(std::lround(x * 10000 / s_width)),
      static_cast<int>(std::lround(y * 10000 / s_height)),
      static_cast<int>(std::lround(rectangle.GetWidth() * 10000 / s_width)),
      s_aspect_lock ? 0 : static_cast<int>(std::lround(rectangle.GetHeight() * 10000 / s_height))};
  s_rectangles[s_selected] = Resolve(s_layout[s_selected], aspect, s_width, s_height);
}

bool Save()
{
  Common::IniFile ini;
  if (s_game_ini.empty() || (File::Exists(s_game_ini) && !ini.Load(s_game_ini)) ||
      !File::CreateFullPath(s_game_ini))
    return false;
  const std::string value = Serialize(s_layout);
  ini.GetOrCreateSection("Video_Settings")->Set("SwitchScreenLayout", value);
  if (!ini.Save(s_game_ini))
    return false;
  Config::Set(Config::LayerType::LocalGame, Config::GFX_SWITCH_SCREEN_LAYOUT, value);
  s_config = value;
  s_editing = false;
  return true;
}
}  // namespace

MathUtil::Rectangle<int> ApplyMain(const MathUtil::Rectangle<int>& rectangle, int width, int height)
{
  const Rectangle result =
      Apply(0,
            {static_cast<float>(rectangle.left), static_cast<float>(rectangle.top),
             static_cast<float>(rectangle.right), static_cast<float>(rectangle.bottom)},
            width, height);
  return {static_cast<int>(std::lround(result.left)), static_cast<int>(std::lround(result.top)),
          static_cast<int>(std::lround(result.right)),
          static_cast<int>(std::lround(result.bottom))};
}

Rectangle ApplyGBA(std::size_t port, const Rectangle& rectangle, float width, float height)
{
  return port < 4 ? Apply(port + 1, rectangle, width, height) : rectangle;
}

void BeginSession(std::string game_id, unsigned revision)
{
  std::lock_guard lock(s_mutex);
  s_editing = false;
  s_game_ini = ConfigLoaders::GetLocalGameConfigOverridePath();
  if (s_game_ini.empty() && !game_id.empty())
  {
    const std::string base = File::GetUserPath(D_GAMESETTINGS_IDX) + game_id;
    const std::string specific = base + "r" + std::to_string(revision) + ".ini";
    s_game_ini = File::Exists(specific) ? specific : base + ".ini";
  }
  s_config = Config::Get(Config::GFX_SWITCH_SCREEN_LAYOUT);
  s_layout = Parse(s_config);
}

void EndSession()
{
  std::lock_guard lock(s_mutex);
  s_editing = false;
  s_layout = {};
  s_config.clear();
  s_game_ini.clear();
  s_defaults = {};
  s_rectangles = {};
}

void BeginEdit(const std::array<bool, 4>& gba_ports)
{
  std::lock_guard lock(s_mutex);
  s_before_edit = s_layout;
  s_available[0] = true;
  for (std::size_t port = 0; port < gba_ports.size(); ++port)
    s_available[port + 1] =
        gba_ports[port] &&
        Config::Get(Config::MAIN_SWITCH_GBA_SCREENS) != Config::GBAScreens::Hidden &&
        s_defaults[port + 1].GetHeight() > 0;
  SelectScreen(0);
  s_touch_pressed = false;
  s_error.clear();
  s_editing = true;
  s_last_input = std::chrono::steady_clock::now();
}

void CancelEdit()
{
  std::lock_guard lock(s_mutex);
  if (s_editing)
    s_layout = s_before_edit;
  s_editing = false;
}

bool UpdateEditor(std::uint64_t down, std::uint64_t held, float move_x, float move_y, float size_x,
                  float size_y)
{
  std::lock_guard lock(s_mutex);
  const auto touch = ciface::Switch::GetTouchscreenState();
  const bool pressed = touch.pressed && !s_touch_pressed;
  s_touch_pressed = touch.pressed;
  const float touch_x = touch.x * s_width / 1280;
  const float touch_y = touch.y * s_height / 720;
  if (pressed)
  {
    for (const auto& button : EditorButtons())
    {
      const auto& r = button.rectangle;
      if (touch_x < r.left || touch_x > r.right || touch_y < r.top || touch_y > r.bottom)
        continue;
      switch (button.action)
      {
      case EditorAction::Select:
        SelectScreen(button.index);
        break;
      case EditorAction::Aspect:
        down |= HidNpadButton_StickR;
        break;
      case EditorAction::Align:
        down |= HidNpadButton_X;
        break;
      case EditorAction::Reset:
        down |= HidNpadButton_Y;
        break;
      case EditorAction::ResetAll:
        down |= HidNpadButton_Minus;
        break;
      case EditorAction::Cancel:
        return true;
      case EditorAction::Save:
        down |= HidNpadButton_Plus;
        break;
      }
      s_touch_down = false;
      break;
    }
  }
  if (down & HidNpadButton_Plus)
  {
    if (Save())
      return true;
    s_error = "Could not save the game layout. Choose Save to retry, or Cancel.";
  }
  if (down & (HidNpadButton_L | HidNpadButton_R))
  {
    std::size_t selected = s_selected;
    do
      selected = (selected + ((down & HidNpadButton_L) ? SCREEN_COUNT - 1 : 1)) % SCREEN_COUNT;
    while (!s_available[selected]);
    SelectScreen(selected);
  }
  if (down & HidNpadButton_Minus)
  {
    s_layout = {};
    s_rectangles = s_defaults;
    SelectScreen(s_selected);
  }
  if (down & HidNpadButton_Y)
  {
    s_layout[s_selected] = {};
    s_rectangles[s_selected] = s_defaults[s_selected];
    SelectScreen(s_selected);
  }
  Rectangle rectangle = s_rectangles[s_selected];
  if (rectangle.GetHeight() <= 0)
    return false;
  if (down & HidNpadButton_StickR)
  {
    s_aspect_lock = !s_aspect_lock;
    SetRectangle(rectangle);
  }
  if (down & HidNpadButton_X)
  {
    const float remaining = std::max(0.0f, s_width - rectangle.GetWidth());
    const float next = rectangle.left < remaining * 0.25f ? remaining * 0.5f :
                       rectangle.left < remaining * 0.75f ? remaining :
                                                            0.0f;
    rectangle.right = next + rectangle.GetWidth();
    rectangle.left = next;
    SetRectangle(rectangle);
  }
  const auto now = std::chrono::steady_clock::now();
  const float dt =
      std::clamp(std::chrono::duration<float>(now - s_last_input).count(), 0.0f, 0.05f);
  s_last_input = now;
  const float dx =
      move_x + ((held & HidNpadButton_Right) != 0) - ((held & HidNpadButton_Left) != 0);
  const float dy = move_y + ((held & HidNpadButton_Down) != 0) - ((held & HidNpadButton_Up) != 0);
  const float resize = ((held & HidNpadButton_ZR) != 0) - ((held & HidNpadButton_ZL) != 0);
  if (dx || dy || resize || size_x || size_y)
  {
    rectangle = s_rectangles[s_selected];
    const float aspect = rectangle.GetWidth() / rectangle.GetHeight();
    float dw = (resize + size_x) * s_width * dt * 0.3f;
    float dh = size_y * s_height * dt * 0.3f;
    if (s_aspect_lock)
    {
      if (std::abs(dh * aspect) > std::abs(dw))
        dw = dh * aspect;
      dh = dw / aspect;
    }
    rectangle.left += dx * s_width * dt * 0.3f - dw * 0.5f;
    rectangle.top += dy * s_height * dt * 0.3f - dh * 0.5f;
    rectangle.right += dx * s_width * dt * 0.3f + dw * 0.5f;
    rectangle.bottom += dy * s_height * dt * 0.3f + dh * 0.5f;
    SetRectangle(rectangle);
  }
  const auto canvas = EditorCanvas();
  const float zoom = canvas.GetWidth() / s_width;
  const float x = (touch_x - canvas.left) / zoom;
  const float y = (touch_y - canvas.top) / zoom;
  if (pressed && touch_x >= canvas.left && touch_x <= canvas.right && touch_y >= canvas.top &&
      touch_y <= canvas.bottom)
  {
    for (std::size_t index = SCREEN_COUNT; index-- > 0;)
    {
      const auto& candidate = s_rectangles[index];
      if (!s_available[index] || x < candidate.left || x > candidate.right || y < candidate.top ||
          y > candidate.bottom)
        continue;
      SelectScreen(index);
      s_touch_x = x;
      s_touch_y = y;
      s_touch_start = candidate;
      const float handle = 28 * s_height / (720 * zoom);
      s_resizing = x > candidate.right - handle && y > candidate.bottom - handle;
      s_touch_down = true;
      break;
    }
  }
  else if (touch.pressed && s_touch_down)
  {
    rectangle = s_touch_start;
    if (s_resizing)
    {
      if (s_aspect_lock)
      {
        const float aspect = rectangle.GetWidth() / rectangle.GetHeight();
        const float change = ((x - s_touch_x) + (y - s_touch_y) * aspect) * 0.5f;
        rectangle.right += change;
        rectangle.bottom += change / aspect;
      }
      else
      {
        rectangle.right += x - s_touch_x;
        rectangle.bottom += y - s_touch_y;
      }
    }
    else
    {
      rectangle.left += x - s_touch_x;
      rectangle.right += x - s_touch_x;
      rectangle.top += y - s_touch_y;
      rectangle.bottom += y - s_touch_y;
    }
    SetRectangle(rectangle, true);
  }
  if (!touch.pressed)
    s_touch_down = false;
  return false;
}

void DrawEditor()
{
  std::lock_guard lock(s_mutex);
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  const float scale = s_height / 720;
  const auto canvas = EditorCanvas();
  draw->AddRect({canvas.left, canvas.top}, {canvas.right, canvas.bottom},
                IM_COL32(90, 120, 140, 220), 0, 0, 2 * scale);
  for (int line = 1; line < 4; ++line)
  {
    const float x = canvas.left + canvas.GetWidth() * line / 4;
    const float y = canvas.top + canvas.GetHeight() * line / 4;
    draw->AddLine({x, canvas.top}, {x, canvas.bottom}, IM_COL32(190, 215, 230, 35));
    draw->AddLine({canvas.left, y}, {canvas.right, y}, IM_COL32(190, 215, 230, 35));
  }
  for (std::size_t index = 0; index < SCREEN_COUNT; ++index)
  {
    if (!s_available[index])
      continue;
    const auto rect = ToCanvas(s_rectangles[index]);
    if (rect.GetHeight() <= 0)
      continue;
    const ImU32 color =
        index == s_selected ? IM_COL32(100, 225, 255, 255) : IM_COL32(255, 255, 255, 180);
    draw->AddRect({rect.left, rect.top}, {rect.right, rect.bottom}, color, 0, 0, 3 * scale);
    draw->AddRectFilled({rect.left, rect.top},
                        {std::min(rect.right, rect.left + 144 * scale),
                         std::min(rect.bottom, rect.top + 32 * scale)},
                        IM_COL32(7, 28, 44, 220));
    const auto label = ScreenName(index);
    draw->AddText(nullptr, 20 * scale, {rect.left + 6 * scale, rect.top + 4 * scale}, color,
                  label.c_str());
    if (index == s_selected)
      draw->AddRectFilled({rect.right - std::min(rect.GetWidth(), 26 * scale),
                           rect.bottom - std::min(rect.GetHeight(), 26 * scale)},
                          {rect.right, rect.bottom}, color, 4 * scale);
  }
  draw->AddRectFilled({0, 0}, {s_width, 98 * scale}, IM_COL32(7, 28, 44, 250));
  draw->AddRectFilled({0, s_height - 116 * scale}, {s_width, s_height}, IM_COL32(7, 28, 44, 250));
  draw->AddText(nullptr, 25 * scale, {24 * scale, 14 * scale}, IM_COL32_WHITE,
                "Custom screen layout");
  const auto& selected = s_rectangles[s_selected];
  const std::string dimensions =
      ScreenName(s_selected) + "  " +
      std::to_string(static_cast<int>(std::lround(selected.GetWidth()))) + " x " +
      std::to_string(static_cast<int>(std::lround(selected.GetHeight()))) + "    X " +
      std::to_string(static_cast<int>(std::lround(selected.left))) + "  Y " +
      std::to_string(static_cast<int>(std::lround(selected.top)));
  draw->AddText(nullptr, 19 * scale, {540 * scale, 20 * scale}, IM_COL32(170, 205, 225, 255),
                dimensions.c_str());
  for (const auto& button : EditorButtons())
  {
    const auto& r = button.rectangle;
    const bool button_selected =
        button.action == EditorAction::Select && button.index == s_selected;
    const ImU32 color = button_selected || button.action == EditorAction::Save ?
                            IM_COL32(20, 112, 148, 255) :
                            IM_COL32(26, 54, 72, 255);
    draw->AddRectFilled({r.left, r.top}, {r.right, r.bottom}, color, 6 * scale);
    draw->AddText(nullptr, 19 * scale, {r.left + 12 * scale, r.top + 8 * scale}, IM_COL32_WHITE,
                  button.label.c_str());
  }
  draw->AddText(nullptr, 18 * scale, {24 * scale, s_height - 38 * scale},
                IM_COL32(190, 215, 230, 255),
                s_error.empty() ? "Touch: drag / resize     Left stick or D-Pad: move     Right "
                                  "stick or ZL / ZR: size     L / R: screen" :
                                  s_error.c_str());
}
}  // namespace DolphinSwitch::ScreenLayout
