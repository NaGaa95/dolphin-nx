// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKCommandBufferManager.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKMemoryTracker.h"
#include "VideoBackends/Deko3D/DKSwapChain.h"
#include "VideoCommon/Statistics.h"

namespace Deko3D
{
std::unique_ptr<DKCommandBufferManager> g_dk_command_buffer_mgr;

void DKCommandBufferManager::CommandMemory::Rewind()
{
  cmdbuf.clear();
  cmdbuf.addMemory(initial_block, initial_offset, initial_size);
  next_growth_chunk = 0;
}

void DKCommandBufferManager::CommandMemory::Grow(size_t min_req_size)
{
  const u32 size = static_cast<u32>(Common::AlignUp(
      std::max<size_t>(min_req_size, COMMAND_BUFFER_GROWTH_SIZE), DK_MEMBLOCK_ALIGNMENT));

  if (next_growth_chunk == growth_chunks.size())
    growth_chunks.push_back(std::make_unique<GrowthChunk>());

  GrowthChunk& chunk = *growth_chunks[next_growth_chunk];
  if (!chunk.block || chunk.size < size)
  {
    const auto create = [size] {
      return dk::MemBlockMaker{g_dk_context->GetDevice(), size}
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
          .create();
    };
    dk::UniqueMemBlock block = create();
    if (!block)
    {
      // deko3d would record through a null block: free the growth chunks of the command buffers
      // the GPU is done with, then try once more.
      g_dk_command_buffer_mgr->ReleaseIdleGrowthChunks();
      block = create();
    }
    if (!block)
    {
      PanicAlertFmt("Out of memory for {} bytes of deko3d command memory.", size);
      std::abort();
    }
    if (chunk.block)
      MemoryTracker::UnregisterMemBlock(chunk.block);
    chunk.block = std::move(block);
    chunk.size = size;
    MemoryTracker::RegisterMemBlock(chunk.block, "command memory growth chunk");
  }

  cmdbuf.addMemory(chunk.block, 0, chunk.size);
  ++next_growth_chunk;
}

DKCommandBufferManager::DKCommandBufferManager() = default;

DKCommandBufferManager::~DKCommandBufferManager()
{
  if (g_dk_context)
    g_dk_context->WaitIdle();

  for (CmdBufferResources& resources : m_command_buffers)
  {
    for (auto& cleanup : resources.cleanup_resources)
      cleanup();
    resources.cleanup_resources.clear();
  }
}

bool DKCommandBufferManager::Initialize()
{
  if (!CreateCommandBuffers())
    return false;

  m_command_buffers[0].fence_counter = m_next_fence_counter++;
  return true;
}

void DKCommandBufferManager::AddMemoryCallback(void* user_data, DkCmdBuf, size_t min_req_size)
{
  static_cast<CommandMemory*>(user_data)->Grow(min_req_size);
}

bool DKCommandBufferManager::CreateCommandBuffers()
{
  DkDevice device = g_dk_context->GetDevice();

  const u32 total_size = static_cast<u32>(Common::AlignUp(
      static_cast<u64>(INIT_COMMAND_BUFFER_SIZE + DRAW_COMMAND_BUFFER_SIZE) * NUM_COMMAND_BUFFERS,
      DK_MEMBLOCK_ALIGNMENT));

  m_command_memory = dk::MemBlockMaker{device, total_size}
                         .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                         .create();
  if (!m_command_memory)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: failed to allocate {} bytes of command memory", total_size);
    return false;
  }

  MemoryTracker::RegisterMemBlock(m_command_memory, "command memory");

  u32 offset = 0;
  const auto create = [&](CommandMemory& mem, u32 size) {
    mem.cmdbuf = dk::CmdBufMaker{device}
                     .setUserData(&mem)
                     .setCbAddMem(&DKCommandBufferManager::AddMemoryCallback)
                     .create();
    if (!mem.cmdbuf)
      return false;

    mem.initial_block = m_command_memory;
    mem.initial_offset = offset;
    mem.initial_size = size;
    mem.cmdbuf.addMemory(m_command_memory, offset, size);
    offset += size;
    return true;
  };

  for (CmdBufferResources& resources : m_command_buffers)
  {
    if (!create(resources.init, INIT_COMMAND_BUFFER_SIZE) ||
        !create(resources.draw, DRAW_COMMAND_BUFFER_SIZE))
    {
      ERROR_LOG_FMT(VIDEO, "deko3d: failed to create command buffer");
      return false;
    }
  }

  return true;
}

DkCmdBuf DKCommandBufferManager::GetCurrentInitCommandBuffer()
{
  CmdBufferResources& resources = m_command_buffers[m_current_cmd_buffer];
#ifdef __SWITCH2__
  // (its uploads run on the copy engine: they must not overwrite what earlier work still reads)
  if (!resources.init_cmdbuf_used)
    dkCmdBufBarrier(resources.init.cmdbuf, DkBarrier_Full, 0);
#endif
  resources.init_cmdbuf_used = true;
  return resources.init.cmdbuf;
}

DkCmdBuf DKCommandBufferManager::GetCurrentCommandBuffer() const
{
  return m_command_buffers[m_current_cmd_buffer].draw.cmdbuf;
}

u64 DKCommandBufferManager::GetCurrentFenceCounter() const
{
  return m_command_buffers[m_current_cmd_buffer].fence_counter;
}

void DKCommandBufferManager::DeferCleanup(std::function<void()> cleanup)
{
  m_command_buffers[m_current_cmd_buffer].cleanup_resources.push_back(std::move(cleanup));
}

void DKCommandBufferManager::NotifyCpuReadback()
{
  m_command_buffers[m_current_cmd_buffer].needs_cpu_readback = true;
}

void DKCommandBufferManager::SubmitCommandBuffer(bool wait_for_completion,
                                                 DKSwapChain* present_swap_chain, int present_slot)
{
  CmdBufferResources& resources = m_command_buffers[m_current_cmd_buffer];
  dk::Queue queue = g_dk_context->GetGraphicsQueue();

  if (resources.init_cmdbuf_used)
  {
#ifdef __SWITCH2__
    // The uploads run on the copy engine, which the Switch 2's Ampere runs alongside graphics.
    dkCmdBufBarrier(resources.init.cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image);
#endif
    queue.submitCommands(resources.init.cmdbuf.finishList());
  }

  queue.submitCommands(resources.draw.cmdbuf.finishList());

  queue.signalFence(resources.fence, resources.needs_cpu_readback);

  if (present_swap_chain)
    present_swap_chain->Present(present_slot);
  else
    queue.flush();

  if (wait_for_completion)
    WaitForCommandBufferCompletion(m_current_cmd_buffer);

  BeginCommandBuffer();
}

void DKCommandBufferManager::BeginCommandBuffer()
{
  const u32 next_buffer_index = (m_current_cmd_buffer + 1) % NUM_COMMAND_BUFFERS;
  CmdBufferResources& resources = m_command_buffers[next_buffer_index];

  if (resources.fence_counter > m_completed_fence_counter)
    WaitForCommandBufferCompletion(next_buffer_index);

  resources.init.Rewind();
  resources.draw.Rewind();
  resources.init_cmdbuf_used = false;
  resources.needs_cpu_readback = false;
  resources.fence_counter = m_next_fence_counter++;
  m_current_cmd_buffer = next_buffer_index;
}

void DKCommandBufferManager::WaitForCommandBufferCompletion(u32 index)
{
  CmdBufferResources& resources = m_command_buffers[index];

  const DkResult res = dkFenceWait(&resources.fence, -1);
  if (res != DkResult_Success)
    ERROR_LOG_FMT(VIDEO, "deko3d: dkFenceWait failed ({})", static_cast<int>(res));

  const u64 now_completed_counter = resources.fence_counter;
  for (u32 retired_index = 0; retired_index < NUM_COMMAND_BUFFERS; ++retired_index)
  {
    CmdBufferResources& retired = m_command_buffers[retired_index];
    if (retired.fence_counter > now_completed_counter)
      continue;

    for (auto& cleanup : retired.cleanup_resources)
      cleanup();
    retired.cleanup_resources.clear();
  }

  m_completed_fence_counter = now_completed_counter;
}

void DKCommandBufferManager::ReleaseIdleGrowthChunks()
{
  for (u32 i = 0; i < NUM_COMMAND_BUFFERS; ++i)
  {
    CmdBufferResources& resources = m_command_buffers[i];
    if (i == m_current_cmd_buffer || resources.fence_counter > m_completed_fence_counter)
      continue;
    // (Rewind gives the command buffer only its initial block again before it records)
    for (CommandMemory* memory : {&resources.init, &resources.draw})
    {
      for (const auto& chunk : memory->growth_chunks)
      {
        if (!chunk->block)
          continue;
        MemoryTracker::UnregisterMemBlock(chunk->block);
        chunk->block = {};
        chunk->size = 0;
      }
    }
  }
}

void DKCommandBufferManager::WaitForFenceCounter(u64 fence_counter)
{
  if (m_completed_fence_counter >= fence_counter)
    return;

  if (fence_counter >= m_command_buffers[m_current_cmd_buffer].fence_counter)
  {
    SubmitCommandBuffer(true);
    return;
  }

  u32 index = (m_current_cmd_buffer + 1) % NUM_COMMAND_BUFFERS;
  while (index != m_current_cmd_buffer)
  {
    if (m_command_buffers[index].fence_counter >= fence_counter)
      break;

    index = (index + 1) % NUM_COMMAND_BUFFERS;
  }

  ASSERT(index != m_current_cmd_buffer);
  WaitForCommandBufferCompletion(index);
}

void DeferMemBlockDestruction(dk::UniqueMemBlock block)
{
  if (!block)
    return;

  if (g_dk_command_buffer_mgr)
  {
    auto shared = std::make_shared<dk::UniqueMemBlock>(std::move(block));
    g_dk_command_buffer_mgr->DeferCleanup([shared]() {});
  }

}
}
