// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKGfx.h"

#include <algorithm>
#include <string>

#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Host.h"
#include "DolphinSwitch/Deko3DName.h"

#include "VideoBackends/Deko3D/DKCommandBufferManager.h"
#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKFrameGenerator.h"
#include "VideoBackends/Deko3D/DKObjectCache.h"
#include "VideoBackends/Deko3D/DKPipeline.h"
#include "VideoBackends/Deko3D/DKShader.h"
#include "VideoBackends/Deko3D/DKShaderCompiler.h"
#include "VideoBackends/Deko3D/DKStateTracker.h"
#include "VideoBackends/Deko3D/DKSwapChain.h"
#include "VideoBackends/Deko3D/DKTexture.h"
#include "VideoBackends/Deko3D/DKVertexFormat.h"

#include "VideoCommon/FrameGeneration.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"

namespace Deko3D
{
namespace
{
DkScissor MakeScissor(const MathUtil::Rectangle<int>& rc)
{

  const int left = std::max(rc.left, 0);
  const int top = std::max(rc.top, 0);
  return {static_cast<u32>(left), static_cast<u32>(top),
          static_cast<u32>(std::max(rc.right - left, 0)),
          static_cast<u32>(std::max(rc.bottom - top, 0))};
}
}

DKGfx::DKGfx(std::unique_ptr<DKSwapChain> swap_chain, float backbuffer_scale)
    : m_swap_chain(std::move(swap_chain)), m_backbuffer_scale(backbuffer_scale)
{
  UpdateActiveConfig();
  m_sampler_states.fill(RenderState::GetPointSamplerState());
}

DKGfx::~DKGfx()
{
  if (g_dk_context)
    g_dk_context->WaitIdle();
  UpdateActiveConfig();
}

bool DKGfx::IsHeadless() const
{
  return m_swap_chain == nullptr;
}

std::unique_ptr<AbstractTexture> DKGfx::CreateTexture(const TextureConfig& config,
                                                      std::string_view name)
{
  return DKTexture::Create(config, name);
}

std::unique_ptr<AbstractStagingTexture> DKGfx::CreateStagingTexture(StagingTextureType type,
                                                                    const TextureConfig& config)
{
  return DKStagingTexture::Create(type, config);
}

std::unique_ptr<AbstractFramebuffer>
DKGfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                         std::vector<AbstractTexture*> additional_color_attachments)
{
  return DKFramebuffer::Create(static_cast<DKTexture*>(color_attachment),
                               static_cast<DKTexture*>(depth_attachment),
                               std::move(additional_color_attachments));
}

std::unique_ptr<AbstractShader>
DKGfx::CreateShaderFromSource(ShaderStage stage, std::string_view source,
                              VideoCommon::ShaderIncluder* shader_includer, std::string_view name)
{
  const auto dksh = ShaderCompiler::CompileShader(stage, source, shader_includer, name);
  if (!dksh)
    return nullptr;

  return DKShader::CreateFromBinary(stage, dksh->data(), dksh->size(), name);
}

std::unique_ptr<AbstractShader> DKGfx::CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                              size_t length, std::string_view name)
{
  return DKShader::CreateFromCacheBinary(stage, data, length, name);
}

std::unique_ptr<NativeVertexFormat>
DKGfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return std::make_unique<DKVertexFormat>(vtx_decl);
}

std::unique_ptr<AbstractPipeline> DKGfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                        const void*  ,
                                                        size_t  )
{
  return DKPipeline::Create(config);
}

void DKGfx::ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool color_enable,
                        bool alpha_enable, bool z_enable, u32 color, u32 z)
{

  u32 color_mask = 0;
  if (color_enable)
    color_mask |= DkColorMask_RGB;
  if (alpha_enable)
    color_mask |= DkColorMask_A;

  if (color_mask == 0 && !z_enable)
    return;

  const ClearColor color_value = {static_cast<float>((color >> 16) & 0xFF) / 255.0f,
                                  static_cast<float>((color >> 8) & 0xFF) / 255.0f,
                                  static_cast<float>((color >> 0) & 0xFF) / 255.0f,
                                  static_cast<float>((color >> 24) & 0xFF) / 255.0f};

  float depth_value = static_cast<float>(z & 0xFFFFFF) / 16777216.0f;
  if (!g_backend_info.bSupportsReversedDepthRange)
    depth_value = 1.0f - depth_value;

  DKStateTracker::GetInstance()->ClearFramebuffer(MakeScissor(target_rc), color_mask, color_value,
                                                  z_enable, depth_value);
}

void DKGfx::SetPipeline(const AbstractPipeline* pipeline)
{
  DKStateTracker::GetInstance()->SetPipeline(static_cast<const DKPipeline*>(pipeline));
}

void DKGfx::BindFramebuffer(DKFramebuffer* framebuffer)
{
  framebuffer->Unbind();
  DKStateTracker::GetInstance()->SetFramebuffer(framebuffer);
  m_current_framebuffer = framebuffer;
}

void DKGfx::OnFramebufferDestroyed(const AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    m_current_framebuffer = nullptr;
}

void DKGfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    return;

  BindFramebuffer(static_cast<DKFramebuffer*>(framebuffer));
}

void DKGfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    return;

  BindFramebuffer(static_cast<DKFramebuffer*>(framebuffer));
  DKStateTracker::GetInstance()->DiscardFramebuffer();
}

void DKGfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value,
                                   float depth_value)
{
  BindFramebuffer(static_cast<DKFramebuffer*>(framebuffer));

  const MathUtil::Rectangle<int> rect = framebuffer->GetRect();
  DKStateTracker::GetInstance()->ClearFramebuffer(MakeScissor(rect), DkColorMask_RGBA, color_value,
                                                  true, depth_value);
}

void DKGfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  DKStateTracker::GetInstance()->SetScissor(MakeScissor(rc));
}

void DKGfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  DKStateTracker::GetInstance()->SetTexture(index, static_cast<const DKTexture*>(texture));
}

void DKGfx::SetSamplerState(u32 index, const SamplerState& state)
{
  if (m_sampler_states[index] == state)
    return;

  DKStateTracker::GetInstance()->SetSampler(index, g_dk_object_cache->GetSamplerIndex(state));
  m_sampler_states[index] = state;
}

void DKGfx::SetComputeImageTexture(u32 index, AbstractTexture* texture, bool  ,
                                   bool  )
{

  DKStateTracker::GetInstance()->SetImageTexture(index, static_cast<DKTexture*>(texture));
}

void DKGfx::UnbindTexture(const AbstractTexture* texture)
{
  DKStateTracker::GetInstance()->UnbindTexture(static_cast<const DKTexture*>(texture));
}

void DKGfx::SetViewport(float x, float y, float width, float height, float near_depth,
                        float far_depth)
{
  const DkViewport viewport{x, y, width, height, near_depth, far_depth};
  DKStateTracker::GetInstance()->SetViewport(viewport);
}

void DKGfx::Draw(u32 base_vertex, u32 num_vertices)
{
  DKStateTracker* state_tracker = DKStateTracker::GetInstance();
  if (!state_tracker->Bind())
    return;

  dkCmdBufDraw(g_dk_command_buffer_mgr->GetCurrentCommandBuffer(),
               state_tracker->GetPipeline()->GetPrimitive(), num_vertices, 1, base_vertex, 0);
}

void DKGfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  DKStateTracker* state_tracker = DKStateTracker::GetInstance();
  if (!state_tracker->Bind())
    return;

  dkCmdBufDrawIndexed(g_dk_command_buffer_mgr->GetCurrentCommandBuffer(),
                      state_tracker->GetPipeline()->GetPrimitive(), num_indices, 1, base_index,
                      static_cast<s32>(base_vertex), 0);
}

void DKGfx::DispatchComputeShader(const AbstractShader* shader, u32  ,
                                  u32  , u32  , u32 groups_x,
                                  u32 groups_y, u32 groups_z)
{

  DKStateTracker::GetInstance()->SetComputeShader(static_cast<const DKShader*>(shader));
  if (!DKStateTracker::GetInstance()->BindCompute())
    return;

  DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();
  dkCmdBufDispatchCompute(cmdbuf, groups_x, groups_y, groups_z);

  dkCmdBufBarrier(cmdbuf, DkBarrier_Primitives, DkInvalidateFlags_Image);
}

void DKGfx::ExecuteCommandBuffer(bool wait_for_completion)
{

  g_dk_command_buffer_mgr->SubmitCommandBuffer(wait_for_completion);
}

void DKGfx::Flush()
{
  ExecuteCommandBuffer(false);
}

void DKGfx::WaitForGPUIdle()
{
  ExecuteCommandBuffer(true);
}

void DKGfx::ResetSamplerStates()
{
  const u32 point_sampler = g_dk_object_cache->GetPointSamplerIndex();
  for (u32 i = 0; i < m_sampler_states.size(); i++)
  {
    m_sampler_states[i] = RenderState::GetPointSamplerState();
    DKStateTracker::GetInstance()->SetSampler(i, point_sampler);
  }

  g_dk_object_cache->ClearSamplerCache();
}

void DKGfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);

  if (bits & (CONFIG_CHANGE_BIT_ANISOTROPY | CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING))
  {
    ExecuteCommandBuffer(true);
    ResetSamplerStates();
  }
}

#ifdef HAS_FRAME_GENERATION
std::unique_ptr<VideoCommon::FrameGenerator>
DKGfx::CreateFrameGenerator(const FrameGenerationConfig& config)
{
  return Deko3D::CreateFrameGenerator(config);
}
#endif

bool DKGfx::BindBackbuffer(const ClearColor& clear_color)
{
  if (g_presenter->SurfaceChangedTestAndClear())
  {
    ExecuteCommandBuffer(true);
    m_swap_chain.reset();
    if (void* surface = g_presenter->GetNewSurfaceHandle())
    {
      m_swap_chain = DKSwapChain::Create(surface);
      if (!m_swap_chain)
      {
        PanicAlertFmt("Failed to create the " DEKO3D_DISPLAY_NAME " display surface.");
        Host_Message(HostMessageID::WMUserStop);
        return false;
      }
      g_presenter->SetBackbuffer(GetSurfaceInfo());
    }
  }
  if (g_presenter->SurfaceResizedTestAndClear() && m_swap_chain)
  {

    ExecuteCommandBuffer(true);
    if (!m_swap_chain->Recreate())
    {
      m_swap_chain.reset();
      PanicAlertFmt("Failed to resize the " DEKO3D_DISPLAY_NAME " display surface.");
      Host_Message(HostMessageID::WMUserStop);
      return false;
    }
    g_presenter->SetBackbuffer(GetSurfaceInfo());
  }
  if (!m_swap_chain)
    return false;

  m_swap_chain->SetVSync(g_ActiveConfig.bVSyncActive);
  m_current_slot = m_swap_chain->Acquire();
  if (m_current_slot < 0)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: could not acquire a swapchain image");
    return false;
  }

  SetAndClearFramebuffer(m_swap_chain->GetFramebuffer(m_current_slot), clear_color);
  return true;
}

void DKGfx::PresentBackbuffer()
{

  if (m_swap_chain && m_current_slot >= 0)
    g_dk_command_buffer_mgr->SubmitCommandBuffer(false, m_swap_chain.get(), m_current_slot);
  else
    g_dk_command_buffer_mgr->SubmitCommandBuffer(false);

  m_current_slot = -1;
  g_dk_command_buffer_mgr->PublishFrameGpuTime();
}

SurfaceInfo DKGfx::GetSurfaceInfo() const
{
  return {m_swap_chain ? m_swap_chain->GetWidth() : 1u,
          m_swap_chain ? m_swap_chain->GetHeight() : 0u, m_backbuffer_scale,
          DKSwapChain::GetTextureFormat()};
}
}
