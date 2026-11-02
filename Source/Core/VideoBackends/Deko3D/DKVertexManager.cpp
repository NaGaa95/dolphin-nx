// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKVertexManager.h"

#include <algorithm>
#include <cstring>

#include "Common/Align.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "Core/System.h"

#include "VideoBackends/Deko3D/DKGfx.h"
#include "VideoBackends/Deko3D/DKStateTracker.h"
#include "VideoBackends/Deko3D/DKStreamBuffer.h"
#include "VideoBackends/Deko3D/DKTexture.h"

#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexShaderManager.h"

namespace Deko3D
{
namespace
{

u32 AlignUniformSize(size_t size)
{
  return static_cast<u32>(Common::AlignUp(size, DK_UNIFORM_BUF_ALIGNMENT));
}
}

DKVertexManager::DKVertexManager() = default;

DKVertexManager::~DKVertexManager() = default;

bool DKVertexManager::Initialize()
{
  if (!VertexManagerBase::Initialize())
    return false;

  m_vertex_stream_buffer = DKStreamBuffer::Create(VERTEX_STREAM_BUFFER_SIZE, "vertex");
  m_index_stream_buffer = DKStreamBuffer::Create(INDEX_STREAM_BUFFER_SIZE, "index");
  m_uniform_stream_buffer = DKStreamBuffer::Create(UNIFORM_STREAM_BUFFER_SIZE, "uniform");
  m_texel_stream_buffer = DKStreamBuffer::Create(TEXEL_STREAM_BUFFER_SIZE, "texel");
  if (!m_vertex_stream_buffer || !m_index_stream_buffer || !m_uniform_stream_buffer ||
      !m_texel_stream_buffer)
  {
    PanicAlertFmt("Failed to allocate deko3d streaming buffers");
    return false;
  }

  static constexpr std::array<DkImageFormat, NUM_TEXEL_BUFFER_FORMATS> texel_buffer_formats = {
      DkImageFormat_R8_Uint, DkImageFormat_R16_Uint, DkImageFormat_RGBA8_Uint,
      DkImageFormat_RG32_Uint};
  for (u32 i = 0; i < NUM_TEXEL_BUFFER_FORMATS; i++)
  {
    const auto format = static_cast<TexelBufferFormat>(i);
    m_texel_buffer_descriptors[i] =
        DKTexture::MakeBufferDescriptor(m_texel_stream_buffer->GetMemBlock(),
                                        texel_buffer_formats[i], GetTexelBufferElementSize(format));
  }

  m_uniform_buffer_reserve_size = AlignUniformSize(sizeof(PixelShaderConstants)) +
                                  AlignUniformSize(sizeof(VertexShaderConstants)) +
                                  AlignUniformSize(sizeof(GeometryShaderConstants));

  UploadAllConstants();
  DKStateTracker::GetInstance()->SetUtilityUniformBuffer(
      m_uniform_stream_buffer->GetGpuAddr(), AlignUniformSize(sizeof(VertexShaderConstants)));

  return true;
}

void DKVertexManager::ResetBuffer(u32 vertex_stride)
{
  bool has_vbuffer_allocation =
      m_vertex_stream_buffer->ReserveMemory(MAXVBUFFERSIZE, vertex_stride);
  bool has_ibuffer_allocation =
      m_index_stream_buffer->ReserveMemory(MAXIBUFFERSIZE * sizeof(u16), sizeof(u16));
  if (!has_vbuffer_allocation || !has_ibuffer_allocation)
  {

    WARN_LOG_FMT(VIDEO, "Executing command buffer while waiting for space in vertex/index buffer");
    DKGfx::GetInstance()->ExecuteCommandBuffer(false);

    if (!has_vbuffer_allocation)
      has_vbuffer_allocation = m_vertex_stream_buffer->ReserveMemory(MAXVBUFFERSIZE, vertex_stride);
    if (!has_ibuffer_allocation)
    {
      has_ibuffer_allocation =
          m_index_stream_buffer->ReserveMemory(MAXIBUFFERSIZE * sizeof(u16), sizeof(u16));
    }

    if (!has_vbuffer_allocation || !has_ibuffer_allocation)
      PanicAlertFmt("Failed to allocate space in streaming buffers for pending draw");
  }

  m_base_buffer_pointer = m_vertex_stream_buffer->GetHostPointer();
  m_end_buffer_pointer = m_vertex_stream_buffer->GetCurrentHostPointer() + MAXVBUFFERSIZE;
  m_cur_buffer_pointer = m_vertex_stream_buffer->GetCurrentHostPointer();
  m_index_generator.Start(reinterpret_cast<u16*>(m_index_stream_buffer->GetCurrentHostPointer()));
}

void DKVertexManager::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                   u32* out_base_vertex, u32* out_base_index)
{
  const u32 vertex_data_size = num_vertices * vertex_stride;
  const u32 index_data_size = num_indices * sizeof(u16);

  *out_base_vertex =
      vertex_stride > 0 ? (m_vertex_stream_buffer->GetCurrentOffset() / vertex_stride) : 0;
  *out_base_index = m_index_stream_buffer->GetCurrentOffset() / sizeof(u16);

  m_vertex_stream_buffer->CommitMemory(vertex_data_size);
  m_index_stream_buffer->CommitMemory(index_data_size);

  ADDSTAT(g_stats.this_frame.bytes_vertex_streamed, static_cast<int>(vertex_data_size));
  ADDSTAT(g_stats.this_frame.bytes_index_streamed, static_cast<int>(index_data_size));
}

void DKVertexManager::BindCommittedBuffer(u32 /*vertex_stride*/)
{
  DKStateTracker::GetInstance()->SetVertexBuffer(m_vertex_stream_buffer->GetGpuAddr(),
                                                 VERTEX_STREAM_BUFFER_SIZE);
  DKStateTracker::GetInstance()->SetIndexBuffer(m_index_stream_buffer->GetGpuAddr(),
                                                DkIdxFormat_Uint16);
}

void DKVertexManager::UploadUniforms()
{
  UpdateVertexShaderConstants();
  UpdateGeometryShaderConstants();
  UpdatePixelShaderConstants();
}

void DKVertexManager::UpdateVertexShaderConstants()
{
  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();

  if (!vertex_shader_manager.dirty || !ReserveConstantStorage())
    return;

  const u32 size = AlignUniformSize(sizeof(VertexShaderConstants));
  DKStateTracker::GetInstance()->SetGXUniformBuffer(
      UBO_BINDING_VS, m_uniform_stream_buffer->GetCurrentGpuAddr(), size);
  std::memcpy(m_uniform_stream_buffer->GetCurrentHostPointer(), &vertex_shader_manager.constants,
              sizeof(VertexShaderConstants));
  m_uniform_stream_buffer->CommitMemory(size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, size);
  vertex_shader_manager.dirty = false;
}

void DKVertexManager::UpdateGeometryShaderConstants()
{
  auto& system = Core::System::GetInstance();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();

  if (!geometry_shader_manager.dirty || !ReserveConstantStorage())
    return;

  const u32 size = AlignUniformSize(sizeof(GeometryShaderConstants));
  DKStateTracker::GetInstance()->SetGXUniformBuffer(
      UBO_BINDING_GS, m_uniform_stream_buffer->GetCurrentGpuAddr(), size);
  std::memcpy(m_uniform_stream_buffer->GetCurrentHostPointer(), &geometry_shader_manager.constants,
              sizeof(GeometryShaderConstants));
  m_uniform_stream_buffer->CommitMemory(size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, size);
  geometry_shader_manager.dirty = false;
}

void DKVertexManager::UpdatePixelShaderConstants()
{
  auto& system = Core::System::GetInstance();
  auto& pixel_shader_manager = system.GetPixelShaderManager();

  if (!ReserveConstantStorage())
    return;

  if (pixel_shader_manager.dirty)
  {
    const u32 size = AlignUniformSize(sizeof(PixelShaderConstants));
    DKStateTracker::GetInstance()->SetGXUniformBuffer(
        UBO_BINDING_PS, m_uniform_stream_buffer->GetCurrentGpuAddr(), size);
    std::memcpy(m_uniform_stream_buffer->GetCurrentHostPointer(), &pixel_shader_manager.constants,
                sizeof(PixelShaderConstants));
    m_uniform_stream_buffer->CommitMemory(size);
    ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, size);
    pixel_shader_manager.dirty = false;
  }

  if (pixel_shader_manager.custom_constants_dirty)
  {
    const u32 size = AlignUniformSize(pixel_shader_manager.custom_constants.size());
    DKStateTracker::GetInstance()->SetGXUniformBuffer(
        UBO_BINDING_CUST, m_uniform_stream_buffer->GetCurrentGpuAddr(), size);
    std::memcpy(m_uniform_stream_buffer->GetCurrentHostPointer(),
                pixel_shader_manager.custom_constants.data(),
                pixel_shader_manager.custom_constants.size());
    m_uniform_stream_buffer->CommitMemory(size);
    pixel_shader_manager.custom_constants_dirty = false;
  }
}

bool DKVertexManager::ReserveConstantStorage()
{
  auto& system = Core::System::GetInstance();
  auto& pixel_shader_manager = system.GetPixelShaderManager();
  const u32 custom_constants_size = AlignUniformSize(pixel_shader_manager.custom_constants.size());

  if (m_uniform_stream_buffer->ReserveMemory(m_uniform_buffer_reserve_size + custom_constants_size,
                                             DK_UNIFORM_BUF_ALIGNMENT))
  {
    return true;
  }

  WARN_LOG_FMT(VIDEO, "Executing command buffer while waiting for space in uniform buffer");
  DKGfx::GetInstance()->ExecuteCommandBuffer(false);

  UploadAllConstants();
  return false;
}

void DKVertexManager::UploadAllConstants()
{
  auto& system = Core::System::GetInstance();
  auto& pixel_shader_manager = system.GetPixelShaderManager();

  const u32 custom_constants_size = AlignUniformSize(pixel_shader_manager.custom_constants.size());

  const u32 pixel_constants_offset = 0;
  const u32 vertex_constants_offset =
      pixel_constants_offset + AlignUniformSize(sizeof(PixelShaderConstants));
  const u32 geometry_constants_offset =
      vertex_constants_offset + AlignUniformSize(sizeof(VertexShaderConstants));
  const u32 custom_pixel_constants_offset =
      geometry_constants_offset + AlignUniformSize(sizeof(GeometryShaderConstants));
  const u32 allocation_size = custom_pixel_constants_offset + custom_constants_size;

  if (!m_uniform_stream_buffer->ReserveMemory(allocation_size, DK_UNIFORM_BUF_ALIGNMENT))
  {
    PanicAlertFmt("Failed to allocate space for constants in streaming buffer");
    return;
  }

  auto& vertex_shader_manager = system.GetVertexShaderManager();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();
  const DkGpuAddr base_addr = m_uniform_stream_buffer->GetCurrentGpuAddr();
  u8* base_pointer = m_uniform_stream_buffer->GetCurrentHostPointer();

  DKStateTracker* state_tracker = DKStateTracker::GetInstance();
  state_tracker->SetGXUniformBuffer(UBO_BINDING_PS, base_addr + pixel_constants_offset,
                                    AlignUniformSize(sizeof(PixelShaderConstants)));
  state_tracker->SetGXUniformBuffer(UBO_BINDING_VS, base_addr + vertex_constants_offset,
                                    AlignUniformSize(sizeof(VertexShaderConstants)));
  state_tracker->SetGXUniformBuffer(UBO_BINDING_GS, base_addr + geometry_constants_offset,
                                    AlignUniformSize(sizeof(GeometryShaderConstants)));
  if (!pixel_shader_manager.custom_constants.empty())
  {
    state_tracker->SetGXUniformBuffer(UBO_BINDING_CUST, base_addr + custom_pixel_constants_offset,
                                      custom_constants_size);
  }

  std::memcpy(base_pointer + pixel_constants_offset, &pixel_shader_manager.constants,
              sizeof(PixelShaderConstants));
  std::memcpy(base_pointer + vertex_constants_offset, &vertex_shader_manager.constants,
              sizeof(VertexShaderConstants));
  std::memcpy(base_pointer + geometry_constants_offset, &geometry_shader_manager.constants,
              sizeof(GeometryShaderConstants));
  if (!pixel_shader_manager.custom_constants.empty())
  {
    std::memcpy(base_pointer + custom_pixel_constants_offset,
                pixel_shader_manager.custom_constants.data(),
                pixel_shader_manager.custom_constants.size());
  }

  m_uniform_stream_buffer->CommitMemory(allocation_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, allocation_size);

  vertex_shader_manager.dirty = false;
  geometry_shader_manager.dirty = false;
  pixel_shader_manager.dirty = false;
}

void DKVertexManager::UploadUtilityUniforms(const void* data, u32 data_size)
{
  InvalidateConstants();

  const u32 size = AlignUniformSize(data_size);
  if (!m_uniform_stream_buffer->ReserveMemory(size, DK_UNIFORM_BUF_ALIGNMENT))
  {
    WARN_LOG_FMT(VIDEO, "Executing command buffer while waiting for ext space in uniform buffer");
    DKGfx::GetInstance()->ExecuteCommandBuffer(false);
    if (!m_uniform_stream_buffer->ReserveMemory(size, DK_UNIFORM_BUF_ALIGNMENT))
    {
      PanicAlertFmt("Failed to allocate space for utility uniforms");
      return;
    }
  }

  DKStateTracker::GetInstance()->SetUtilityUniformBuffer(
      m_uniform_stream_buffer->GetCurrentGpuAddr(), size);
  std::memcpy(m_uniform_stream_buffer->GetCurrentHostPointer(), data, data_size);
  m_uniform_stream_buffer->CommitMemory(size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, size);
}

bool DKVertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                        u32* out_offset)
{
  if (data_size > m_texel_stream_buffer->GetCurrentSize())
    return false;

  const u32 elem_size = GetTexelBufferElementSize(format);
  if (!m_texel_stream_buffer->ReserveMemory(data_size, elem_size))
  {
    WARN_LOG_FMT(VIDEO, "Executing command buffer while waiting for space in texel buffer");
    DKGfx::GetInstance()->ExecuteCommandBuffer(false);
    if (!m_texel_stream_buffer->ReserveMemory(data_size, elem_size))
    {
      PanicAlertFmt("Failed to allocate {} bytes from texel buffer", data_size);
      return false;
    }
  }

  std::memcpy(m_texel_stream_buffer->GetCurrentHostPointer(), data, data_size);
  *out_offset = m_texel_stream_buffer->GetCurrentOffset() / elem_size;
  m_texel_stream_buffer->CommitMemory(data_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, data_size);
  DKStateTracker::GetInstance()->SetTexelBuffer(0, m_texel_buffer_descriptors[format]);
  return true;
}

bool DKVertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                        u32* out_offset, const void* palette_data, u32 palette_size,
                                        TexelBufferFormat palette_format, u32* out_palette_offset)
{
  const u32 elem_size = GetTexelBufferElementSize(format);
  const u32 palette_elem_size = GetTexelBufferElementSize(palette_format);
  const u32 reserve_size = data_size + palette_size + palette_elem_size;
  if (reserve_size > m_texel_stream_buffer->GetCurrentSize())
    return false;

  const u32 alignment = std::max(elem_size, palette_elem_size);
  if (!m_texel_stream_buffer->ReserveMemory(reserve_size, alignment))
  {
    WARN_LOG_FMT(VIDEO, "Executing command buffer while waiting for space in texel buffer");
    DKGfx::GetInstance()->ExecuteCommandBuffer(false);
    if (!m_texel_stream_buffer->ReserveMemory(reserve_size, alignment))
    {
      PanicAlertFmt("Failed to allocate {} bytes from texel buffer", reserve_size);
      return false;
    }
  }

  const u32 palette_byte_offset = Common::AlignUp(data_size, palette_elem_size);
  std::memcpy(m_texel_stream_buffer->GetCurrentHostPointer(), data, data_size);
  std::memcpy(m_texel_stream_buffer->GetCurrentHostPointer() + palette_byte_offset, palette_data,
              palette_size);
  *out_offset = m_texel_stream_buffer->GetCurrentOffset() / elem_size;
  *out_palette_offset =
      (m_texel_stream_buffer->GetCurrentOffset() + palette_byte_offset) / palette_elem_size;

  m_texel_stream_buffer->CommitMemory(palette_byte_offset + palette_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, palette_byte_offset + palette_size);
  DKStateTracker::GetInstance()->SetTexelBuffer(0, m_texel_buffer_descriptors[format]);
  DKStateTracker::GetInstance()->SetTexelBuffer(1, m_texel_buffer_descriptors[palette_format]);
  return true;
}
}
