// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKTexture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Deko3D/DKCommandBufferManager.h"
#include "VideoBackends/Deko3D/DKContext.h"
#include "VideoBackends/Deko3D/DKGfx.h"
#include "VideoBackends/Deko3D/DKMemoryTracker.h"
#include "VideoBackends/Deko3D/DKObjectCache.h"
#include "VideoBackends/Deko3D/DKStateTracker.h"
#include "VideoBackends/Deko3D/DKStreamBuffer.h"

namespace Deko3D
{
namespace
{
DkImageType GetDkImageType(const TextureConfig& config)
{
  if (config.type == AbstractTextureType::Texture_CubeMap)
    return DkImageType_Cubemap;

  if (config.type == AbstractTextureType::Texture_2D)
    return config.IsMultisampled() ? DkImageType_2DMS : DkImageType_2D;

  return config.IsMultisampled() ? DkImageType_2DMSArray : DkImageType_2DArray;
}

DkMsMode GetDkMsMode(u32 samples)
{
  switch (samples)
  {
  case 1:
    return DkMsMode_1x;
  case 2:
    return DkMsMode_2x;
  case 4:
    return DkMsMode_4x;
  case 8:
    return DkMsMode_8x;
  default:
    return DkMsMode_1x;
  }
}

DkTileSize PickTileSize(const TextureConfig& config)
{
  static constexpr std::array<u32, 5> samples_y = {1, 1, 2, 2, 4};
  const u32 block_size = AbstractTexture::GetBlockSizeForFormat(config.format);
  const u32 rows = Common::AlignUp(config.height, block_size) / block_size *
                   samples_y[std::countr_zero(std::max(config.samples, 1u))];

  const u32 gobs = (rows + rows / 2 + 7) / 8;
  u32 tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
  while (tile != 0 && rows <= (8u << (tile - 1)))
    --tile;

  return static_cast<DkTileSize>(tile);
}
}

DKTexture::DKTexture(const TextureConfig& config, dk::UniqueMemBlock memblock,
                     const dk::ImageLayout& layout, const dk::Image& image,
                     const DkImageDescriptor& descriptor)
    : AbstractTexture(config), m_memblock(std::move(memblock)), m_layout(layout), m_image(image),
      m_descriptor(descriptor)
{
}

DKTexture::~DKTexture()
{
  if (DKStateTracker* state_tracker = DKStateTracker::GetInstance())
    state_tracker->UnbindTexture(this);

  if (m_memblock)
    MemoryTracker::Unregister(dkImageGetGpuAddr(&m_image));

  DeferMemBlockDestruction(std::move(m_memblock));
}

DkImageFormat DKTexture::GetDkFormatForHostTextureFormat(AbstractTextureFormat format)
{
  switch (format)
  {
  case AbstractTextureFormat::DXT1:
    return DkImageFormat_RGBA_BC1;
  case AbstractTextureFormat::DXT3:
    return DkImageFormat_RGBA_BC2;
  case AbstractTextureFormat::DXT5:
    return DkImageFormat_RGBA_BC3;
  case AbstractTextureFormat::BPTC:
    return DkImageFormat_RGBA_BC7_Unorm;
  case AbstractTextureFormat::RGBA8:
    return DkImageFormat_RGBA8_Unorm;
  case AbstractTextureFormat::BGRA8:
    return DkImageFormat_BGRA8_Unorm;
  case AbstractTextureFormat::RGB10_A2:
    return DkImageFormat_RGB10A2_Unorm;
  case AbstractTextureFormat::RGBA16F:
    return DkImageFormat_RGBA16_Float;
  case AbstractTextureFormat::R16:
    return DkImageFormat_R16_Unorm;
  case AbstractTextureFormat::D16:
    return DkImageFormat_Z16;
  case AbstractTextureFormat::D24_S8:
    return DkImageFormat_Z24S8;
  case AbstractTextureFormat::R32F:
    return DkImageFormat_R32_Float;
  case AbstractTextureFormat::D32F:
    return DkImageFormat_ZF32;
  case AbstractTextureFormat::D32F_S8:
    return DkImageFormat_ZF32_X24S8;
  case AbstractTextureFormat::Undefined:
    return DkImageFormat_None;
  default:
    PanicAlertFmt("Unhandled texture format.");
    return DkImageFormat_RGBA8_Unorm;
  }
}

std::unique_ptr<DKTexture> DKTexture::Create(const TextureConfig& config, std::string_view name)
{
  DkDevice device = g_dk_context->GetDevice();

  u32 flags = DkImageFlags_CustomTileSize;
  if (!IsCompressedFormat(config.format))
    flags |= DkImageFlags_Usage2DEngine;
  if (config.IsRenderTarget())
    flags |= DkImageFlags_UsageRender | DkImageFlags_HwCompression;
  if (config.IsComputeImage())
    flags |= DkImageFlags_UsageLoadStore;

  const DkImageType type = GetDkImageType(config);

  dk::ImageLayoutMaker layout_maker{device};
  layout_maker.setType(type)
      .setFlags(flags)
      .setFormat(GetDkFormatForHostTextureFormat(config.format))
      .setMsMode(GetDkMsMode(config.samples))
      .setMipLevels(config.levels)
      .setTileSize(PickTileSize(config));
  if (type == DkImageType_2D || type == DkImageType_2DMS)
    layout_maker.setDimensions(config.width, config.height);
  else
    layout_maker.setDimensions(config.width, config.height, config.layers);

  dk::ImageLayout layout;
  layout_maker.initialize(layout);

  dk::UniqueMemBlock memblock =
      dk::MemBlockMaker{device, static_cast<u32>(Common::AlignUp(
                                    layout.getSize(),
                                    std::max<u64>(layout.getAlignment(), DK_MEMBLOCK_ALIGNMENT)))}
          .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
          .create();
  if (!memblock)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: failed to allocate image memory for '{}'", name);
    return nullptr;
  }

  dk::Image image;
  image.initialize(layout, memblock, 0);

  dk::ImageView view{image};
  DkImageDescriptor descriptor{};
  dkImageDescriptorInitialize(&descriptor, &view, config.IsComputeImage(), false);

  MemoryTracker::Register(dkImageGetGpuAddr(&image), layout.getSize(),
                          fmt::format("image '{}' {}x{}x{} fmt {} rt {}", name, config.width,
                                      config.height, config.layers, static_cast<int>(config.format),
                                      config.IsRenderTarget()));

  return std::make_unique<DKTexture>(config, std::move(memblock), layout, image, descriptor);
}

std::unique_ptr<DKTexture> DKTexture::CreateAdopted(const TextureConfig& config,
                                                    const dk::ImageLayout& layout,
                                                    const dk::Image& image)
{
  dk::ImageView view{image};
  DkImageDescriptor descriptor{};
  dkImageDescriptorInitialize(&descriptor, &view, config.IsComputeImage(), false);

  return std::make_unique<DKTexture>(config, dk::UniqueMemBlock{}, layout, image, descriptor);
}

DkImageDescriptor DKTexture::MakeBufferDescriptor(DkMemBlock memblock, DkImageFormat format,
                                                  u32 element_size)
{
  dk::ImageLayout layout;
  dk::ImageLayoutMaker{g_dk_context->GetDevice()}
      .setType(DkImageType_Buffer)
      .setFormat(format)
      .setDimensions(dkMemBlockGetSize(memblock) / element_size)
      .initialize(layout);

  dk::Image image;
  image.initialize(layout, memblock, 0);

  dk::ImageView view{image};
  DkImageDescriptor descriptor{};
  dkImageDescriptorInitialize(&descriptor, &view, false, false);
  return descriptor;
}

DkImageView DKTexture::MakeView(u32 level, u32 layer, u32 layer_count) const
{
  dk::ImageView view{m_image};
  view.setMipLevels(static_cast<u8>(level), 1);
  view.setLayers(static_cast<u16>(layer), static_cast<u16>(layer_count));
  return view;
}

void DKTexture::CopyRectangleFromTexture(const AbstractTexture* src,
                                         const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                         u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                         u32 dst_layer, u32 dst_level)
{
  const DKTexture* src_texture = static_cast<const DKTexture*>(src);

  ASSERT_MSG(VIDEO,
             static_cast<u32>(src_rect.GetWidth()) <= src_texture->GetWidth() &&
                 static_cast<u32>(src_rect.GetHeight()) <= src_texture->GetHeight(),
             "Source rect is too large for CopyRectangleFromTexture");
  ASSERT_MSG(VIDEO,
             static_cast<u32>(dst_rect.GetWidth()) <= m_config.width &&
                 static_cast<u32>(dst_rect.GetHeight()) <= m_config.height,
             "Dest rect is too large for CopyRectangleFromTexture");

  const DkImageView src_view = src_texture->MakeView(src_level, src_layer, 1);
  const DkImageView dst_view = MakeView(dst_level, dst_layer, 1);

  const DkImageRect src_image_rect{
      static_cast<u32>(src_rect.left),       static_cast<u32>(src_rect.top),         0,
      static_cast<u32>(src_rect.GetWidth()), static_cast<u32>(src_rect.GetHeight()), 1};
  const DkImageRect dst_image_rect{
      static_cast<u32>(dst_rect.left),       static_cast<u32>(dst_rect.top),         0,
      static_cast<u32>(dst_rect.GetWidth()), static_cast<u32>(dst_rect.GetHeight()), 1};

  const DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();
#ifdef __SWITCH2__
  // The copy engine runs alongside graphics on the Switch 2: the copy waits for the draws before
  // it, and what follows waits for the copy.
  dkCmdBufBarrier(cmdbuf, DkBarrier_Full, 0);
#endif
  dkCmdBufCopyImage(cmdbuf, &src_view, &src_image_rect, &dst_view, &dst_image_rect, 0);
#ifdef __SWITCH2__
  dkCmdBufBarrier(cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image);
#endif
}

void DKTexture::ResolveFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& rect,
                                   u32 layer, u32 level)
{
  const DKTexture* src_texture = static_cast<const DKTexture*>(src);
  DEBUG_ASSERT(m_config.samples == 1 && m_config.width == src_texture->m_config.width &&
               m_config.height == src_texture->m_config.height &&
               src_texture->m_config.samples > 1);

  const DkImageView src_view = src_texture->MakeView(level, layer, 1);
  const DkImageView dst_view = MakeView(level, layer, 1);

  const DkImageRect image_rect{
      static_cast<u32>(rect.left),       static_cast<u32>(rect.top),         0,
      static_cast<u32>(rect.GetWidth()), static_cast<u32>(rect.GetHeight()), 1};

  DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();

  dkCmdBufBarrier(cmdbuf, DkBarrier_Fragments, 0);

  dkCmdBufBlitImage(cmdbuf, &src_view, &image_rect, &dst_view, &image_rect, DkBlitFlag_FilterLinear,
                    0);
}

void DKTexture::Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer,
                     size_t buffer_size, u32 layer)
{
  width = std::max(1u, std::min(width, GetWidth() >> level));
  height = std::max(1u, std::min(height, GetHeight() >> level));

  const u32 block_size = AbstractTexture::GetBlockSizeForFormat(GetFormat());
  const u32 num_rows = Common::AlignUp(height, block_size) / block_size;
  const u32 source_pitch = AbstractTexture::CalculateStrideForFormat(m_config.format, row_length);
  const u32 upload_size = source_pitch * num_rows;
  if (upload_size > buffer_size)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: texture upload needs {} bytes but only {} were provided",
                  upload_size, buffer_size);
    return;
  }

  DkGpuAddr upload_addr = DK_GPU_ADDR_INVALID;
  dk::UniqueMemBlock upload;
  if (upload_size <= STAGING_TEXTURE_UPLOAD_THRESHOLD)
  {
    DKStreamBuffer* upload_buffer = g_dk_object_cache->GetTextureUploadBuffer();
    if (!upload_buffer->ReserveMemory(upload_size, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT))
    {
      WARN_LOG_FMT(VIDEO, "Executing command buffer while waiting for texture upload space");
      DKGfx::GetInstance()->ExecuteCommandBuffer(false);
      if (!upload_buffer->ReserveMemory(upload_size, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT))
      {
        PanicAlertFmt("Failed to allocate {} bytes from the texture upload ring.", upload_size);
        return;
      }
    }

    upload_addr = upload_buffer->GetCurrentGpuAddr();
    std::memcpy(upload_buffer->GetCurrentHostPointer(), buffer, upload_size);
    upload_buffer->CommitMemory(upload_size);
  }
  else
  {
    upload =
        dk::MemBlockMaker{g_dk_context->GetDevice(),
                          static_cast<u32>(Common::AlignUp(upload_size, DK_MEMBLOCK_ALIGNMENT))}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
    if (!upload)
    {
      PanicAlertFmt("Failed to allocate {} bytes for a texture upload.", upload_size);
      return;
    }

    upload_addr = upload.getGpuAddr();
    MemoryTracker::RegisterMemBlock(upload, "texture upload staging block");
    std::memcpy(upload.getCpuAddr(), buffer, upload_size);
  }

  const DkCopyBuf src{upload_addr, source_pitch, 0};
  const DkImageView dst_view = MakeView(level, layer, 1);
  const DkImageRect dst_rect{0, 0, 0, width, height, 1};

  dkCmdBufCopyBufferToImage(g_dk_command_buffer_mgr->GetCurrentInitCommandBuffer(), &src, &dst_view,
                            &dst_rect, 0);

  if (upload)
  {
    MemoryTracker::UnregisterMemBlock(upload);
    DeferMemBlockDestruction(std::move(upload));
  }
}

DKStagingTexture::DKStagingTexture(StagingTextureType type, const TextureConfig& config,
                                   dk::UniqueMemBlock memblock)
    : AbstractStagingTexture(type, config), m_memblock(std::move(memblock))
{
  m_map_pointer = static_cast<char*>(m_memblock.getCpuAddr());
  m_map_stride = config.GetStride();
}

DKStagingTexture::~DKStagingTexture()
{
  MemoryTracker::UnregisterMemBlock(m_memblock);
  DeferMemBlockDestruction(std::move(m_memblock));
}

std::unique_ptr<DKStagingTexture> DKStagingTexture::Create(StagingTextureType type,
                                                           const TextureConfig& config)
{
  const size_t buffer_size = config.GetStride() * static_cast<size_t>(config.height);

  const u32 cpu_access =
      type == StagingTextureType::Upload ? DkMemBlockFlags_CpuCached : DkMemBlockFlags_CpuUncached;

  dk::UniqueMemBlock memblock =
      dk::MemBlockMaker{g_dk_context->GetDevice(),
                        static_cast<u32>(Common::AlignUp(buffer_size, DK_MEMBLOCK_ALIGNMENT))}
          .setFlags(cpu_access | DkMemBlockFlags_GpuCached)
          .create();
  if (!memblock)
  {
    ERROR_LOG_FMT(VIDEO, "deko3d: failed to allocate a {} byte staging texture", buffer_size);
    return nullptr;
  }

  MemoryTracker::RegisterMemBlock(memblock,
                                  fmt::format("staging texture {}x{} type {}", config.width,
                                              config.height, static_cast<int>(type)));

  return std::make_unique<DKStagingTexture>(type, config, std::move(memblock));
}

void DKStagingTexture::CopyFromTexture(const AbstractTexture* src,
                                       const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                       u32 src_level, const MathUtil::Rectangle<int>& dst_rect)
{
  const DKTexture* src_tex = static_cast<const DKTexture*>(src);
  ASSERT(m_type == StagingTextureType::Readback || m_type == StagingTextureType::Mutable);
  ASSERT(src_rect.GetWidth() == dst_rect.GetWidth() &&
         src_rect.GetHeight() == dst_rect.GetHeight());

  DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();

  const DkImageView src_view = src_tex->MakeView(src_level, src_layer, 1);
  const DkImageRect src_image_rect{
      static_cast<u32>(src_rect.left),       static_cast<u32>(src_rect.top),         0,
      static_cast<u32>(src_rect.GetWidth()), static_cast<u32>(src_rect.GetHeight()), 1};

  const u32 offset = static_cast<u32>(static_cast<size_t>(dst_rect.top) * m_config.GetStride() +
                                      static_cast<size_t>(dst_rect.left) * m_texel_size);
  const DkCopyBuf dst{m_memblock.getGpuAddr() + offset, static_cast<u32>(m_config.GetStride()), 0};

#ifdef __SWITCH2__
  // (the copy engine runs alongside graphics on the Switch 2)
  dkCmdBufBarrier(cmdbuf, DkBarrier_Full, 0);
#endif
  dkCmdBufCopyImageToBuffer(cmdbuf, &src_view, &src_image_rect, &dst, 0);

#ifdef __SWITCH2__
  dkCmdBufBarrier(cmdbuf, DkBarrier_Full, DkInvalidateFlags_L2Cache);
#else
  const u32 threed_nop = 0x80000040;
  dkCmdBufReplayCmds(cmdbuf, &threed_nop, 1);
  dkCmdBufBarrier(cmdbuf, DkBarrier_None, DkInvalidateFlags_L2Cache);
#endif
  // The command buffer's fence then waits for the copy, before Flush() lets the CPU read it.
  g_dk_command_buffer_mgr->NotifyCpuReadback();

  m_needs_flush = true;
  m_flush_fence_counter = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
}

void DKStagingTexture::CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                                     u32 dst_level)
{
  const DKTexture* dst_tex = static_cast<const DKTexture*>(dst);
  ASSERT(m_type == StagingTextureType::Upload || m_type == StagingTextureType::Mutable);
  ASSERT(src_rect.GetWidth() == dst_rect.GetWidth() &&
         src_rect.GetHeight() == dst_rect.GetHeight());

  if (m_type == StagingTextureType::Upload)
    m_memblock.flushCpuCache(0, m_memblock.getSize());

  DkCmdBuf cmdbuf = g_dk_command_buffer_mgr->GetCurrentCommandBuffer();

  const u32 offset = static_cast<u32>(static_cast<size_t>(src_rect.top) * m_config.GetStride() +
                                      static_cast<size_t>(src_rect.left) * m_texel_size);
  const DkCopyBuf src{m_memblock.getGpuAddr() + offset, static_cast<u32>(m_config.GetStride()), 0};

  const DkImageView dst_view = dst_tex->MakeView(dst_level, dst_layer, 1);
  const DkImageRect dst_image_rect{
      static_cast<u32>(dst_rect.left),       static_cast<u32>(dst_rect.top),         0,
      static_cast<u32>(dst_rect.GetWidth()), static_cast<u32>(dst_rect.GetHeight()), 1};

#ifdef __SWITCH2__
  // (the copy engine runs alongside graphics on the Switch 2)
  dkCmdBufBarrier(cmdbuf, DkBarrier_Full, 0);
#endif
  dkCmdBufCopyBufferToImage(cmdbuf, &src, &dst_view, &dst_image_rect, 0);
#ifdef __SWITCH2__
  dkCmdBufBarrier(cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image);
#endif

  m_needs_flush = true;
  m_flush_fence_counter = g_dk_command_buffer_mgr->GetCurrentFenceCounter();
}

bool DKStagingTexture::Map()
{
  return true;
}

void DKStagingTexture::Unmap()
{
}

void DKStagingTexture::Flush()
{
  if (!m_needs_flush)
    return;

  if (g_dk_command_buffer_mgr->GetCurrentFenceCounter() == m_flush_fence_counter)
    g_dk_command_buffer_mgr->SubmitCommandBuffer(true);
  else
    g_dk_command_buffer_mgr->WaitForFenceCounter(m_flush_fence_counter);

  m_needs_flush = false;
}

DKFramebuffer::DKFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                             std::vector<AbstractTexture*> additional_color_attachments,
                             AbstractTextureFormat color_format, AbstractTextureFormat depth_format,
                             u32 width, u32 height, u32 layers, u32 samples)
    : AbstractFramebuffer(color_attachment, depth_attachment,
                          std::move(additional_color_attachments), color_format, depth_format,
                          width, height, layers, samples)
{
}

DKFramebuffer::~DKFramebuffer()
{
  if (DKGfx* gfx = DKGfx::GetInstance())
    gfx->OnFramebufferDestroyed(this);
  if (DKStateTracker* state_tracker = DKStateTracker::GetInstance())
    state_tracker->UnbindFramebuffer(this);
}

std::unique_ptr<DKFramebuffer>
DKFramebuffer::Create(DKTexture* color_attachment, DKTexture* depth_attachment,
                      std::vector<AbstractTexture*> additional_color_attachments)
{
  if (!ValidateConfig(color_attachment, depth_attachment, additional_color_attachments))
    return nullptr;

  const AbstractTextureFormat color_format =
      color_attachment ? color_attachment->GetFormat() : AbstractTextureFormat::Undefined;
  const AbstractTextureFormat depth_format =
      depth_attachment ? depth_attachment->GetFormat() : AbstractTextureFormat::Undefined;
  const DKTexture* either_attachment = color_attachment ? color_attachment : depth_attachment;
  const u32 width = either_attachment->GetWidth();
  const u32 height = either_attachment->GetHeight();
  const u32 layers = either_attachment->GetLayers();
  const u32 samples = either_attachment->GetSamples();

  return std::make_unique<DKFramebuffer>(color_attachment, depth_attachment,
                                         std::move(additional_color_attachments), color_format,
                                         depth_format, width, height, layers, samples);
}

void DKFramebuffer::Unbind() const
{
  DKStateTracker* state_tracker = DKStateTracker::GetInstance();
  if (!state_tracker)
    return;

  if (m_color_attachment)
    state_tracker->UnbindTexture(static_cast<const DKTexture*>(m_color_attachment));
  if (m_depth_attachment)
    state_tracker->UnbindTexture(static_cast<const DKTexture*>(m_depth_attachment));
  for (const AbstractTexture* attachment : m_additional_color_attachments)
    state_tracker->UnbindTexture(static_cast<const DKTexture*>(attachment));
}

void DKFramebuffer::Bind(DkCmdBuf cmdbuf) const
{
  std::vector<DkImageView> color_views;
  std::vector<const DkImageView*> color_target_ptrs;
  color_views.reserve(1 + m_additional_color_attachments.size());

  const auto add_color = [&](AbstractTexture* attachment) {
    color_views.push_back(static_cast<DKTexture*>(attachment)->MakeView(0, 0, GetLayers()));
  };

  if (m_color_attachment)
    add_color(m_color_attachment);
  for (auto* attachment : m_additional_color_attachments)
    add_color(attachment);

  for (const DkImageView& view : color_views)
    color_target_ptrs.push_back(&view);

  DkImageView depth_view;
  if (m_depth_attachment)
    depth_view = static_cast<DKTexture*>(m_depth_attachment)->MakeView(0, 0, GetLayers());

  dkCmdBufBindRenderTargets(cmdbuf, color_target_ptrs.data(),
                            static_cast<u32>(color_target_ptrs.size()),
                            m_depth_attachment ? &depth_view : nullptr);
}
}
