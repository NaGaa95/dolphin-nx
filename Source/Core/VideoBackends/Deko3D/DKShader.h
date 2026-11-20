// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

#include "VideoCommon/AbstractShader.h"

namespace Deko3D
{

class DKShaderCode
{
public:
  struct Block;

  DKShaderCode(std::shared_ptr<Block> block, u32 offset, u32 size, const DkShader& shader);
  ~DKShaderCode();

  const DkShader* GetShader() const { return &m_shader; }
  bool IsValid() const { return dkShaderIsValid(&m_shader); }

  // Before binding shaders: once code went into a range that held other code, the GPU's shader
  // cache is invalidated.
  static void PrepareForUse(DkCmdBuf cmdbuf);
  // At backend shutdown, after the command buffers have retired every shader.
  static void ReleaseMemory();

private:
  std::shared_ptr<Block> m_block;
  u32 m_offset;
  u32 m_size;
  DkShader m_shader;
};

class DKShader final : public AbstractShader
{
public:
  DKShader(ShaderStage stage, std::vector<u8> dksh, std::shared_ptr<DKShaderCode> code);
  ~DKShader() override;

  const DkShader* GetShader() const { return m_code->GetShader(); }
  bool IsValid() const { return m_code && m_code->IsValid(); }

  const std::shared_ptr<DKShaderCode>& GetCode() const { return m_code; }

  BinaryData GetBinary() const override;

  static std::unique_ptr<DKShader> CreateFromBinary(ShaderStage stage, const void* data,
                                                    size_t length, std::string_view name);
  static std::unique_ptr<DKShader> CreateFromCacheBinary(ShaderStage stage, const void* data,
                                                         size_t length, std::string_view name);

private:
  std::vector<u8> m_dksh;
  std::shared_ptr<DKShaderCode> m_code;
};
}
