// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKSwapChain.h"

#include <array>

#include <switch.h>

#include "Common/Align.h"
#include "Common/Logging/Log.h"
#ifdef __SWITCH2__
#include "Common/nx2_platform.h"
#endif

#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKMemoryTracker.h"
#include "VideoBackends/Deko3D/DKTexture.h"

#include "VideoCommon/TextureConfig.h"

namespace Deko3D
{
DKSwapChain::~DKSwapChain()
{
  Destroy();
}

std::unique_ptr<DKSwapChain> DKSwapChain::Create(void* native_window)
{
  auto swap_chain = std::make_unique<DKSwapChain>();
  swap_chain->m_native_window = native_window;
  if (!swap_chain->Initialize())
    return nullptr;
  return swap_chain;
}

bool DKSwapChain::Initialize()
{
  DkDevice device = g_dk_context->GetDevice();

  u32 width = 0;
  u32 height = 0;
  if (R_FAILED(nwindowGetDimensions(static_cast<NWindow*>(m_native_window), &width, &height)))
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: nwindowGetDimensions failed");
    return false;
  }
  m_width = width;
  m_height = height;
  if (m_width == 0 || m_height == 0)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: invalid window dimensions");
    return false;
  }

  dk::ImageLayout layout;
#ifdef __SWITCH2__
  // VI2 presents uncompressed images.
  constexpr u32 SWAPCHAIN_FLAGS = DkImageFlags_UsageRender | DkImageFlags_UsagePresent;
#else
  constexpr u32 SWAPCHAIN_FLAGS =
      DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
#endif
  dk::ImageLayoutMaker{device}
      .setFlags(SWAPCHAIN_FLAGS)
      .setFormat(FORMAT)
      .setDimensions(m_width, m_height)
      .initialize(layout);

  const u64 image_size = layout.getSize();
  const u32 image_align = layout.getAlignment();
  const u64 aligned_size = Common::AlignUp(image_size, image_align);

  m_image_memblock =
      dk::MemBlockMaker{device, static_cast<u32>(Common::AlignUp(aligned_size * NUM_IMAGES,
                                                                 DK_MEMBLOCK_ALIGNMENT))}
          .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
          .create();
  if (!m_image_memblock)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: failed to allocate swapchain image memory");
    return false;
  }

  const TextureConfig config(m_width, m_height, 1, 1, 1, GetTextureFormat(),
                             AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2D);

  std::array<DkImage const*, NUM_IMAGES> image_ptrs;
  for (u32 i = 0; i < NUM_IMAGES; ++i)
  {
    m_images[i].initialize(layout, m_image_memblock, static_cast<u32>(aligned_size * i));
    image_ptrs[i] = &m_images[i];

    MemoryTracker::Register(dkImageGetGpuAddr(&m_images[i]), image_size,
                            fmt::format("swapchain image {}", i));

    m_textures[i] = DKTexture::CreateAdopted(config, layout, m_images[i]);
    m_framebuffers[i] = DKFramebuffer::Create(m_textures[i].get(), nullptr, {});
    if (!m_framebuffers[i])
    {
      ERROR_LOG_FMT(VIDEO, "deko3d: failed to wrap swapchain image {} in a framebuffer", i);
      return false;
    }
  }

  m_swapchain = dk::SwapchainMaker{device, m_native_window, image_ptrs}.create();
  if (!m_swapchain)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: failed to create swapchain");
    return false;
  }

  return true;
}

void DKSwapChain::Destroy()
{
  if (g_dk_context)
    g_dk_context->WaitIdle();
  m_swapchain = nullptr;
  m_swap_interval.reset();
  for (const dk::Image& image : m_images)
    MemoryTracker::Unregister(dkImageGetGpuAddr(&image));
  for (auto& framebuffer : m_framebuffers)
    framebuffer.reset();
  for (auto& texture : m_textures)
    texture.reset();
  m_image_memblock = nullptr;
}

int DKSwapChain::Acquire()
{
  return g_dk_context->GetGraphicsQueue().acquireImage(m_swapchain);
}

void DKSwapChain::Present(int slot)
{
  g_dk_context->GetGraphicsQueue().presentImage(m_swapchain, slot);
}

void DKSwapChain::SetVSync(bool enabled)
{
  const u32 interval = enabled ? 1 : 0;
#ifdef __SWITCH2__
  // (drako3d presents at the window's interval; its setSwapInterval would withdraw the 120 Hz request)
  if (m_swapchain)
  {
    (void)nx2DisplayApply(static_cast<int>(interval));
    m_swap_interval = interval;
  }
#else
  if (m_swapchain && m_swap_interval != interval)
  {
    m_swapchain.setSwapInterval(interval);
    m_swap_interval = interval;
  }
#endif
}

bool DKSwapChain::Recreate()
{
  Destroy();
  return Initialize();
}
}
