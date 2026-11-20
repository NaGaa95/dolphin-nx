// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKShader.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>

#include "Common/Align.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/Deko3D/Constants.h"
#include "VideoBackends/Deko3D/DKCommandBufferManager.h"
#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKMemoryTracker.h"

namespace Deko3D
{
namespace
{
constexpr u32 DKSH_MAGIC = 0x48534B44;
constexpr u32 DKSH_CACHE_MAGIC = 0x434B4444;
constexpr u32 DKSH_CACHE_VERSION = 1;

struct DkshHeader
{
  u32 magic;
  u32 header_sz;
  u32 control_sz;
  u32 code_sz;
  u32 programs_off;
  u32 num_programs;
};

struct DkshCacheFooter
{
  u32 magic;
  u32 version;
};

struct DkshProgramHeader
{
  u32 type;
  u32 entrypoint;
  u32 num_gprs;
  u32 constbuf1_off;
  u32 constbuf1_sz;
  u32 per_warp_scratch_sz;
};
constexpr u32 DKSH_PROGRAM_HEADER_SIZE = 64;

bool ScratchMemoryFits(const u8* control, const DkshHeader& header, std::string_view name)
{
  if (header.programs_off > header.control_sz ||
      (header.control_sz - header.programs_off) / DKSH_PROGRAM_HEADER_SIZE < header.num_programs)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: '{}' has a truncated DKSH program table", name);
    return false;
  }

  for (u32 i = 0; i < header.num_programs; i++)
  {
    DkshProgramHeader program;
    std::memcpy(&program, control + header.programs_off + i * DKSH_PROGRAM_HEADER_SIZE,
                sizeof(program));
    if (program.per_warp_scratch_sz > PER_WARP_SCRATCH_MEMORY_SIZE)
    {
      ERROR_LOG_FMT(VIDEO,
                    "deko3d: '{}' spills {} bytes per warp, more than the {} the queue reserves",
                    name, program.per_warp_scratch_sz, PER_WARP_SCRATCH_MEMORY_SIZE);
      return false;
    }
  }

  return true;
}

// Shaders share large code blocks: deko3d never gives a code memblock's address space back
// (CodeSegMgr::freeSpace), and each code memblock takes a big page (on the Switch 2 also a
// mapping), which made loading a shader cache slow and a long session run out of code space.
constexpr u32 CODE_BLOCK_SIZE = 2 * 1024 * 1024;

std::mutex s_code_mutex;
std::vector<std::shared_ptr<DKShaderCode::Block>> s_code_blocks;
// Set when code goes into a range that held other code, which the GPU may still have cached.
std::atomic<bool> s_shader_cache_stale{false};
}

struct DKShaderCode::Block
{
  dk::UniqueMemBlock memory;
  // Free ranges by offset (the unusable tail is never in them).
  std::map<u32, u32> free_ranges;
  // The end of the furthest range handed out.
  u32 high_water = 0;
};

namespace
{
bool AllocateCode(u32 size, std::shared_ptr<DKShaderCode::Block>* out_block, u32* out_offset)
{
  const u32 bytes = Common::AlignUp(size, DK_SHADER_CODE_ALIGNMENT);
  const auto take = [&](const std::shared_ptr<DKShaderCode::Block>& block) {
    for (auto it = block->free_ranges.begin(); it != block->free_ranges.end(); ++it)
    {
      if (it->second < bytes)
        continue;
      const u32 offset = it->first;
      const u32 rest = it->second - bytes;
      block->free_ranges.erase(it);
      if (rest != 0)
        block->free_ranges.emplace(offset + bytes, rest);
      if (offset < block->high_water)
        s_shader_cache_stale.store(true, std::memory_order_release);
      block->high_water = std::max(block->high_water, offset + bytes);
      *out_block = block;
      *out_offset = offset;
      return true;
    }
    return false;
  };

  std::lock_guard lock(s_code_mutex);
  for (const auto& block : s_code_blocks)
  {
    if (take(block))
      return true;
  }

  const u32 block_size = static_cast<u32>(Common::AlignUp(
      std::max<u32>(CODE_BLOCK_SIZE, bytes + DK_SHADER_CODE_UNUSABLE_SIZE), DK_MEMBLOCK_ALIGNMENT));
  dk::UniqueMemBlock memory =
      dk::MemBlockMaker{g_dk_context->GetDevice(), block_size}
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code)
          .create();
  if (!memory)
    return false;
  MemoryTracker::RegisterMemBlock(memory, "shader code");
  const u32 usable = block_size - DK_SHADER_CODE_UNUSABLE_SIZE;
  std::memset(static_cast<u8*>(memory.getCpuAddr()) + usable, 0, DK_SHADER_CODE_UNUSABLE_SIZE);

  auto block = std::make_shared<DKShaderCode::Block>();
  block->memory = std::move(memory);
  block->free_ranges.emplace(0, usable);
  s_code_blocks.push_back(block);
  return take(block);
}

void FreeCode(DKShaderCode::Block& block, u32 offset, u32 size)
{
  u32 start = offset;
  u32 end = offset + Common::AlignUp(size, DK_SHADER_CODE_ALIGNMENT);

  std::lock_guard lock(s_code_mutex);
  auto next = block.free_ranges.lower_bound(offset);
  if (next != block.free_ranges.begin())
  {
    const auto previous = std::prev(next);
    if (previous->first + previous->second == start)
    {
      start = previous->first;
      block.free_ranges.erase(previous);
    }
  }
  if (next != block.free_ranges.end() && next->first == end)
  {
    end += next->second;
    block.free_ranges.erase(next);
  }
  block.free_ranges.emplace(start, end - start);
}
}

DKShaderCode::DKShaderCode(std::shared_ptr<Block> block, u32 offset, u32 size,
                           const DkShader& shader)
    : m_block(std::move(block)), m_offset(offset), m_size(size), m_shader(shader)
{
}

DKShaderCode::~DKShaderCode()
{
  // The command buffers recorded so far may still run this code.
  auto release = [block = std::move(m_block), offset = m_offset, size = m_size] {
    FreeCode(*block, offset, size);
  };
  if (g_dk_command_buffer_mgr)
    g_dk_command_buffer_mgr->DeferCleanup(std::move(release));
  else
    release();
}

void DKShaderCode::PrepareForUse(DkCmdBuf cmdbuf)
{
  if (s_shader_cache_stale.load(std::memory_order_relaxed) &&
      s_shader_cache_stale.exchange(false, std::memory_order_acq_rel))
  {
    dkCmdBufBarrier(cmdbuf, DkBarrier_None, DkInvalidateFlags_Shader | DkInvalidateFlags_L2Cache);
  }
}

void DKShaderCode::ReleaseMemory()
{
  std::lock_guard lock(s_code_mutex);
  for (const auto& block : s_code_blocks)
  {
    MemoryTracker::UnregisterMemBlock(block->memory);
    DeferMemBlockDestruction(std::move(block->memory));
  }
  s_code_blocks.clear();
  s_shader_cache_stale.store(false, std::memory_order_relaxed);
}

DKShader::DKShader(ShaderStage stage, std::vector<u8> dksh, std::shared_ptr<DKShaderCode> code)
    : AbstractShader(stage), m_dksh(std::move(dksh)), m_code(std::move(code))
{
}

DKShader::~DKShader() = default;

AbstractShader::BinaryData DKShader::GetBinary() const
{
  BinaryData binary = m_dksh;
  const DkshCacheFooter footer{DKSH_CACHE_MAGIC, DKSH_CACHE_VERSION};
  const u8* footer_bytes = reinterpret_cast<const u8*>(&footer);
  binary.insert(binary.end(), footer_bytes, footer_bytes + sizeof(footer));
  return binary;
}

std::unique_ptr<DKShader> DKShader::CreateFromBinary(ShaderStage stage, const void* data,
                                                     size_t length, std::string_view name)
{
  if (length < sizeof(DkshHeader))
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: DKSH blob for '{}' is too small ({} bytes)", name, length);
    return nullptr;
  }

  std::vector<u8> dksh(length);
  std::memcpy(dksh.data(), data, length);

  DkshHeader header;
  std::memcpy(&header, dksh.data(), sizeof(header));
  if (header.magic != DKSH_MAGIC ||
      static_cast<size_t>(header.control_sz) + header.code_sz > length)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: '{}' is not a valid DKSH blob", name);
    return nullptr;
  }

  if (!ScratchMemoryFits(dksh.data(), header, name))
    return nullptr;

  std::shared_ptr<DKShaderCode::Block> block;
  u32 offset = 0;
  if (!AllocateCode(header.code_sz, &block, &offset))
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: failed to allocate {} bytes of shader code memory",
                  header.code_sz);
    return nullptr;
  }

  std::memcpy(static_cast<u8*>(block->memory.getCpuAddr()) + offset,
              dksh.data() + header.control_sz, header.code_sz);

  DkShaderMaker maker{};
  dkShaderMakerDefaults(&maker, block->memory, offset);
  maker.control = dksh.data();

  DkShader shader{};
  dkShaderInitialize(&shader, &maker);
  if (!dkShaderIsValid(&shader))
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: dkShaderInitialize rejected '{}'", name);
    FreeCode(*block, offset, header.code_sz);
    return nullptr;
  }

  return std::make_unique<DKShader>(
      stage, std::move(dksh),
      std::make_shared<DKShaderCode>(std::move(block), offset, header.code_sz, shader));
}

std::unique_ptr<DKShader> DKShader::CreateFromCacheBinary(ShaderStage stage, const void* data,
                                                          size_t length, std::string_view name)
{
  if (length < sizeof(DkshCacheFooter))
    return nullptr;

  DkshCacheFooter footer;
  std::memcpy(&footer, static_cast<const u8*>(data) + length - sizeof(footer), sizeof(footer));
  if (footer.magic != DKSH_CACHE_MAGIC || footer.version != DKSH_CACHE_VERSION)
    return nullptr;

  return CreateFromBinary(stage, data, length - sizeof(footer), name);
}
}
