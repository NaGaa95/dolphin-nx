// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VKGfx.h"

#include <cstddef>
#include <cstdio>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKPipeline.h"
#include "VideoBackends/Vulkan/VKShader.h"
#include "VideoBackends/Vulkan/VKSwapChain.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VKVertexFormat.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoConfig.h"

namespace Vulkan
{
VKGfx::VKGfx(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale)
    : m_swap_chain(std::move(swap_chain)), m_backbuffer_scale(backbuffer_scale)
{
  UpdateActiveConfig();
  for (SamplerState& sampler_state : m_sampler_states)
    sampler_state = RenderState::GetPointSamplerState();

  // Various initialization routines will have executed commands on the command buffer.
  // Execute what we have done before beginning the first frame.
  ExecuteCommandBuffer(true, false);
}

VKGfx::~VKGfx()
{
#ifdef __SWITCH2__
  CloseGuestScreen();
#endif
}

bool VKGfx::IsHeadless() const
{
  return m_swap_chain == nullptr;
}

std::unique_ptr<AbstractTexture> VKGfx::CreateTexture(const TextureConfig& config,
                                                      std::string_view name)
{
  return VKTexture::Create(config, name);
}

std::unique_ptr<AbstractStagingTexture> VKGfx::CreateStagingTexture(StagingTextureType type,
                                                                    const TextureConfig& config)
{
  return VKStagingTexture::Create(type, config);
}

std::unique_ptr<AbstractShader>
VKGfx::CreateShaderFromSource(ShaderStage stage, std::string_view source,
                              VideoCommon::ShaderIncluder* shader_includer, std::string_view name)
{
  return VKShader::CreateFromSource(stage, source, shader_includer, name);
}

std::unique_ptr<AbstractShader> VKGfx::CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                              size_t length, std::string_view name)
{
  return VKShader::CreateFromBinary(stage, data, length, name);
}

std::unique_ptr<NativeVertexFormat>
VKGfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return std::make_unique<VertexFormat>(vtx_decl);
}

std::unique_ptr<AbstractPipeline> VKGfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                        const void* cache_data,
                                                        size_t cache_data_length)
{
  return VKPipeline::Create(config);
}

std::unique_ptr<AbstractFramebuffer>
VKGfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                         std::vector<AbstractTexture*> additional_color_attachments)
{
  return VKFramebuffer::Create(static_cast<VKTexture*>(color_attachment),
                               static_cast<VKTexture*>(depth_attachment),
                               std::move(additional_color_attachments));
}

void VKGfx::SetPipeline(const AbstractPipeline* pipeline)
{
  StateTracker::GetInstance()->SetPipeline(static_cast<const VKPipeline*>(pipeline));
}

void VKGfx::ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool color_enable,
                        bool alpha_enable, bool z_enable, u32 color, u32 z)
{
  VkRect2D target_vk_rc = {
      {target_rc.left, target_rc.top},
      {static_cast<uint32_t>(target_rc.GetWidth()), static_cast<uint32_t>(target_rc.GetHeight())}};

  // Convert RGBA8 -> floating-point values.
  VkClearValue clear_color_value = {};
  VkClearValue clear_depth_value = {};
  clear_color_value.color.float32[0] = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
  clear_color_value.color.float32[1] = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
  clear_color_value.color.float32[2] = static_cast<float>((color >> 0) & 0xFF) / 255.0f;
  clear_color_value.color.float32[3] = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
  clear_depth_value.depthStencil.depth = static_cast<float>(z & 0xFFFFFF) / 16777216.0f;
  if (!g_backend_info.bSupportsReversedDepthRange)
    clear_depth_value.depthStencil.depth = 1.0f - clear_depth_value.depthStencil.depth;

  // If we're not in a render pass (start of the frame), we can use a clear render pass
  // to discard the data, rather than loading and then clearing.
  bool use_clear_attachments = (color_enable && alpha_enable) || z_enable;
  bool use_clear_render_pass =
      !StateTracker::GetInstance()->InRenderPass() && color_enable && alpha_enable && z_enable;

  // The NVIDIA Vulkan driver causes the GPU to lock up, or throw exceptions if MSAA is enabled,
  // a non-full clear rect is specified, and a clear loadop or vkCmdClearAttachments is used.
  if (g_ActiveConfig.iMultisamples > 1 &&
      DriverDetails::HasBug(DriverDetails::BUG_BROKEN_MSAA_CLEAR))
  {
    use_clear_render_pass = false;
    use_clear_attachments = false;
  }

  // This path cannot be used if the driver implementation doesn't guarantee pixels with no drawn
  // geometry in "this" renderpass won't be cleared
  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLEAR_LOADOP_RENDERPASS))
    use_clear_render_pass = false;

  auto* vk_frame_buffer = static_cast<VKFramebuffer*>(m_current_framebuffer);

  // Fastest path: Use a render pass to clear the buffers.
  if (use_clear_render_pass)
  {
    vk_frame_buffer->SetAndClear(target_vk_rc, clear_color_value, clear_depth_value);
    return;
  }

  // Fast path: Use vkCmdClearAttachments to clear the buffers within a render path
  // We can't use this when preserving alpha but clearing color.
  if (use_clear_attachments)
  {
    std::vector<VkClearAttachment> clear_attachments;
    bool has_color = false;
    if (color_enable && alpha_enable)
    {
      VkClearAttachment clear_attachment;
      clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      clear_attachment.colorAttachment = 0;
      clear_attachment.clearValue = clear_color_value;
      clear_attachments.push_back(std::move(clear_attachment));
      color_enable = false;
      alpha_enable = false;
      has_color = true;
    }
    if (z_enable)
    {
      VkClearAttachment clear_attachment;
      clear_attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      clear_attachment.colorAttachment = 0;
      clear_attachment.clearValue = clear_depth_value;
      clear_attachments.push_back(std::move(clear_attachment));
      z_enable = false;
    }
    if (has_color)
    {
      for (std::size_t i = 0; i < vk_frame_buffer->GetNumberOfAdditonalAttachments(); i++)
      {
        VkClearAttachment clear_attachment;
        clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear_attachment.colorAttachment = 0;
        clear_attachment.clearValue = clear_color_value;
        clear_attachments.push_back(std::move(clear_attachment));
      }
    }
    if (!clear_attachments.empty())
    {
      VkClearRect vk_rect = {target_vk_rc, 0, g_framebuffer_manager->GetEFBLayers()};
      if (!StateTracker::GetInstance()->IsWithinRenderArea(
              target_vk_rc.offset.x, target_vk_rc.offset.y, target_vk_rc.extent.width,
              target_vk_rc.extent.height))
      {
        StateTracker::GetInstance()->EndClearRenderPass();
      }
      StateTracker::GetInstance()->BeginRenderPass();

      vkCmdClearAttachments(g_command_buffer_mgr->GetCurrentCommandBuffer(),
                            static_cast<uint32_t>(clear_attachments.size()),
                            clear_attachments.data(), 1, &vk_rect);
    }
  }

  // Anything left over for the slow path?
  if (!color_enable && !alpha_enable && !z_enable)
    return;

  AbstractGfx::ClearRegion(target_rc, color_enable, alpha_enable, z_enable, color, z);
}

void VKGfx::Flush()
{
  ExecuteCommandBuffer(true, false);
}

void VKGfx::WaitForGPUIdle()
{
  ExecuteCommandBuffer(false, true);
}

bool VKGfx::BindBackbuffer(const ClearColor& clear_color)
{
  StateTracker::GetInstance()->EndRenderPass();

  g_command_buffer_mgr->WaitForWorkerThreadIdle();

  // Handle host window resizes.
  CheckForSurfaceChange();
  CheckForSurfaceResize();

  // Check for exclusive fullscreen request.
  if (m_swap_chain->GetCurrentFullscreenState() != m_swap_chain->GetNextFullscreenState() &&
      !m_swap_chain->SetFullscreenState(m_swap_chain->GetNextFullscreenState()))
  {
    // if it fails, don't keep trying
    m_swap_chain->SetNextFullscreenState(m_swap_chain->GetCurrentFullscreenState());
  }

  const bool present_fail = g_command_buffer_mgr->CheckLastPresentFail();
  VkResult res = present_fail ? g_command_buffer_mgr->GetLastPresentResult() :
                                m_swap_chain->AcquireNextImage();

  if (res == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT &&
      !m_swap_chain->GetCurrentFullscreenState())
  {
    // AMD's binary driver as of 21.3 seems to return exclusive fullscreen lost even when it was
    // never requested, so long as the caller requested it to be application controlled. Handle
    // this ignoring the lost result and just continuing as normal if we never acquired it.
    res = VK_SUCCESS;
    if (present_fail)
    {
      // We still need to acquire an image.
      res = m_swap_chain->AcquireNextImage();
    }
  }

  if (res != VK_SUCCESS)
  {
    // Execute cmdbuffer before resizing, as the last frame could still be presenting.
    ExecuteCommandBuffer(false, true);

    // Was this a lost exclusive fullscreen?
    if (res == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
    {
      // The present keeps returning exclusive mode lost unless we re-create the swap chain.
      INFO_LOG_FMT(VIDEO, "Lost exclusive fullscreen.");
      m_swap_chain->RecreateSwapChain();
    }
    else if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
    {
      INFO_LOG_FMT(VIDEO, "Resizing swap chain due to suboptimal/out-of-date");
      m_swap_chain->ResizeSwapChain();
    }
    else
    {
      ERROR_LOG_FMT(VIDEO, "Unknown present error {:#010X} {}, please report.",
                    std::to_underlying(res), VkResultToString(res));
      m_swap_chain->RecreateSwapChain();
    }

    res = m_swap_chain->AcquireNextImage();
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
    {
      if (res == VK_ERROR_OUT_OF_DATE_KHR)
      {
        INFO_LOG_FMT(VIDEO, "Swapchain still out of date, will try again next frame...");
      }
      else
      {
        PanicAlertFmt("Failed to grab image from swap chain: {:#010X} {}", std::to_underlying(res),
                      VkResultToString(res));
      }
    }
  }

  if (!m_swap_chain->IsCurrentImageValid())
    return false;

  // Transition from undefined (or present src, but it can be substituted) to
  // color attachment ready for writing. These transitions must occur outside
  // a render pass, unless the render pass declares a self-dependency.
  m_swap_chain->GetCurrentTexture()->OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);
  m_swap_chain->GetCurrentTexture()->TransitionToLayout(
      g_command_buffer_mgr->GetCurrentCommandBuffer(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  SetAndClearFramebuffer(m_swap_chain->GetCurrentFramebuffer(),
                         ClearColor{{0.0f, 0.0f, 0.0f, 1.0f}});
  return true;
}

void VKGfx::PresentBackbuffer()
{
  // End drawing to backbuffer
  StateTracker::GetInstance()->EndRenderPass();

  if (m_swap_chain->IsCurrentImageValid())
  {
    // Transition the backbuffer to PRESENT_SRC to ensure all commands drawing
    // to it have finished before present.
    m_swap_chain->GetCurrentTexture()->TransitionToLayout(
        g_command_buffer_mgr->GetCurrentCommandBuffer(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // Submit the current command buffer, signaling rendering finished semaphore when it's done
    // Because this final command buffer is rendering to the swap chain, we need to wait for
    // the available semaphore to be signaled before executing the buffer. This final submission
    // can happen off-thread in the background while we're preparing the next frame.
    g_command_buffer_mgr->SubmitCommandBuffer(true, false, true, m_swap_chain->GetSwapChain(),
                                              m_swap_chain->GetCurrentImageIndex());
  }
  else
  {
    g_command_buffer_mgr->SubmitCommandBuffer(true, false, true);
  }

  // New cmdbuffer, so invalidate state.
  StateTracker::GetInstance()->InvalidateCachedState();
}

#ifdef __SWITCH2__
bool VKGfx::OpenGuestScreen(void* window)
{
  CloseGuestScreen();
  const VkDevice device = g_vulkan_context->GetDevice();
  const VkPhysicalDevice physical_device = g_vulkan_context->GetPhysicalDevice();
  const WindowSystemInfo wsi{WindowSystemType::Switch, nullptr, window, window};
  m_guest.surface = SwapChain::CreateVulkanSurface(g_vulkan_context->GetVulkanInstance(), wsi);
  if (m_guest.surface == VK_NULL_HANDLE)
    return false;

  VkBool32 present_supported = VK_FALSE;
  VkSurfaceCapabilitiesKHR caps = {};
  u32 format_count = 0;
  if (vkGetPhysicalDeviceSurfaceSupportKHR(physical_device,
                                           g_vulkan_context->GetPresentQueueFamilyIndex(),
                                           m_guest.surface, &present_supported) != VK_SUCCESS ||
      !present_supported ||
      vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, m_guest.surface, &caps) !=
          VK_SUCCESS ||
      !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
      vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, m_guest.surface, &format_count,
                                           nullptr) != VK_SUCCESS ||
      format_count == 0)
  {
    ERROR_LOG_FMT(VIDEO, "Vulkan: the GameShare guest's window cannot be presented to");
    CloseGuestScreen();
    return false;
  }
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, m_guest.surface, &format_count,
                                       formats.data());
  VkSurfaceFormatKHR format = formats[0];
  for (const VkSurfaceFormatKHR& candidate : formats)
  {
    if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM ||
        candidate.format == VK_FORMAT_B8G8R8A8_UNORM)
    {
      format = candidate;
      break;
    }
  }
  m_guest.extent = caps.currentExtent;
  if (m_guest.extent.width == UINT32_MAX || m_guest.extent.width == 0)
    m_guest.extent = {1280, 720};
  u32 image_count = std::max(caps.minImageCount, 3u);
  if (caps.maxImageCount)
    image_count = std::min(image_count, caps.maxImageCount);

  VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = m_guest.surface;
  info.minImageCount = image_count;
  info.imageFormat = format.format;
  info.imageColorSpace = format.colorSpace;
  info.imageExtent = m_guest.extent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  info.clipped = VK_TRUE;
  const VkResult res = vkCreateSwapchainKHR(device, &info, nullptr, &m_guest.swapchain);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateSwapchainKHR (GameShare guest) failed: ");
    m_guest.swapchain = VK_NULL_HANDLE;
    CloseGuestScreen();
    return false;
  }
  u32 count = 0;
  vkGetSwapchainImagesKHR(device, m_guest.swapchain, &count, nullptr);
  m_guest.images.resize(count);
  vkGetSwapchainImagesKHR(device, m_guest.swapchain, &count, m_guest.images.data());
  const VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  m_guest.rendered.assign(count, VK_NULL_HANDLE);
  bool created = true;
  for (VkSemaphore& semaphore : m_guest.rendered)
    created = created && vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore) == VK_SUCCESS;
  for (VkSemaphore& semaphore : m_guest.acquired)
    created = created && vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore) == VK_SUCCESS;
  if (!created)
  {
    CloseGuestScreen();
    return false;
  }
  INFO_LOG_FMT(VIDEO, "Vulkan: GameShare guest screen {}x{}, {} images", m_guest.extent.width,
               m_guest.extent.height, count);
  return true;
}

void VKGfx::CloseGuestScreen()
{
  if (m_guest.surface == VK_NULL_HANDLE)
    return;
  if (g_command_buffer_mgr)
    g_command_buffer_mgr->WaitForWorkerThreadIdle();
  const VkDevice device = g_vulkan_context->GetDevice();
  vkDeviceWaitIdle(device);
  for (VkSemaphore semaphore : m_guest.rendered)
  {
    if (semaphore != VK_NULL_HANDLE)
      vkDestroySemaphore(device, semaphore, nullptr);
  }
  for (VkSemaphore semaphore : m_guest.acquired)
  {
    if (semaphore != VK_NULL_HANDLE)
      vkDestroySemaphore(device, semaphore, nullptr);
  }
  if (m_guest.swapchain != VK_NULL_HANDLE)
    vkDestroySwapchainKHR(device, m_guest.swapchain, nullptr);
  vkDestroySurfaceKHR(g_vulkan_context->GetVulkanInstance(), m_guest.surface, nullptr);
  m_guest = {};
}

void VKGfx::CopyToGuestScreen(const AbstractTexture* source)
{
  if (m_guest.swapchain == VK_NULL_HANDLE)
    return;
  VKTexture* const texture =
      source ? static_cast<VKTexture*>(const_cast<AbstractTexture*>(source)) :
      m_swap_chain && m_swap_chain->IsCurrentImageValid() && m_swap_chain->CanCopyImages() ?
               m_swap_chain->GetCurrentTexture() :
               nullptr;
  if (!texture)
    return;
  StateTracker::GetInstance()->EndRenderPass();
  // No image free (the guest's display shows them slower): this frame is not copied
  const VkSemaphore acquired = m_guest.acquired[m_guest.next_acquired];
  u32 index = 0;
  const VkResult res = vkAcquireNextImageKHR(g_vulkan_context->GetDevice(), m_guest.swapchain, 0,
                                             acquired, VK_NULL_HANDLE, &index);
  if ((res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) || index >= m_guest.images.size())
    return;
  m_guest.next_acquired = (m_guest.next_acquired + 1) % m_guest.acquired.size();

  const VkCommandBuffer command_buffer = g_command_buffer_mgr->GetCurrentCommandBuffer();
  const VkImageLayout source_layout = texture->GetLayout();
  texture->TransitionToLayout(command_buffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  const VkImage guest = m_guest.images[index];
  const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  nullptr,
                                  0,
                                  VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_QUEUE_FAMILY_IGNORED,
                                  VK_QUEUE_FAMILY_IGNORED,
                                  guest,
                                  range};
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

  // Letterboxed to the source's shape
  const s32 source_width = static_cast<s32>(texture->GetWidth());
  const s32 source_height = static_cast<s32>(texture->GetHeight());
  const s32 guest_width = static_cast<s32>(m_guest.extent.width);
  const s32 guest_height = static_cast<s32>(m_guest.extent.height);
  s32 width = guest_width;
  s32 height = static_cast<s32>(static_cast<s64>(guest_width) * source_height / source_width);
  if (height > guest_height)
  {
    height = guest_height;
    width = static_cast<s32>(static_cast<s64>(guest_height) * source_width / source_height);
  }
  const s32 x = (guest_width - width) / 2;
  const s32 y = (guest_height - height) / 2;
  if (width != guest_width || height != guest_height)
  {
    const VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
    vkCmdClearColorImage(command_buffer, guest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                         &range);
  }
  VkImageBlit blit = {};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[1] = {source_width, source_height, 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstOffsets[0] = {x, y, 0};
  blit.dstOffsets[1] = {x + width, y + height, 1};
  vkCmdBlitImage(command_buffer, texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, guest,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                 source ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = 0;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);
  // (the backbuffer goes to PRESENT_SRC in PresentBackbuffer; a texture goes back to sampling)
  if (source)
  {
    texture->TransitionToLayout(command_buffer, source_layout == VK_IMAGE_LAYOUT_UNDEFINED ?
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
                                                    source_layout);
  }
  g_command_buffer_mgr->SetExtraPresent(m_guest.swapchain, index, acquired,
                                        m_guest.rendered[index]);
}
#endif

void VKGfx::SetFullscreen(bool enable_fullscreen)
{
  if (!m_swap_chain->IsFullscreenSupported())
    return;

  m_swap_chain->SetNextFullscreenState(enable_fullscreen);
}

bool VKGfx::IsFullscreen() const
{
  return m_swap_chain && m_swap_chain->GetCurrentFullscreenState();
}

void VKGfx::ExecuteCommandBuffer(bool submit_off_thread, bool wait_for_completion)
{
  StateTracker::GetInstance()->EndRenderPass();

  g_command_buffer_mgr->SubmitCommandBuffer(submit_off_thread, wait_for_completion);

  StateTracker::GetInstance()->InvalidateCachedState();
}

void VKGfx::CheckForSurfaceChange()
{
  if (!g_presenter->SurfaceChangedTestAndClear() || !m_swap_chain)
    return;

  // Submit the current draws up until rendering the XFB.
  ExecuteCommandBuffer(false, true);

  // Clear the present failed flag, since we don't want to resize after recreating.
  g_command_buffer_mgr->CheckLastPresentFail();

  // Recreate the surface. If this fails we're in trouble.
  if (!m_swap_chain->RecreateSurface(g_presenter->GetNewSurfaceHandle()))
    PanicAlertFmt("Failed to recreate Vulkan surface. Cannot continue.");

  // Handle case where the dimensions are now different.
  OnSwapChainResized();
}

void VKGfx::CheckForSurfaceResize()
{
  if (!g_presenter->SurfaceResizedTestAndClear())
    return;

  // If we don't have a surface, how can we resize the swap chain?
  // CheckForSurfaceChange should handle this case.
  if (!m_swap_chain)
  {
    WARN_LOG_FMT(VIDEO, "Surface resize event received without active surface, ignoring");
    return;
  }

  // Wait for the GPU to catch up since we're going to destroy the swap chain.
  ExecuteCommandBuffer(false, true);

  // Clear the present failed flag, since we don't want to resize after recreating.
  g_command_buffer_mgr->CheckLastPresentFail();

  // Resize the swap chain.
  m_swap_chain->RecreateSwapChain();
  OnSwapChainResized();
}

void VKGfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);

  if (bits & CONFIG_CHANGE_BIT_HOST_CONFIG)
    g_object_cache->ReloadPipelineCache();

  // For vsync, we need to change the present mode, which means recreating the swap chain.
  if (m_swap_chain && (bits & CONFIG_CHANGE_BIT_VSYNC))
  {
    ExecuteCommandBuffer(false, true);
    m_swap_chain->SetVSync(g_ActiveConfig.bVSyncActive);
  }

  // For quad-buffered stereo we need to change the layer count, so recreate the swap chain.
  if (m_swap_chain && ((bits & CONFIG_CHANGE_BIT_STEREO_MODE) || (bits & CONFIG_CHANGE_BIT_HDR)))
  {
    ExecuteCommandBuffer(false, true);
    m_swap_chain->RecreateSwapChain();
  }

  // Wipe sampler cache if force texture filtering or anisotropy changes.
  if (bits & (CONFIG_CHANGE_BIT_ANISOTROPY | CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING))
  {
    ExecuteCommandBuffer(false, true);
    ResetSamplerStates();
  }
}

void VKGfx::OnSwapChainResized()
{
  g_presenter->SetBackbuffer(m_swap_chain->GetWidth(), m_swap_chain->GetHeight());
}

void VKGfx::BindFramebuffer(VKFramebuffer* fb)
{
  StateTracker::GetInstance()->EndRenderPass();

  // Shouldn't be bound as a texture.
  fb->Unbind();

  fb->TransitionForRender();
  StateTracker::GetInstance()->SetFramebuffer(fb);
  m_current_framebuffer = fb;
}

void VKGfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    return;

  VKFramebuffer* vkfb = static_cast<VKFramebuffer*>(framebuffer);
  BindFramebuffer(vkfb);
}

void VKGfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    return;

  VKFramebuffer* vkfb = static_cast<VKFramebuffer*>(framebuffer);
  BindFramebuffer(vkfb);

  // If we're discarding, begin the discard pass, then switch to a load pass.
  // This way if the command buffer is flushed, we don't start another discard pass.
  StateTracker::GetInstance()->BeginDiscardRenderPass();
}

void VKGfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value,
                                   float depth_value)
{
  VKFramebuffer* vkfb = static_cast<VKFramebuffer*>(framebuffer);
  BindFramebuffer(vkfb);

  VkClearValue clear_color_value;
  std::memcpy(clear_color_value.color.float32, color_value.data(),
              sizeof(clear_color_value.color.float32));
  VkClearValue clear_depth_value;
  clear_depth_value.depthStencil.depth = depth_value;
  clear_depth_value.depthStencil.stencil = 0;
  vkfb->SetAndClear(vkfb->GetRect(), clear_color_value, clear_depth_value);
}

void VKGfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  // Texture should always be in SHADER_READ_ONLY layout prior to use.
  // This is so we don't need to transition during render passes.
  const VKTexture* tex = static_cast<const VKTexture*>(texture);
  if (tex)
  {
    if (tex->GetLayout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
      if (StateTracker::GetInstance()->InRenderPass())
      {
        WARN_LOG_FMT(VIDEO, "Transitioning image in render pass in VKGfx::SetTexture()");
        StateTracker::GetInstance()->EndRenderPass();
      }

      tex->TransitionToLayout(g_command_buffer_mgr->GetCurrentCommandBuffer(),
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    StateTracker::GetInstance()->SetTexture(index, tex->GetView());
  }
  else
  {
    StateTracker::GetInstance()->SetTexture(0, VK_NULL_HANDLE);
  }
}

void VKGfx::SetSamplerState(u32 index, const SamplerState& state)
{
  // Skip lookup if the state hasn't changed.
  if (m_sampler_states[index] == state)
    return;

  // Look up new state and replace in state tracker.
  VkSampler sampler = g_object_cache->GetSampler(state);
  if (sampler == VK_NULL_HANDLE)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create sampler");
    sampler = g_object_cache->GetPointSampler();
  }

  StateTracker::GetInstance()->SetSampler(index, sampler);
  m_sampler_states[index] = state;
}

void VKGfx::SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write)
{
  VKTexture* vk_texture = static_cast<VKTexture*>(texture);
  if (vk_texture)
  {
    StateTracker::GetInstance()->EndRenderPass();
    StateTracker::GetInstance()->SetImageTexture(index, vk_texture->GetView());
    vk_texture->TransitionToLayout(g_command_buffer_mgr->GetCurrentCommandBuffer(),
                                   read ? (write ? VKTexture::ComputeImageLayout::ReadWrite :
                                                   VKTexture::ComputeImageLayout::ReadOnly) :
                                          VKTexture::ComputeImageLayout::WriteOnly);
  }
  else
  {
    StateTracker::GetInstance()->SetImageTexture(index, VK_NULL_HANDLE);
  }
}

void VKGfx::UnbindTexture(const AbstractTexture* texture)
{
  StateTracker::GetInstance()->UnbindTexture(static_cast<const VKTexture*>(texture)->GetView());
}

void VKGfx::ResetSamplerStates()
{
  // Invalidate all sampler states, next draw will re-initialize them.
  for (u32 i = 0; i < m_sampler_states.size(); i++)
  {
    m_sampler_states[i] = RenderState::GetPointSamplerState();
    StateTracker::GetInstance()->SetSampler(i, g_object_cache->GetPointSampler());
  }

  // Invalidate all sampler objects (some will be unused now).
  g_object_cache->ClearSamplerCache();
}

void VKGfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  VkRect2D scissor = {{rc.left, rc.top},
                      {static_cast<u32>(rc.GetWidth()), static_cast<u32>(rc.GetHeight())}};

  // See Vulkan spec for vkCmdSetScissor:
  // The x and y members of offset must be greater than or equal to 0.
  if (scissor.offset.x < 0)
  {
    scissor.extent.width -= -scissor.offset.x;
    scissor.offset.x = 0;
  }
  if (scissor.offset.y < 0)
  {
    scissor.extent.height -= -scissor.offset.y;
    scissor.offset.y = 0;
  }
  StateTracker::GetInstance()->SetScissor(scissor);
}

void VKGfx::SetViewport(float x, float y, float width, float height, float near_depth,
                        float far_depth)
{
  VkViewport viewport = {x, y, width, height, near_depth, far_depth};
  StateTracker::GetInstance()->SetViewport(viewport);
}

void VKGfx::Draw(u32 base_vertex, u32 num_vertices)
{
  if (!StateTracker::GetInstance()->Bind())
    return;

  vkCmdDraw(g_command_buffer_mgr->GetCurrentCommandBuffer(), num_vertices, 1, base_vertex, 0);
}

void VKGfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  if (!StateTracker::GetInstance()->Bind())
    return;

  vkCmdDrawIndexed(g_command_buffer_mgr->GetCurrentCommandBuffer(), num_indices, 1, base_index,
                   base_vertex, 0);
}

void VKGfx::DispatchComputeShader(const AbstractShader* shader, u32 groupsize_x, u32 groupsize_y,
                                  u32 groupsize_z, u32 groups_x, u32 groups_y, u32 groups_z)
{
  StateTracker::GetInstance()->SetComputeShader(static_cast<const VKShader*>(shader));
  if (StateTracker::GetInstance()->BindCompute())
    vkCmdDispatch(g_command_buffer_mgr->GetCurrentCommandBuffer(), groups_x, groups_y, groups_z);
}

SurfaceInfo VKGfx::GetSurfaceInfo() const
{
  return {m_swap_chain ? m_swap_chain->GetWidth() : 1u,
          m_swap_chain ? m_swap_chain->GetHeight() : 0u, m_backbuffer_scale,
          m_swap_chain ? m_swap_chain->GetTextureFormat() : AbstractTextureFormat::Undefined};
}

}  // namespace Vulkan
