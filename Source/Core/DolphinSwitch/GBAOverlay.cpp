// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/GBAOverlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include <imgui.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/GBACore.h"
#include "Core/Host.h"
#include "DolphinSwitch/ScreenLayout.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/TextureConfig.h"

namespace DolphinSwitch::GBAOverlay
{
namespace
{
constexpr size_t SLOT_COUNT = 4;
constexpr float REFERENCE_HEIGHT = 720.0f;
constexpr float MARGIN = 8.0f;

struct Slot
{
  std::mutex mutex;
  bool active = false;
  bool dirty = false;
  u32 width = 0;
  u32 height = 0;
  std::vector<u32> pixels;

  std::unique_ptr<AbstractTexture> texture;
  std::vector<u32> upload;
};

std::array<Slot, SLOT_COUNT> s_slots;
std::atomic_int s_guest_port{-1};

class Host final : public GBAHostInterface
{
public:
  explicit Host(std::weak_ptr<HW::GBA::Core> core) : m_core(std::move(core))
  {
    const auto core_ptr = m_core.lock();
    m_slot = static_cast<size_t>(core_ptr->GetCoreInfo().device_number);

    Slot& slot = s_slots[m_slot];
    std::lock_guard lock(slot.mutex);
    slot.active = true;
    slot.dirty = false;
    slot.pixels.clear();
    SetSize(slot, core_ptr->GetCoreInfo());
  }

  ~Host() override
  {
    Slot& slot = s_slots[m_slot];
    std::lock_guard lock(slot.mutex);
    slot.active = false;
    slot.dirty = false;
  }

  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  void GameChanged() override
  {
    const auto core_ptr = m_core.lock();
    if (!core_ptr || !core_ptr->IsStarted())
      return;

    Slot& slot = s_slots[m_slot];
    std::lock_guard lock(slot.mutex);
    SetSize(slot, core_ptr->GetCoreInfo());
  }

  void FrameEnded(std::span<const u32> video_buffer) override
  {
    Slot& slot = s_slots[m_slot];
    std::lock_guard lock(slot.mutex);
    if (video_buffer.size() != size_t{slot.width} * slot.height)
      return;

    slot.pixels.assign(video_buffer.begin(), video_buffer.end());
    slot.dirty = true;
  }

private:
  static void SetSize(Slot& slot, const HW::GBA::CoreInfo& info)
  {
    if (slot.width != info.width || slot.height != info.height)
    {
      slot.pixels.clear();
      slot.dirty = false;
    }
    slot.width = info.width;
    slot.height = info.height;
  }

  std::weak_ptr<HW::GBA::Core> m_core;
  size_t m_slot = 0;
};

AbstractTexture* UpdateTexture(Slot& slot)
{
  u32 width;
  u32 height;
  {
    std::lock_guard lock(slot.mutex);
    if (!slot.active)
    {
      slot.texture.reset();
      return nullptr;
    }

    width = slot.width;
    height = slot.height;
    if (slot.dirty)
    {
      std::swap(slot.upload, slot.pixels);
      slot.dirty = false;
    }
    else
    {
      slot.upload.clear();
    }
  }

  if (!slot.upload.empty())
  {
    if (!slot.texture || slot.texture->GetWidth() != width || slot.texture->GetHeight() != height)
    {
      const TextureConfig config(width, height, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                                 AbstractTextureType::Texture_2DArray);
      slot.texture = g_gfx->CreateTexture(config, "GBA screen");
    }

    if (slot.texture)
    {
      for (u32& pixel : slot.upload)
        pixel |= 0xFF000000;

      slot.texture->Load(0, width, height, width, reinterpret_cast<const u8*>(slot.upload.data()),
                         slot.upload.size() * sizeof(u32));
    }
  }

  return slot.texture.get();
}
}

std::unique_ptr<GBAHostInterface> CreateHost(std::weak_ptr<HW::GBA::Core> core)
{
  const auto instance = core.lock();
  if (!instance || instance->GetCoreInfo().device_number < 0 ||
      static_cast<size_t>(instance->GetCoreInfo().device_number) >= SLOT_COUNT)
    return {};
  return std::make_unique<Host>(std::move(core));
}

void Draw()
{
  std::array<AbstractTexture*, SLOT_COUNT> textures{};
  for (size_t i = 0; i < SLOT_COUNT; ++i)
    textures[i] = UpdateTexture(s_slots[i]);

  const Config::GBAScreens screens = Config::Get(Config::MAIN_SWITCH_GBA_SCREENS);
  if (screens == Config::GBAScreens::Hidden)
    return;

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float scale =
      (display.y / REFERENCE_HEIGHT) * (screens == Config::GBAScreens::Large ? 2.0f : 1.0f);
  const float margin = MARGIN * display.y / REFERENCE_HEIGHT;

  const Config::GBAScreenCorner corner = Config::Get(Config::MAIN_SWITCH_GBA_SCREEN_CORNER);
  const bool left =
      corner == Config::GBAScreenCorner::TopLeft || corner == Config::GBAScreenCorner::BottomLeft;
  const bool bottom = corner == Config::GBAScreenCorner::BottomLeft ||
                      corner == Config::GBAScreenCorner::BottomRight;

  ImDrawList* const draw = ImGui::GetBackgroundDrawList();
  float column_x = margin;
  float offset = margin;
  float column_width = 0.0f;
  for (size_t port = 0; port < SLOT_COUNT; ++port)
  {
    AbstractTexture* texture = textures[port];
    if (!texture || static_cast<int>(port) == s_guest_port.load(std::memory_order_relaxed))
      continue;

    const float width = texture->GetWidth() * scale;
    const float height = texture->GetHeight() * scale;
    if (offset > margin && offset + height > display.y - margin)
    {
      column_x += column_width + margin;
      offset = margin;
      column_width = 0.0f;
    }

    const float x = left ? column_x : display.x - column_x - width;
    const float y = bottom ? display.y - offset - height : offset;
    const auto rectangle =
        ScreenLayout::ApplyGBA(port, {x, y, x + width, y + height}, display.x, display.y);
    draw->AddImage(*texture, ImVec2(rectangle.left, rectangle.top),
                   ImVec2(rectangle.right, rectangle.bottom));
    offset += height + margin;
    column_width = std::max(column_width, width);
  }
}

void ReleaseTextures()
{
  for (Slot& slot : s_slots)
    slot.texture.reset();
}

void SetGuestPort(int port)
{
  s_guest_port.store(port, std::memory_order_relaxed);
}

const AbstractTexture* GetTexture(int port)
{
  if (port < 0 || static_cast<size_t>(port) >= SLOT_COUNT)
    return nullptr;
  // (the presenter's thread, which uploads them in Draw)
  return s_slots[port].texture.get();
}
}
