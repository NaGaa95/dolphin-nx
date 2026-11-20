// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/VideoBackend.h"
#include "VideoCommon/VideoConfig.h"

namespace Deko3D
{
void VideoBackend::InitBackendInfo(const WindowSystemInfo& wsi)
{
  g_backend_info.api_type = APIType::Vulkan;
  g_backend_info.MaxTextureSize = 16384;
  g_backend_info.bUsesLowerLeftOrigin = false;
  g_backend_info.bSupportsExclusiveFullscreen = false;
  g_backend_info.bSupportsDualSourceBlend = true;
  g_backend_info.bSupportsPrimitiveRestart = true;
  g_backend_info.bSupportsGeometryShaders = true;
  g_backend_info.bSupportsComputeShaders = true;
  g_backend_info.bSupports3DVision = false;
  g_backend_info.bSupportsEarlyZ = true;
  g_backend_info.bSupportsBindingLayout = true;
  g_backend_info.bSupportsGSInstancing = true;
  g_backend_info.bSupportsPostProcessing = true;
  g_backend_info.bSupportsClipControl = true;
  g_backend_info.bSupportsSSAA = true;
  g_backend_info.bSupportsBitfield = true;
  g_backend_info.bSupportsDynamicSamplerIndexing = true;
  g_backend_info.bSupportsFragmentStoresAndAtomics = true;
  g_backend_info.bSupportsCopyToVram = true;

  g_backend_info.bSupportsDepthClamp = false;
  g_backend_info.bSupportsReversedDepthRange = true;
  g_backend_info.bSupportsLogicOp = true;
  g_backend_info.bSupportsMultithreading = false;
  g_backend_info.bSupportsLodBiasInSampler = true;
  g_backend_info.bSupportsSettingObjectNames = false;
  g_backend_info.bSupportsPartialMultisampleResolve = true;
  g_backend_info.bSupportsDynamicVertexLoader = true;
  g_backend_info.bSupportsVSLinePointExpand = true;
  g_backend_info.bSupportsLargePoints = true;
  g_backend_info.bSupportsCoarseDerivatives = true;
  g_backend_info.bSupportsTextureQueryLevels = true;

  g_backend_info.bSupportsShaderBinaries = true;
  g_backend_info.bSupportsBackgroundCompiling = true;

  g_backend_info.bSupportsBBox = true;

  g_backend_info.bSupportsPaletteConversion = true;

  g_backend_info.bSupportsST3CTextures = true;
  g_backend_info.bSupportsBPTCTextures = true;

  g_backend_info.bSupportsGPUTextureDecoding = false;
  g_backend_info.bSupportsDepthReadback = true;
  g_backend_info.bSupportsPartialDepthCopies = true;

  g_backend_info.bSupportsPipelineCacheData = false;

  g_backend_info.Adapters.clear();
  g_backend_info.AAModes = {1, 2, 4};
}

}
