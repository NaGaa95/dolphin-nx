// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKBoundingBox.h"

#include <cstring>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Deko3D/DKCommandBufferManager.h"
#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKGfx.h"
#include "VideoBackends/Deko3D/DKMemoryTracker.h"
#include "VideoBackends/Deko3D/DKStateTracker.h"
#include "VideoBackends/Deko3D/DKStreamBuffer.h"

namespace Deko3D
{
DKBoundingBox::DKBoundingBox() = default;

DKBoundingBox::~DKBoundingBox() = default;

bool DKBoundingBox::Initialize()
{
  const u32 allocation_size = Common::AlignUp(BUFFER_SIZE, DK_MEMBLOCK_ALIGNMENT);
  m_gpu_buffer = dk::MemBlockMaker{g_dk_context->GetDevice(), allocation_size}
                     .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_ZeroFillInit)
                     .create();
  m_readback_buffer = dk::MemBlockMaker{g_dk_context->GetDevice(), allocation_size}
                          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                    DkMemBlockFlags_ZeroFillInit)
                          .create();
  m_upload_buffer = DKStreamBuffer::Create(UPLOAD_BUFFER_SIZE, "bounding box upload");
  if (!m_gpu_buffer || !m_readback_buffer || !m_upload_buffer)
  {
    PanicAlertFmt("Failed to allocate the deko3d bounding box buffers.");
    return false;
  }

  MemoryTracker::RegisterMemBlock(m_gpu_buffer, "bounding box");
  MemoryTracker::RegisterMemBlock(m_readback_buffer, "bounding box readback");

  m_readback_pointer = static_cast<BBoxType*>(m_readback_buffer.getCpuAddr());
  DKStateTracker::GetInstance()->SetSSBO(m_gpu_buffer.getGpuAddr(), BUFFER_SIZE);
  return true;
}

std::vector<BBoxType> DKBoundingBox::Read(u32 index, u32 length)
{
  ASSERT(index + length <= NUM_BBOX_VALUES);

  DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();
#ifdef __SWITCH2__
  // (the copy engine runs alongside graphics on the Switch 2: wait for the shaders' atomics)
  dkCmdBufBarrier(cmdbuf, DkBarrier_Full, 0);
#endif
  dkCmdBufCopyBuffer(cmdbuf, m_gpu_buffer.getGpuAddr(), m_readback_buffer.getGpuAddr(),
                     BUFFER_SIZE);

  const u32 threed_nop = 0x80000040;
  dkCmdBufReplayCmds(cmdbuf, &threed_nop, 1);
  dkCmdBufBarrier(cmdbuf, DkBarrier_None, DkInvalidateFlags_L2Cache);
  g_dk_command_buffer_mgr->NotifyCpuReadback();
  DKGfx::GetInstance()->ExecuteCommandBuffer(true);

  std::vector<BBoxType> values(length);
  std::memcpy(values.data(), m_readback_pointer + index, length * sizeof(BBoxType));
  return values;
}

void DKBoundingBox::Write(u32 index, std::span<const BBoxType> values)
{
  ASSERT(index + values.size() <= NUM_BBOX_VALUES);

  const u32 size = static_cast<u32>(values.size_bytes());
  if (!m_upload_buffer->ReserveMemory(size, alignof(BBoxType)))
  {
    WARN_LOG_FMT(VIDEO, "Executing command buffer while waiting for bounding box upload space");
    DKGfx::GetInstance()->ExecuteCommandBuffer(false);
    if (!m_upload_buffer->ReserveMemory(size, alignof(BBoxType)))
    {
      PanicAlertFmt("Failed to allocate bounding box upload space.");
      return;
    }
  }

  const DkGpuAddr upload_addr = m_upload_buffer->GetCurrentGpuAddr();
  std::memcpy(m_upload_buffer->GetCurrentHostPointer(), values.data(), size);
  m_upload_buffer->CommitMemory(size);

  const DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();
#ifdef __SWITCH2__
  // (the copy engine runs alongside graphics on the Switch 2: the shaders' atomics and the reset
  // values stay in order)
  dkCmdBufBarrier(cmdbuf, DkBarrier_Full, 0);
#endif
  dkCmdBufCopyBuffer(cmdbuf, upload_addr, m_gpu_buffer.getGpuAddr() + index * sizeof(BBoxType),
                     size);
#ifdef __SWITCH2__
  dkCmdBufBarrier(cmdbuf, DkBarrier_Full, DkInvalidateFlags_L2Cache);
#endif
}
}
