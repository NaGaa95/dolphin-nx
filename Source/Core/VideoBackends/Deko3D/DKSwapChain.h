// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <optional>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

#include "VideoCommon/TextureConfig.h"

namespace Deko3D
{
class DKFramebuffer;
class DKTexture;

class DKSwapChain
{
public:
  static constexpr u32 NUM_IMAGES = 2;
  static constexpr DkImageFormat FORMAT = DkImageFormat_RGBA8_Unorm;

  ~DKSwapChain();

  // guest: GameShare's guest screen, a blit destination paced by its own window
  static std::unique_ptr<DKSwapChain> Create(void* native_window, bool guest = false);

  u32 GetWidth() const { return m_width; }
  u32 GetHeight() const { return m_height; }
  const dk::Image& GetImage(int slot) const { return m_images[slot]; }

  DKFramebuffer* GetFramebuffer(int slot) const { return m_framebuffers[slot].get(); }

  static AbstractTextureFormat GetTextureFormat() { return AbstractTextureFormat::RGBA8; }

  int Acquire();
  void Present(int slot);
  void SetVSync(bool enabled);

  bool Recreate();

private:
  bool Initialize();
  void Destroy();

  void* m_native_window = nullptr;
  bool m_guest = false;
  u32 m_width = 0;
  u32 m_height = 0;

  dk::UniqueMemBlock m_image_memblock;
  std::array<dk::Image, NUM_IMAGES> m_images;
  std::array<std::unique_ptr<DKTexture>, NUM_IMAGES> m_textures;
  std::array<std::unique_ptr<DKFramebuffer>, NUM_IMAGES> m_framebuffers;
  dk::UniqueSwapchain m_swapchain;
  std::optional<u32> m_swap_interval;
};
}
