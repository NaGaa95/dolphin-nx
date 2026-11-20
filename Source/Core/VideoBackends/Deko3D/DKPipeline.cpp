// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKPipeline.h"

#include <array>
#include <mutex>

#include "Common/Assert.h"
#include "Common/EnumMap.h"

#include "VideoBackends/Deko3D/DKObjectCache.h"
#include "VideoBackends/Deko3D/DKShader.h"
#include "VideoBackends/Deko3D/DKVertexFormat.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/VideoConfig.h"

namespace Deko3D
{
namespace
{
constexpr u32 MAX_CAPTURE_WORDS = 1024;

u32 GetDkBlendFactorSetting(DkBlendFactor factor)
{
  u32 result = static_cast<u32>(factor) & 0x1f;
  if ((static_cast<u32>(factor) & 0x20) != 0)
    result |= 0xc000;
  return result;
}

DkMsMode GetDkMsMode(u32 samples)
{
  switch (samples)
  {
  case 2:
    return DkMsMode_2x;
  case 4:
    return DkMsMode_4x;
  case 8:
    return DkMsMode_8x;
  case 1:
  default:
    return DkMsMode_1x;
  }
}

DkPrimitive GetDkPrimitive(PrimitiveType type)
{
  static constexpr std::array<DkPrimitive, 4> primitives = {
      DkPrimitive_Points, DkPrimitive_Lines, DkPrimitive_Triangles, DkPrimitive_TriangleStrip};
  return primitives[static_cast<u32>(type)];
}

DkRasterizerState GetRasterizerState(const RasterizationState& state)
{
  static constexpr std::array<DkFace, 4> cull_modes = {DkFace_None, DkFace_Back, DkFace_Front,
                                                       DkFace_FrontAndBack};

  DkRasterizerState raster{};
  dkRasterizerStateDefaults(&raster);
  raster.depthClampEnable = g_backend_info.bSupportsDepthClamp ? 1 : 0;
  raster.polygonModeFront = DkPolygonMode_Fill;
  raster.polygonModeBack = DkPolygonMode_Fill;
  raster.cullMode = cull_modes[static_cast<u32>(state.cull_mode.Value())];

  raster.frontFace = DkFrontFace_CW;
  return raster;
}

DkMultisampleState GetMultisampleState(const FramebufferState& state)
{
  DkMultisampleState multisample{};
  dkMultisampleStateDefaults(&multisample);
  multisample.mode = GetDkMsMode(state.samples);
  multisample.rasterizerMode = multisample.mode;
  dkMultisampleStateSetLocations(&multisample, nullptr, 0);
  return multisample;
}

DkDepthStencilState GetDepthStencilState(const DepthState& state)
{
  const bool inverted_depth = !g_backend_info.bSupportsReversedDepthRange;
  DkCompareOp compare_op;
  switch (state.func)
  {
  case CompareMode::Never:
    compare_op = DkCompareOp_Never;
    break;
  case CompareMode::Less:
    compare_op = inverted_depth ? DkCompareOp_Greater : DkCompareOp_Less;
    break;
  case CompareMode::Equal:
    compare_op = DkCompareOp_Equal;
    break;
  case CompareMode::LEqual:
    compare_op = inverted_depth ? DkCompareOp_Gequal : DkCompareOp_Lequal;
    break;
  case CompareMode::Greater:
    compare_op = inverted_depth ? DkCompareOp_Less : DkCompareOp_Greater;
    break;
  case CompareMode::NEqual:
    compare_op = DkCompareOp_NotEqual;
    break;
  case CompareMode::GEqual:
    compare_op = inverted_depth ? DkCompareOp_Lequal : DkCompareOp_Gequal;
    break;
  case CompareMode::Always:
  default:
    compare_op = DkCompareOp_Always;
    break;
  }

  DkDepthStencilState depth_stencil{};
  dkDepthStencilStateDefaults(&depth_stencil);
  depth_stencil.depthTestEnable = state.test_enable;
  depth_stencil.depthWriteEnable = state.update_enable;
  depth_stencil.stencilTestEnable = 0;
  depth_stencil.depthCompareOp = compare_op;
  return depth_stencil;
}

DkColorState GetColorState(const BlendingState& state, u32 num_attachments)
{
  DkColorState color{};
  dkColorStateDefaults(&color);

  color.blendEnableMask = 0;
  if (state.blend_enable)
  {
    for (u32 i = 0; i < num_attachments; i++)
      color.blendEnableMask |= 1u << i;
  }

  const bool logic_op_enable = state.logic_op_enable && g_backend_info.bSupportsLogicOp;

  color.logicOp = logic_op_enable ?
                      static_cast<DkLogicOp>(static_cast<u32>(state.logic_mode.Value())) :
                      DkLogicOp_Copy;

  color.alphaCompareOp = DkCompareOp_Always;
  return color;
}

DkColorWriteState GetColorWriteState(const BlendingState& state, u32 num_attachments)
{
  u32 mask = 0;
  if (state.color_update)
    mask |= DkColorMask_RGB;
  if (state.alpha_update)
    mask |= DkColorMask_A;

  DkColorWriteState color_write{};
  dkColorWriteStateDefaults(&color_write);
  color_write.masks = 0;
  for (u32 i = 0; i < num_attachments; i++)
    dkColorWriteStateSetMask(&color_write, i, mask);
  return color_write;
}

DkBlendFactor GetSrcFactor(SrcBlendFactor factor, bool dual_source)
{
  static constexpr Common::EnumMap<DkBlendFactor, SrcBlendFactor::InvDstAlpha> factors{
      DkBlendFactor_Zero,        DkBlendFactor_One,         DkBlendFactor_DstColor,
      DkBlendFactor_InvDstColor, DkBlendFactor_SrcAlpha,    DkBlendFactor_InvSrcAlpha,
      DkBlendFactor_DstAlpha,    DkBlendFactor_InvDstAlpha,
  };
  static constexpr Common::EnumMap<DkBlendFactor, SrcBlendFactor::InvDstAlpha> dual_factors{
      DkBlendFactor_Zero,        DkBlendFactor_One,         DkBlendFactor_DstColor,
      DkBlendFactor_InvDstColor, DkBlendFactor_Src1Alpha,   DkBlendFactor_InvSrc1Alpha,
      DkBlendFactor_DstAlpha,    DkBlendFactor_InvDstAlpha,
  };
  return dual_source ? dual_factors[factor] : factors[factor];
}

DkBlendFactor GetDstFactor(DstBlendFactor factor, bool dual_source)
{
  static constexpr Common::EnumMap<DkBlendFactor, DstBlendFactor::InvDstAlpha> factors{
      DkBlendFactor_Zero,        DkBlendFactor_One,         DkBlendFactor_SrcColor,
      DkBlendFactor_InvSrcColor, DkBlendFactor_SrcAlpha,    DkBlendFactor_InvSrcAlpha,
      DkBlendFactor_DstAlpha,    DkBlendFactor_InvDstAlpha,
  };
  static constexpr Common::EnumMap<DkBlendFactor, DstBlendFactor::InvDstAlpha> dual_factors{
      DkBlendFactor_Zero,        DkBlendFactor_One,         DkBlendFactor_SrcColor,
      DkBlendFactor_InvSrcColor, DkBlendFactor_Src1Alpha,   DkBlendFactor_InvSrc1Alpha,
      DkBlendFactor_DstAlpha,    DkBlendFactor_InvDstAlpha,
  };
  return dual_source ? dual_factors[factor] : factors[factor];
}

DkBlendState GetBlendState(const BlendingState& state)
{
  const bool dual_source = state.use_dual_src;

  DkBlendState blend{};
  dkBlendStateDefaults(&blend);
  blend.colorBlendOp = state.subtract ? DkBlendOp_RevSub : DkBlendOp_Add;
  blend.alphaBlendOp = state.subtract_alpha ? DkBlendOp_RevSub : DkBlendOp_Add;
  blend.srcColorBlendFactor = GetSrcFactor(state.src_factor, dual_source);
  blend.dstColorBlendFactor = GetDstFactor(state.dst_factor, dual_source);
  blend.srcAlphaBlendFactor = GetSrcFactor(state.src_factor_alpha, dual_source);
  blend.dstAlphaBlendFactor = GetDstFactor(state.dst_factor_alpha, dual_source);
  return blend;
}
}

DKPipeline::DKPipeline(const AbstractPipelineConfig& config, std::vector<u32> replay_words,
                       std::vector<std::shared_ptr<DKShaderCode>> shader_code,
                       DkPrimitive primitive, u32 stage_mask)
    : AbstractPipeline(config), m_replay_words(std::move(replay_words)),
      m_shader_code(std::move(shader_code)), m_primitive(primitive), m_stage_mask(stage_mask)
{
}

DKPipeline::~DKPipeline() = default;

void DKPipeline::Replay(DkCmdBuf cmdbuf) const
{
  dkCmdBufReplayCmds(cmdbuf, m_replay_words.data(), static_cast<u32>(m_replay_words.size()));
}

std::unique_ptr<DKPipeline> DKPipeline::Create(const AbstractPipelineConfig& config)
{
  DEBUG_ASSERT(config.vertex_shader && config.pixel_shader);

  const u32 num_attachments =
      1 + static_cast<u32>(config.framebuffer_state.additional_color_attachment_count);

  const DkRasterizerState raster = GetRasterizerState(config.rasterization_state);
  const DkMultisampleState multisample = GetMultisampleState(config.framebuffer_state);
  const DkColorState color = GetColorState(config.blending_state, num_attachments);
  const DkColorWriteState color_write = GetColorWriteState(config.blending_state, num_attachments);
  const DkDepthStencilState depth_stencil = GetDepthStencilState(config.depth_state);

  const std::vector<DkBlendState> blend_states(num_attachments,
                                               GetBlendState(config.blending_state));

  const auto* vertex_shader = static_cast<const DKShader*>(config.vertex_shader);
  const auto* geometry_shader = static_cast<const DKShader*>(config.geometry_shader);
  const auto* pixel_shader = static_cast<const DKShader*>(config.pixel_shader);

  const DkPrimitive primitive = GetDkPrimitive(config.rasterization_state.primitive.Value());

  if (!vertex_shader->IsValid() || !pixel_shader->IsValid() ||
      (geometry_shader && !geometry_shader->IsValid()))
  {
    return std::make_unique<DKPipeline>(config, std::vector<u32>{},
                                        std::vector<std::shared_ptr<DKShaderCode>>{}, primitive, 0);
  }

  std::vector<const DkShader*> shaders;
  std::vector<std::shared_ptr<DKShaderCode>> shader_code;
  u32 stage_mask = 0;
  shaders.push_back(vertex_shader->GetShader());
  shader_code.push_back(vertex_shader->GetCode());
  stage_mask |= DkStageFlag_Vertex;
  if (geometry_shader)
  {
    shaders.push_back(geometry_shader->GetShader());
    shader_code.push_back(geometry_shader->GetCode());
    stage_mask |= DkStageFlag_Geometry;
  }
  shaders.push_back(pixel_shader->GetShader());
  shader_code.push_back(pixel_shader->GetCode());
  stage_mask |= DkStageFlag_Fragment;

  const auto* vertex_format = static_cast<const DKVertexFormat*>(config.vertex_format);
  const std::array<DkVtxBufferState, 1> buffer_states = {
      vertex_format ? vertex_format->GetBufferState() : DkVtxBufferState{}};

  const bool primitive_restart =
      g_backend_info.bSupportsPrimitiveRestart && primitive == DkPrimitive_TriangleStrip;

  std::lock_guard capture_guard(g_dk_object_cache->GetPipelineCaptureMutex());
  DkCmdBuf scratch = g_dk_object_cache->GetPipelineCaptureCommandBuffer();

  constexpr u32 BLEND_STATE_WORDS = 7;
  const u32 expected_blend_words = BLEND_STATE_WORDS * num_attachments;

  std::vector<u32> blend_commands(expected_blend_words + 1);
  dkCmdBufBeginCaptureCmds(scratch, blend_commands.data(), static_cast<u32>(blend_commands.size()));
  dkCmdBufBindBlendStates(scratch, 0, blend_states.data(), static_cast<u32>(blend_states.size()));
  const u32 num_blend_words = dkCmdBufEndCaptureCmds(scratch);
  if (num_blend_words != expected_blend_words)
  {
    PanicAlertFmt("Unexpected deko3d blend command size: got {}, expected {}", num_blend_words,
                  expected_blend_words);
    return nullptr;
  }
  blend_commands.resize(num_blend_words);
  for (u32 i = 0; i < num_attachments; ++i)
  {
    blend_commands[i * BLEND_STATE_WORDS + 6] =
        GetDkBlendFactorSetting(blend_states[i].dstAlphaBlendFactor);
  }

  std::vector<u32> storage(MAX_CAPTURE_WORDS);
  dkCmdBufBeginCaptureCmds(scratch, storage.data(), static_cast<u32>(storage.size()));

  dkCmdBufBindShaders(scratch, DkStageFlag_GraphicsMask, shaders.data(),
                      static_cast<u32>(shaders.size()));
  dkCmdBufBindRasterizerState(scratch, &raster);
  dkCmdBufBindMultisampleState(scratch, &multisample);
  dkCmdBufBindColorState(scratch, &color);
  dkCmdBufBindColorWriteState(scratch, &color_write);
  dkCmdBufReplayCmds(scratch, blend_commands.data(), static_cast<u32>(blend_commands.size()));
  dkCmdBufBindDepthStencilState(scratch, &depth_stencil);
  const auto& attributes =
      vertex_format ? vertex_format->GetAttributes() : DKVertexFormat::GetUnusedAttributes();
  dkCmdBufBindVtxAttribState(scratch, attributes.data(), static_cast<u32>(attributes.size()));
  if (vertex_format)
  {
    dkCmdBufBindVtxBufferState(scratch, buffer_states.data(),
                               static_cast<u32>(buffer_states.size()));
  }
  dkCmdBufSetPrimitiveRestart(scratch, primitive_restart, 0xFFFF);

  const u32 num_words = dkCmdBufEndCaptureCmds(scratch);
  storage.resize(num_words);

  return std::make_unique<DKPipeline>(config, std::move(storage), std::move(shader_code), primitive,
                                      stage_mask);
}
}
