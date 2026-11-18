// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/VideoBackend.h"

#include "Common/MsgHandler.h"
#ifdef __SWITCH2__
#include "Common/nx2_platform.h"
#endif
#include "DolphinSwitch/FrameGeneration.h"

#include "VideoBackends/Deko3D/DKBoundingBox.h"
#include "VideoBackends/Deko3D/DKCommandBufferManager.h"
#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKGfx.h"
#include "VideoBackends/Deko3D/DKObjectCache.h"
#include "VideoBackends/Deko3D/DKPerfQuery.h"
#include "VideoBackends/Deko3D/DKShaderCompiler.h"
#include "VideoBackends/Deko3D/DKStateTracker.h"
#include "VideoBackends/Deko3D/DKSwapChain.h"
#include "VideoBackends/Deko3D/DKVertexManager.h"

#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace Deko3D
{
bool VideoBackend::Initialize(const WindowSystemInfo& wsi)
{
  DolphinSwitch::FrameGeneration::BeginDekoSession();
#ifdef __SWITCH2__
  // (whatever Mesa or the framebuffer console, the LSFG loading screen, held)
  if (!nx2GpuReleaseForDrako3d())
  {
    PanicAlertFmt("Failed to release the display layer for the deko3d device.");
    return false;
  }
#endif
  g_dk_context = DKContext::Create();
  if (!g_dk_context)
  {
    PanicAlertFmt("Failed to create deko3d device.");
    return false;
  }

  g_dk_command_buffer_mgr = std::make_unique<DKCommandBufferManager>();
  if (!g_dk_command_buffer_mgr->Initialize())
  {
    PanicAlertFmt("Failed to create deko3d command buffers.");
    Shutdown();
    return false;
  }

  g_dk_object_cache = DKObjectCache::Create();
  if (!g_dk_object_cache)
  {
    PanicAlertFmt("Failed to create the deko3d object cache.");
    Shutdown();
    return false;
  }

  if (!DKStateTracker::CreateInstance())
  {
    PanicAlertFmt("Failed to create the deko3d state tracker.");
    Shutdown();
    return false;
  }

  UpdateActiveConfig();
  ShaderCompiler::Initialize();

  std::unique_ptr<DKSwapChain> swap_chain;
  if (wsi.render_surface)
  {
    swap_chain = DKSwapChain::Create(wsi.render_surface);
    if (!swap_chain)
    {
      PanicAlertFmt("Failed to create deko3d swapchain.");
      Shutdown();
      return false;
    }
  }

  auto gfx = std::make_unique<DKGfx>(std::move(swap_chain), wsi.render_surface_scale);
  auto vertex_manager = std::make_unique<DKVertexManager>();
  auto perf_query = std::make_unique<DKPerfQuery>();
  auto bounding_box = std::make_unique<DKBoundingBox>();

  return InitializeShared(std::move(gfx), std::move(vertex_manager), std::move(perf_query),
                          std::move(bounding_box));
}

void VideoBackend::Shutdown()
{
  DolphinSwitch::FrameGeneration::EndDekoSession();
  if (g_dk_context)
    g_dk_context->WaitIdle();

  ShutdownShared();
  ShaderCompiler::Shutdown();

  DKStateTracker::DestroyInstance();
  g_dk_object_cache.reset();
  g_dk_command_buffer_mgr.reset();
  g_dk_context.reset();
}
}
