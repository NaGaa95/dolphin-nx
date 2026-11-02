// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

#include "VideoCommon/VertexManagerBase.h"

namespace Deko3D
{
class DKStreamBuffer;

class DKVertexManager final : public VertexManagerBase
{
public:
  DKVertexManager();
  ~DKVertexManager() override;

  bool Initialize() override;

  void UploadUtilityUniforms(const void* uniforms, u32 uniforms_size) override;
  bool UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                         u32* out_offset) override;
  bool UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format, u32* out_offset,
                         const void* palette_data, u32 palette_size,
                         TexelBufferFormat palette_format, u32* out_palette_offset) override;

protected:
  void ResetBuffer(u32 vertex_stride) override;
  void CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices, u32* out_base_vertex,
                    u32* out_base_index) override;
  void BindCommittedBuffer(u32 vertex_stride) override;
  void UploadUniforms() override;

private:
  void UpdateVertexShaderConstants();
  void UpdateGeometryShaderConstants();
  void UpdatePixelShaderConstants();

  bool ReserveConstantStorage();
  void UploadAllConstants();

  std::unique_ptr<DKStreamBuffer> m_vertex_stream_buffer;
  std::unique_ptr<DKStreamBuffer> m_index_stream_buffer;
  std::unique_ptr<DKStreamBuffer> m_uniform_stream_buffer;
  std::unique_ptr<DKStreamBuffer> m_texel_stream_buffer;
  std::array<DkImageDescriptor, NUM_TEXEL_BUFFER_FORMATS> m_texel_buffer_descriptors = {};
  u32 m_uniform_buffer_reserve_size = 0;
};
}
