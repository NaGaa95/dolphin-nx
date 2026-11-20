// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"
#include "VideoCommon/Constants.h"

namespace Deko3D
{
constexpr size_t NUM_COMMAND_BUFFERS = 8;

constexpr u32 INIT_COMMAND_BUFFER_SIZE = 64 * 1024;
constexpr u32 DRAW_COMMAND_BUFFER_SIZE = 256 * 1024;

constexpr u32 COMMAND_BUFFER_GROWTH_SIZE = 256 * 1024;

constexpr u32 TEXTURE_UPLOAD_BUFFER_SIZE = 32 * 1024 * 1024;
constexpr u32 STAGING_TEXTURE_UPLOAD_THRESHOLD = 4 * 1024 * 1024;

constexpr u32 MAX_VERTEX_ATTRIBUTES = 16;

constexpr u32 PER_WARP_SCRATCH_MEMORY_SIZE = 32 * 1024;

constexpr u32 UBO_BINDING_PS = 0;
constexpr u32 UBO_BINDING_VS = 1;
constexpr u32 UBO_BINDING_CUST = 2;
constexpr u32 UBO_BINDING_GS = 3;
constexpr u32 NUM_UBO_BINDINGS = 4;

constexpr u32 UBO_BINDING_UTILITY = 0;

constexpr u32 SSBO_BINDING_BBOX = 0;
constexpr u32 SSBO_BINDING_VERTEX = 1;

constexpr u32 NUM_PIXEL_SHADER_SAMPLERS = VideoCommon::MAX_PIXEL_SHADER_SAMPLERS;
constexpr u32 NUM_COMPUTE_SHADER_SAMPLERS = VideoCommon::MAX_COMPUTE_SHADER_SAMPLERS;

constexpr u32 TEXEL_BUFFER_BINDING_BASE = NUM_PIXEL_SHADER_SAMPLERS;
constexpr u32 NUM_TEXEL_BUFFERS = 2;
constexpr u32 NUM_FRAGMENT_TEXTURE_BINDINGS = TEXEL_BUFFER_BINDING_BASE + NUM_TEXEL_BUFFERS;

constexpr u32 NUM_IMAGE_DESCRIPTORS = 64 * 1024;

constexpr u32 NUM_SAMPLER_DESCRIPTORS = 1024;
}
