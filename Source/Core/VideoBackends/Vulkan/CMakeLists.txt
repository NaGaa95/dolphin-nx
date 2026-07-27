add_library(videovulkan
  CommandBufferManager.cpp
  CommandBufferManager.h
  Constants.h
  ObjectCache.cpp
  ObjectCache.h
  ShaderCompiler.cpp
  ShaderCompiler.h
  StagingBuffer.cpp
  StagingBuffer.h
  StateTracker.cpp
  StateTracker.h
  VKBoundingBox.cpp
  VKBoundingBox.h
  VKMain.cpp
  VKPerfQuery.cpp
  VKPerfQuery.h
  VKPipeline.cpp
  VKPipeline.h
  VKGfx.cpp
  VKGfx.h
  VKShader.cpp
  VKShader.h
  VKStreamBuffer.cpp
  VKStreamBuffer.h
  VKSwapChain.cpp
  VKSwapChain.h
  VKTexture.cpp
  VKTexture.h
  VKVertexFormat.cpp
  VKVertexFormat.h
  VKVertexManager.cpp
  VKVertexManager.h
  VulkanContext.cpp
  VulkanContext.h
  VulkanLoader.cpp
  VulkanLoader.h
)

if(DOLPHIN_SWITCH)
  set(_lsfg_root "${CMAKE_SOURCE_DIR}/Externals/LSFG-VK")
  add_library(dolphin_switch_lsfg STATIC
    "${_lsfg_root}/bridge/lsfg_bridge.cpp"
    "${_lsfg_root}/bridge/lsfg_bridge.h"
    "${_lsfg_root}/lsfg-vk-common/src/helpers/errors.cpp"
    "${_lsfg_root}/lsfg-vk-common/src/vulkan/buffer.cpp"
    "${_lsfg_root}/lsfg-vk-common/src/vulkan/command_buffer.cpp"
    "${_lsfg_root}/lsfg-vk-common/src/vulkan/descriptor_pool.cpp"
    "${_lsfg_root}/lsfg-vk-common/src/vulkan/descriptor_set.cpp"
    "${_lsfg_root}/lsfg-vk-common/src/vulkan/fence.cpp"
    "${_lsfg_root}/lsfg-vk-common/src/vulkan/image.cpp"
    "${_lsfg_root}/lsfg-vk-common/src/vulkan/sampler.cpp"
    "${_lsfg_root}/lsfg-vk-common/src/vulkan/semaphore.cpp"
    "${_lsfg_root}/lsfg-vk-common/src/vulkan/shader.cpp"
    "${_lsfg_root}/lsfg-vk-common/src/vulkan/timeline_semaphore.cpp"
    "${_lsfg_root}/lsfg-vk-common/src/vulkan/vulkan.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/extraction/dll_reader.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/extraction/shader_registry.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/helpers/limits.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/helpers/managed_shader.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/helpers/utils.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/lsfgvk.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/shaderchains/alpha0.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/shaderchains/alpha1.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/shaderchains/beta0.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/shaderchains/beta1.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/shaderchains/delta0.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/shaderchains/delta1.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/shaderchains/gamma0.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/shaderchains/gamma1.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/shaderchains/generate.cpp"
    "${_lsfg_root}/lsfg-vk-backend/src/shaderchains/mipmaps.cpp"
  )
  target_include_directories(dolphin_switch_lsfg PUBLIC
    "${_lsfg_root}/bridge"
    "${_lsfg_root}/lsfg-vk-common/include"
    "${_lsfg_root}/lsfg-vk-backend/include"
    "${_lsfg_root}/lsfg-vk-backend/src"
    "${CMAKE_SOURCE_DIR}/Externals/Vulkan-Headers/include"
  )
  target_include_directories(dolphin_switch_lsfg PRIVATE "${CMAKE_SOURCE_DIR}/Source/Core")
  # Dolphin normally builds without C++ exceptions. LSFG-VK deliberately uses
  # exceptions as the error boundary around shader extraction and Vulkan setup.
  target_compile_options(dolphin_switch_lsfg PRIVATE -fexceptions)
  # Dolphin exposes Vulkan entry points as PFN variables. Keep the vendored
  # backend on the same no-prototypes ABI so LTO does not merge those variables
  # with Vulkan's normal function declarations.
  target_compile_definitions(dolphin_switch_lsfg PRIVATE USE_VULKAN=1 VK_NO_PROTOTYPES=1)
  target_link_libraries(dolphin_switch_lsfg PRIVATE common)

  target_sources(videovulkan PRIVATE
    LSFG.cpp
    LSFG.h
    LSFGControl.h
  )
  target_link_libraries(videovulkan PRIVATE dolphin_switch_lsfg)
endif()

target_link_libraries(videovulkan
PUBLIC
  common
  videocommon

PRIVATE
  xxhash::xxhash
)

if (ANDROID AND _M_ARM_64)
  target_link_libraries(videovulkan
  PRIVATE
      adrenotools
  )
endif()

# Only include the Vulkan headers when building the Vulkan backend
target_include_directories(videovulkan
PRIVATE
  ${CMAKE_SOURCE_DIR}/Externals/Vulkan-Headers/include
  ${CMAKE_SOURCE_DIR}/Externals/VulkanMemoryAllocator/include
  ${CMAKE_SOURCE_DIR}/Externals/libadrenotools/include
)

if(MSVC)
  # Add precompiled header
  target_link_libraries(videovulkan PRIVATE use_pch)
endif()
