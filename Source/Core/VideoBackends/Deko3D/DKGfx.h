// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <string_view>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/Constants.h"
#include "VideoCommon/RenderState.h"

namespace Deko3D
{
class DKFramebuffer;
class DKSwapChain;

class DKGfx final : public ::AbstractGfx
{
public:
  DKGfx(std::unique_ptr<DKSwapChain> swap_chain, float backbuffer_scale);
  ~DKGfx() override;

  static DKGfx* GetInstance() { return static_cast<DKGfx*>(g_gfx.get()); }

  bool IsHeadless() const override;

  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                 std::string_view name) override;
  std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config) override;
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments) override;

  std::unique_ptr<AbstractShader>
  CreateShaderFromSource(ShaderStage stage, std::string_view source,
                         VideoCommon::ShaderIncluder* shader_includer,
                         std::string_view name) override;
  std::unique_ptr<AbstractShader> CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                         size_t length,
                                                         std::string_view name) override;
  std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl) override;
  std::unique_ptr<AbstractPipeline> CreatePipeline(const AbstractPipelineConfig& config,
                                                   const void* cache_data = nullptr,
                                                   size_t cache_data_length = 0) override;

  DKSwapChain* GetSwapChain() const { return m_swap_chain.get(); }

  void OnFramebufferDestroyed(const AbstractFramebuffer* framebuffer);

  void ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool color_enable, bool alpha_enable,
                   bool z_enable, u32 color, u32 z) override;

  void SetPipeline(const AbstractPipeline* pipeline) override;
  void SetFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value = {},
                              float depth_value = 0.0f) override;
  void SetScissorRect(const MathUtil::Rectangle<int>& rc) override;
  void SetTexture(u32 index, const AbstractTexture* texture) override;
  void SetSamplerState(u32 index, const SamplerState& state) override;
  void SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write) override;
  void UnbindTexture(const AbstractTexture* texture) override;
  void SetViewport(float x, float y, float width, float height, float near_depth,
                   float far_depth) override;

  void Draw(u32 base_vertex, u32 num_vertices) override;
  void DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex) override;
  void DispatchComputeShader(const AbstractShader* shader, u32 groupsize_x, u32 groupsize_y,
                             u32 groupsize_z, u32 groups_x, u32 groups_y, u32 groups_z) override;

  void Flush() override;
  void WaitForGPUIdle() override;
  void OnConfigChanged(u32 bits) override;

  void ExecuteCommandBuffer(bool wait_for_completion);

#ifdef HAS_FRAME_GENERATION
  std::unique_ptr<VideoCommon::FrameGenerator>
  CreateFrameGenerator(const FrameGenerationConfig& config) override;
#endif

  bool BindBackbuffer(const ClearColor& clear_color = {}) override;
  void PresentBackbuffer() override;
#ifdef __SWITCH2__
  bool SupportsGuestScreen() const override { return true; }
  bool OpenGuestScreen(void* window) override;
  void CloseGuestScreen() override;
  void CopyToGuestScreen(const AbstractTexture* source) override;
#endif

  SurfaceInfo GetSurfaceInfo() const override;

private:
  void BindFramebuffer(DKFramebuffer* framebuffer);
  void ResetSamplerStates();

  std::unique_ptr<DKSwapChain> m_swap_chain;
  float m_backbuffer_scale;

  std::array<SamplerState, VideoCommon::MAX_PIXEL_SHADER_SAMPLERS> m_sampler_states = {};

  int m_current_slot = -1;
#ifdef __SWITCH2__
  // GameShare's guest screen: CopyToGuestScreen blits into its acquired image, presented after the
  // frame's present
  std::unique_ptr<DKSwapChain> m_guest_swap_chain;
  int m_guest_slot = -1;
#endif
};
}
