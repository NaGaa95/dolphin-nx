// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKStateTracker.h"

#include <cstring>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Deko3D/DKCommandBufferManager.h"
#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKGfx.h"
#include "VideoBackends/Deko3D/DKMemoryTracker.h"
#include "VideoBackends/Deko3D/DKObjectCache.h"
#include "VideoBackends/Deko3D/DKPipeline.h"
#include "VideoBackends/Deko3D/DKShader.h"
#include "VideoBackends/Deko3D/DKStreamBuffer.h"
#include "VideoBackends/Deko3D/DKTexture.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"

namespace Deko3D
{
namespace
{
std::unique_ptr<DKStateTracker> s_state_tracker;

static_assert(sizeof(DkImageDescriptor) == DK_IMAGE_DESCRIPTOR_ALIGNMENT,
              "Image descriptor indices are derived from ring offsets");
static_assert(NUM_FRAGMENT_TEXTURE_BINDINGS <= DK_NUM_TEXTURE_BINDINGS);

constexpr std::array<DkStage, 3> GRAPHICS_STAGES = {DkStage_Vertex, DkStage_Geometry,
                                                    DkStage_Fragment};

constexpr u32 DUMMY_BUFFER_SIZE = DK_MEMBLOCK_ALIGNMENT;
}

DKStateTracker::DKStateTracker() = default;

DKStateTracker::~DKStateTracker() = default;

DKStateTracker* DKStateTracker::GetInstance()
{
  return s_state_tracker.get();
}

bool DKStateTracker::CreateInstance()
{
  ASSERT(!s_state_tracker);
  s_state_tracker = std::make_unique<DKStateTracker>();
  if (!s_state_tracker->Initialize())
  {
    s_state_tracker.reset();
    return false;
  }

  return true;
}

void DKStateTracker::DestroyInstance()
{
  if (!s_state_tracker)
    return;

  s_state_tracker->m_textures.fill(nullptr);
  s_state_tracker->m_image_textures.fill(nullptr);
  s_state_tracker->m_dummy_texture.reset();
  s_state_tracker->m_dummy_compute_texture.reset();

  s_state_tracker.reset();
}

bool DKStateTracker::Initialize()
{
  m_image_descriptors = DKStreamBuffer::Create(NUM_IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor),
                                               "image descriptors");
  if (!m_image_descriptors)
  {
    PanicAlertFmt("Failed to allocate the deko3d image descriptor set");
    return false;
  }

  m_dummy_buffer = dk::MemBlockMaker{g_dk_context->GetDevice(), DUMMY_BUFFER_SIZE}
                       .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                 DkMemBlockFlags_ZeroFillInit)
                       .create();
  if (!m_dummy_buffer)
  {
    PanicAlertFmt("Failed to allocate the deko3d dummy uniform buffer");
    return false;
  }

  MemoryTracker::RegisterMemBlock(m_dummy_buffer, "dummy uniform/storage buffer");

  const DkBufExtents dummy_extents{m_dummy_buffer.getGpuAddr(), DUMMY_BUFFER_SIZE};
  m_gx_ubos.fill(dummy_extents);
  m_utility_ubo = dummy_extents;
  m_ssbo = dummy_extents;

  m_index_buffer_addr = dummy_extents.addr;

  m_dummy_texture = DKTexture::Create(TextureConfig(1, 1, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                                                    AbstractTextureType::Texture_2DArray),
                                      "dummy");
  m_dummy_compute_texture = DKTexture::Create(
      TextureConfig(1, 1, 1, 1, 1, AbstractTextureFormat::RGBA8, AbstractTextureFlag_ComputeImage,
                    AbstractTextureType::Texture_2DArray),
      "dummy compute");
  if (!m_dummy_texture || !m_dummy_compute_texture)
  {
    PanicAlertFmt("Failed to allocate the deko3d dummy textures");
    return false;
  }

  m_dummy_texel_buffer =
      DKTexture::MakeBufferDescriptor(m_dummy_buffer, DkImageFormat_R8_Uint, sizeof(u8));

  m_textures.fill(m_dummy_texture.get());
  m_image_textures.fill(m_dummy_compute_texture.get());
  m_texel_buffers.fill(m_dummy_texel_buffer);
  m_samplers.fill(g_dk_object_cache->GetPointSamplerIndex());

  InvalidateCachedState();
  return true;
}

DkImageDescriptor* DKStateTracker::GetImageDescriptors() const
{
  return reinterpret_cast<DkImageDescriptor*>(m_image_descriptors->GetHostPointer());
}

bool DKStateTracker::ReserveImageDescriptors(u32 count, u32* out_first_index)
{
  const u32 size = count * sizeof(DkImageDescriptor);
  if (!m_image_descriptors->ReserveMemory(size, DK_IMAGE_DESCRIPTOR_ALIGNMENT))
  {
    WARN_LOG_FMT(VIDEO, "Submitting command buffer while waiting for space in the image "
                        "descriptor set");
    DKGfx::GetInstance()->ExecuteCommandBuffer(false);
    if (!m_image_descriptors->ReserveMemory(size, DK_IMAGE_DESCRIPTOR_ALIGNMENT))
    {
      PanicAlertFmt("Failed to allocate {} deko3d image descriptors", count);
      return false;
    }
  }

  *out_first_index = m_image_descriptors->GetCurrentOffset() / sizeof(DkImageDescriptor);
  return true;
}

void DKStateTracker::CommitImageDescriptors(u32 count)
{
  m_image_descriptors->CommitMemory(count * sizeof(DkImageDescriptor));
}

bool DKStateTracker::PrepareTextureHandles()
{
  u32 first_index;
  if (!ReserveImageDescriptors(NUM_FRAGMENT_TEXTURE_BINDINGS, &first_index))
    return false;

  DkImageDescriptor* descriptors = GetImageDescriptors();
  for (u32 i = 0; i < NUM_PIXEL_SHADER_SAMPLERS; i++)
  {
    const DKTexture* texture = m_textures[i] ? m_textures[i] : m_dummy_texture.get();
    descriptors[first_index + i] = texture->GetDescriptor();
    m_texture_handles[i] = dkMakeTextureHandle(first_index + i, m_samplers[i]);
  }

  const u32 point_sampler = g_dk_object_cache->GetPointSamplerIndex();
  for (u32 i = 0; i < NUM_TEXEL_BUFFERS; i++)
  {
    const u32 binding = TEXEL_BUFFER_BINDING_BASE + i;
    descriptors[first_index + binding] = m_texel_buffers[i];
    m_texture_handles[binding] = dkMakeTextureHandle(first_index + binding, point_sampler);
  }

  CommitImageDescriptors(NUM_FRAGMENT_TEXTURE_BINDINGS);
  return true;
}

bool DKStateTracker::PrepareImageHandles()
{
  u32 first_index;
  if (!ReserveImageDescriptors(NUM_COMPUTE_SHADER_SAMPLERS, &first_index))
    return false;

  DkImageDescriptor* descriptors = GetImageDescriptors();
  for (u32 i = 0; i < NUM_COMPUTE_SHADER_SAMPLERS; i++)
  {
    const DKTexture* texture =
        m_image_textures[i] ? m_image_textures[i] : m_dummy_compute_texture.get();
    descriptors[first_index + i] = texture->GetDescriptor();
    m_image_handles[i] = dkMakeImageHandle(first_index + i);
  }

  CommitImageDescriptors(NUM_COMPUTE_SHADER_SAMPLERS);
  return true;
}

void DKStateTracker::SetVertexBuffer(DkGpuAddr addr, u32 size)
{
  if (m_vertex_buffer.addr == addr && m_vertex_buffer.size == size)
    return;

  m_vertex_buffer = {addr, size};
  m_dirty_flags |= DIRTY_FLAG_VERTEX_BUFFER | DIRTY_FLAG_SSBO;
}

void DKStateTracker::SetIndexBuffer(DkGpuAddr addr, DkIdxFormat format)
{
  if (m_index_buffer_addr == addr && m_index_format == format)
    return;

  m_index_buffer_addr = addr;
  m_index_format = format;
  m_dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;
}

void DKStateTracker::SetFramebuffer(DKFramebuffer* framebuffer)
{
  if (m_framebuffer == framebuffer)
    return;

  m_framebuffer = framebuffer;
  m_dirty_flags |= DIRTY_FLAG_FRAMEBUFFER;
}

void DKStateTracker::SetPipeline(const DKPipeline* pipeline)
{
  if (m_pipeline == pipeline)
    return;

  const bool usage_changed =
      pipeline && (!m_pipeline || m_pipeline->GetUsage() != pipeline->GetUsage());
  const bool stages_changed =
      pipeline && (!m_pipeline || m_pipeline->GetStageMask() != pipeline->GetStageMask());

  m_pipeline = pipeline;
  m_dirty_flags |= DIRTY_FLAG_PIPELINE;
  if (usage_changed || stages_changed)
    m_dirty_flags |= DIRTY_FLAG_GX_UBOS | DIRTY_FLAG_UTILITY_UBO;
}

void DKStateTracker::SetComputeShader(const DKShader* shader)
{
  if (m_compute_shader == shader)
    return;

  if (m_compute_shader_code && g_dk_command_buffer_mgr)
    g_dk_command_buffer_mgr->DeferCleanup([code = std::move(m_compute_shader_code)]() {});

  m_compute_shader = shader;
  m_compute_shader_code = shader ? shader->GetCode() : nullptr;
  m_dirty_flags |= DIRTY_FLAG_COMPUTE_SHADER;
}

void DKStateTracker::SetGXUniformBuffer(u32 index, DkGpuAddr addr, u32 size)
{
  DkBufExtents& binding = m_gx_ubos[index];
  if (binding.addr == addr && binding.size == size)
    return;

  binding = {addr, size};
  m_dirty_flags |= DIRTY_FLAG_GX_UBOS;
}

void DKStateTracker::SetUtilityUniformBuffer(DkGpuAddr addr, u32 size)
{
  if (m_utility_ubo.addr == addr && m_utility_ubo.size == size)
    return;

  m_utility_ubo = {addr, size};
  m_dirty_flags |= DIRTY_FLAG_UTILITY_UBO;
}

void DKStateTracker::SetTexture(u32 index, const DKTexture* texture)
{
  if (!texture)
    texture = m_dummy_texture.get();

  if (m_textures[index] == texture)
    return;

  m_textures[index] = texture;
  m_dirty_flags |= DIRTY_FLAG_TEXTURES;
}

void DKStateTracker::SetSampler(u32 index, u32 sampler_index)
{
  if (m_samplers[index] == sampler_index)
    return;

  m_samplers[index] = sampler_index;
  m_dirty_flags |= DIRTY_FLAG_TEXTURES;
}

void DKStateTracker::SetTexelBuffer(u32 index, const DkImageDescriptor& descriptor)
{
  if (std::memcmp(&m_texel_buffers[index], &descriptor, sizeof(descriptor)) == 0)
    return;

  m_texel_buffers[index] = descriptor;
  m_dirty_flags |= DIRTY_FLAG_TEXTURES;
}

void DKStateTracker::SetSSBO(DkGpuAddr addr, u32 size)
{
  if (m_ssbo.addr == addr && m_ssbo.size == size)
    return;

  m_ssbo = {addr, size};
  m_dirty_flags |= DIRTY_FLAG_SSBO;
}

void DKStateTracker::SetImageTexture(u32 index, const DKTexture* texture)
{
  if (!texture)
    texture = m_dummy_compute_texture.get();

  if (m_image_textures[index] == texture)
    return;

  m_image_textures[index] = texture;
  m_dirty_flags |= DIRTY_FLAG_COMPUTE_IMAGES;
}

void DKStateTracker::UnbindTexture(const DKTexture* texture)
{
  for (const DKTexture*& bound : m_textures)
  {
    if (bound == texture)
    {
      bound = m_dummy_texture.get();
      m_dirty_flags |= DIRTY_FLAG_TEXTURES;
    }
  }

  for (const DKTexture*& bound : m_image_textures)
  {
    if (bound == texture)
    {
      bound = m_dummy_compute_texture.get();
      m_dirty_flags |= DIRTY_FLAG_COMPUTE_IMAGES;
    }
  }
}

void DKStateTracker::UnbindFramebuffer(const DKFramebuffer* framebuffer)
{
  if (m_framebuffer != framebuffer)
    return;

  m_framebuffer = nullptr;
  m_dirty_flags |= DIRTY_FLAG_FRAMEBUFFER;
}

void DKStateTracker::SetViewport(const DkViewport& viewport)
{
  if (std::memcmp(&m_viewport, &viewport, sizeof(viewport)) == 0)
    return;

  m_viewport = viewport;
  m_dirty_flags |= DIRTY_FLAG_VIEWPORT;
}

void DKStateTracker::SetScissor(const DkScissor& scissor)
{
  if (std::memcmp(&m_scissor, &scissor, sizeof(scissor)) == 0)
    return;

  m_scissor = scissor;
  m_dirty_flags |= DIRTY_FLAG_SCISSOR;
}

void DKStateTracker::InvalidateCachedState()
{
  m_dirty_flags = DIRTY_FLAG_ALL;
}

void DKStateTracker::BindStaticState(DkCmdBuf cmdbuf)
{
  dkCmdBufBindImageDescriptorSet(cmdbuf, m_image_descriptors->GetGpuAddr(), NUM_IMAGE_DESCRIPTORS);
  dkCmdBufBindSamplerDescriptorSet(cmdbuf, g_dk_object_cache->GetSamplerDescriptorSetAddr(),
                                   DKObjectCache::GetSamplerDescriptorCount());

  constexpr DkViewportSwizzle swizzle{DkSwizzle_PositiveX, DkSwizzle_NegativeY, DkSwizzle_PositiveZ,
                                      DkSwizzle_PositiveW};
  std::array<DkViewportSwizzle, DK_NUM_VIEWPORTS> swizzles;
  swizzles.fill(swizzle);
  dkCmdBufSetViewportSwizzles(cmdbuf, 0, swizzles.data(), static_cast<u32>(swizzles.size()));
}

void DKStateTracker::BindFramebuffer()
{
  if (!(m_dirty_flags & DIRTY_FLAG_FRAMEBUFFER) || !m_framebuffer)
    return;

  DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();

  dkCmdBufBarrier(cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image);

  m_framebuffer->Bind(cmdbuf);
  m_dirty_flags &= ~DIRTY_FLAG_FRAMEBUFFER;
}

void DKStateTracker::ClearFramebuffer(const DkScissor& area, u32 color_mask,
                                      const std::array<float, 4>& color, bool clear_depth,
                                      float depth)
{
  if (!m_framebuffer)
    return;

  BindFramebuffer();

  DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();
  dkCmdBufSetScissors(cmdbuf, 0, &area, 1);
  m_dirty_flags |= DIRTY_FLAG_SCISSOR;

  if (color_mask != 0 && m_framebuffer->HasColorBuffer())
  {
    const u32 num_targets = 1 + m_framebuffer->GetNumberOfAdditionalAttachments();
    for (u32 i = 0; i < num_targets; i++)
    {
      dkCmdBufClearColorFloat(cmdbuf, i, color_mask, color[0], color[1], color[2], color[3]);
    }
  }

  if (clear_depth && m_framebuffer->HasDepthBuffer())
    dkCmdBufClearDepthStencil(cmdbuf, true, depth, 0, 0);
}

void DKStateTracker::DiscardFramebuffer()
{
  if (!m_framebuffer)
    return;

  BindFramebuffer();

  DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();
  if (m_framebuffer->HasColorBuffer())
  {
    const u32 num_targets = 1 + m_framebuffer->GetNumberOfAdditionalAttachments();
    for (u32 i = 0; i < num_targets; i++)
      dkCmdBufDiscardColor(cmdbuf, i);
  }

  if (m_framebuffer->HasDepthBuffer())
    dkCmdBufDiscardDepthStencil(cmdbuf);
}

void DKStateTracker::UpdateUniformBuffers(DkCmdBuf cmdbuf)
{
  if (m_pipeline->GetUsage() == AbstractPipelineUsage::Utility)
  {
    if (!(m_dirty_flags & DIRTY_FLAG_UTILITY_UBO))
      return;

    for (DkStage stage : GRAPHICS_STAGES)
    {
      if (m_pipeline->GetStageMask() & (1u << static_cast<u32>(stage)))
        dkCmdBufBindUniformBuffers(cmdbuf, stage, UBO_BINDING_UTILITY, &m_utility_ubo, 1);
    }

    m_dirty_flags &= ~DIRTY_FLAG_UTILITY_UBO;
    return;
  }

  if (!(m_dirty_flags & DIRTY_FLAG_GX_UBOS))
    return;

  for (DkStage stage : GRAPHICS_STAGES)
  {
    if (m_pipeline->GetStageMask() & (1u << static_cast<u32>(stage)))
    {
      dkCmdBufBindUniformBuffers(cmdbuf, stage, 0, m_gx_ubos.data(),
                                 static_cast<u32>(m_gx_ubos.size()));
    }
  }

  m_dirty_flags &= ~DIRTY_FLAG_GX_UBOS;
}

bool DKStateTracker::Bind()
{
  if (!m_pipeline || !m_pipeline->IsValid() || !m_framebuffer)
    return false;

  if ((m_dirty_flags & DIRTY_FLAG_TEXTURES) && !PrepareTextureHandles())
    return false;

  DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();

  if (m_dirty_flags & DIRTY_FLAG_STATIC_STATE)
    BindStaticState(cmdbuf);

  BindFramebuffer();

  if (m_dirty_flags & DIRTY_FLAG_PIPELINE)
  {
    DKShaderCode::PrepareForUse(cmdbuf);
    m_pipeline->Replay(cmdbuf);
    m_dirty_flags |= DIRTY_FLAG_COMPUTE_SHADER;
  }

  const bool needs_vertex_buffer = !g_backend_info.bSupportsDynamicVertexLoader ||
                                   m_pipeline->GetUsage() != AbstractPipelineUsage::GXUber;
  if (needs_vertex_buffer && (m_dirty_flags & DIRTY_FLAG_VERTEX_BUFFER))
  {
    dkCmdBufBindVtxBuffer(cmdbuf, 0, m_vertex_buffer.addr, m_vertex_buffer.size);
    m_dirty_flags &= ~DIRTY_FLAG_VERTEX_BUFFER;
  }

  if (m_dirty_flags & DIRTY_FLAG_INDEX_BUFFER)
    dkCmdBufBindIdxBuffer(cmdbuf, m_index_format, m_index_buffer_addr);

  if (m_dirty_flags & DIRTY_FLAG_VIEWPORT)
    dkCmdBufSetViewports(cmdbuf, 0, &m_viewport, 1);

  if (m_dirty_flags & DIRTY_FLAG_SCISSOR)
    dkCmdBufSetScissors(cmdbuf, 0, &m_scissor, 1);

  UpdateUniformBuffers(cmdbuf);

  if (m_dirty_flags & DIRTY_FLAG_TEXTURES)
  {
    dkCmdBufBindTextures(cmdbuf, DkStage_Fragment, 0, m_texture_handles.data(),
                         static_cast<u32>(m_texture_handles.size()));
  }

  if (m_dirty_flags & DIRTY_FLAG_SSBO)
  {
    dkCmdBufBindStorageBuffers(cmdbuf, DkStage_Fragment, SSBO_BINDING_BBOX, &m_ssbo, 1);
    dkCmdBufBindStorageBuffers(cmdbuf, DkStage_Vertex, SSBO_BINDING_VERTEX, &m_vertex_buffer, 1);
  }

  m_dirty_flags &=
      ~(DIRTY_FLAG_STATIC_STATE | DIRTY_FLAG_PIPELINE | DIRTY_FLAG_INDEX_BUFFER |
        DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR | DIRTY_FLAG_TEXTURES | DIRTY_FLAG_SSBO);
  return true;
}

bool DKStateTracker::BindCompute()
{
  if (!m_compute_shader || !m_compute_shader->IsValid())
    return false;

  if ((m_dirty_flags & DIRTY_FLAG_TEXTURES) && !PrepareTextureHandles())
    return false;
  if ((m_dirty_flags & DIRTY_FLAG_COMPUTE_IMAGES) && !PrepareImageHandles())
    return false;

  DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();

  if (m_dirty_flags & DIRTY_FLAG_STATIC_STATE)
    BindStaticState(cmdbuf);

  if (m_dirty_flags & DIRTY_FLAG_COMPUTE_SHADER)
  {
    DKShaderCode::PrepareForUse(cmdbuf);
    const DkShader* shader = m_compute_shader->GetShader();
    dkCmdBufBindShaders(cmdbuf, DkStageFlag_Compute, &shader, 1);
  }

  dkCmdBufBindUniformBuffers(cmdbuf, DkStage_Compute, UBO_BINDING_UTILITY, &m_utility_ubo, 1);
  dkCmdBufBindTextures(cmdbuf, DkStage_Compute, 0, m_texture_handles.data(),
                       static_cast<u32>(m_texture_handles.size()));
  dkCmdBufBindImages(cmdbuf, DkStage_Compute, 0, m_image_handles.data(),
                     static_cast<u32>(m_image_handles.size()));

  m_dirty_flags &= ~(DIRTY_FLAG_STATIC_STATE | DIRTY_FLAG_COMPUTE_SHADER | DIRTY_FLAG_TEXTURES |
                     DIRTY_FLAG_COMPUTE_IMAGES);
  m_dirty_flags |= DIRTY_FLAG_PIPELINE;
  return true;
}
}
