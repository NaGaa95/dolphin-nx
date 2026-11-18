// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/DKFrameGenerationShaders.h"

#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <lsfg/common/cache_format.hpp>
#include <lsfg/common/cache_store.hpp>
#include <lsfg/common/error.hpp>
#include <lsfg/common/pe_resources.hpp>
#include <lsfg/common/prepare.hpp>
#include <lsfg/common/shader_set.hpp>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/HorizonClocks.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

namespace Deko3D::FrameGeneration
{
namespace
{
std::string Describe(lsfg::ErrorCode code)
{
  switch (code)
  {
  case lsfg::ErrorCode::io_error:
    return "The shader cache could not be written to the SD card.";
  case lsfg::ErrorCode::out_of_memory:
    return "There was not enough memory to prepare the shaders.";
  case lsfg::ErrorCode::shader_set_unknown:
  case lsfg::ErrorCode::shader_interface_mismatch:
    return "This Lossless.dll holds a shader set that Dolphin does not know how to read.";
  case lsfg::ErrorCode::shader_compile_failed:
    return "A shader from this Lossless.dll did not compile.";
  default:
    return fmt::format("Preparation failed ({}).", lsfg::error_name(code));
  }
}

std::vector<u8> ReadDll()
{
  File::IOFile file(GetDllPath(), "rb");
  if (!file)
    return {};

  std::vector<u8> bytes(file.GetSize());
  if (bytes.empty() || !file.ReadBytes(bytes.data(), bytes.size()))
    return {};
  return bytes;
}

bool HasUsableManifest(const std::string& directory)
{
  const std::string path = directory + DIR_SEP + std::string(lsfg::cache::manifest_name);
  File::IOFile file(path, "rb");
  lsfg::cache::ManifestHeader header;
  if (!file || !file.ReadBytes(&header, sizeof(header)))
    return false;

  return lsfg::succeeded(lsfg::cache::validate(header)) &&
         header.backend_abi_version == lsfg::cache::backend_abi_version;
}

u32 CountModules(std::span<const u8> image, bool performance)
{
  lsfg::pe::ResourceTable table;
  lsfg::shaders::ShaderSet set;
  std::vector<lsfg::shaders::ModuleRequest> requests;
  if (!lsfg::succeeded(lsfg::pe::enumerate_resources(image, table)) ||
      !lsfg::succeeded(lsfg::shaders::identify(image, table.resources, set)) ||
      !lsfg::succeeded(lsfg::shaders::required_modules(set, lsfg::shaders::Precision::high,
                                                       performance, requests)))
  {
    return 0;
  }
  return static_cast<u32>(requests.size());
}

std::string PrepareSet(std::span<const u8> image, bool performance, u32 compiled_before, u32 total,
                       const PrepareProgress& progress)
{
  const lsfg::prepare::Options options{
      .precision = lsfg::shaders::Precision::high,
      .graph = {.performance = performance},
  };

  u32 compiled = compiled_before;
  lsfg::prepare::Result result;
  const lsfg::ErrorCode prepared =
      lsfg::prepare::run(image, options, result, [&](const lsfg::prepare::ModuleReport& module) {
        DEBUG_LOG_FMT(VIDEO, "Frame generation: compiled {} ({} registers, {} bytes of scratch)",
                      module.name, module.registers, module.scratch_bytes);
        if (progress)
          progress(++compiled, total);
      });
  if (!lsfg::succeeded(prepared))
    return Describe(prepared);

  const std::string directory = GetCacheDirectory(performance);
  if (const lsfg::ErrorCode written = lsfg::cache::write(directory, result.contents);
      !lsfg::succeeded(written))
  {
    return Describe(written);
  }

  lsfg::cache::Loaded read_back;
  if (const lsfg::ErrorCode read = lsfg::cache::read(directory, read_back); !lsfg::succeeded(read))
    return Describe(read);
  if (!lsfg::cache::same_modules(read_back, result.contents))
    return "The shader cache did not read back the way it was written.";

  return {};
}
}

std::string GetDirectory()
{
  return File::GetUserPath(D_USER_IDX) + "lsfg" DIR_SEP;
}

std::string GetDllPath()
{
  return GetDirectory() + "Lossless.dll";
}

std::string GetCacheDirectory(bool performance)
{
#ifdef __SWITCH2__
  // (uam2rt's Ampere code, apart from the Switch's: one SD card can serve both consoles)
  return GetDirectory() +
         (performance ? "Cache-nx2" DIR_SEP "Performance" : "Cache-nx2" DIR_SEP "Quality");
#else
  return GetDirectory() + (performance ? "Cache" DIR_SEP "Performance" : "Cache" DIR_SEP "Quality");
#endif
}

ShaderStatus GetShaderStatus()
{
  if (HasUsableManifest(GetCacheDirectory(false)) && HasUsableManifest(GetCacheDirectory(true)))
    return ShaderStatus::Prepared;
  if (!File::Exists(GetDllPath()))
    return ShaderStatus::MissingDll;
  return ShaderStatus::NotPrepared;
}

std::string PrepareShaders(const PrepareProgress& progress)
{
  const std::vector<u8> image = ReadDll();
  if (image.empty())
    return fmt::format("Lossless.dll could not be read from {}", GetDllPath());

  const u32 quality_modules = CountModules(image, false);
  const u32 performance_modules = CountModules(image, true);
  if (quality_modules == 0 || performance_modules == 0)
    return Describe(lsfg::ErrorCode::shader_set_unknown);

  const Common::HorizonClocks::ScopedCpuBoost boost;

  const u32 total = quality_modules + performance_modules;
  if (progress)
    progress(0, total);

  if (std::string error = PrepareSet(image, false, 0, total, progress); !error.empty())
    return error;
  if (std::string error = PrepareSet(image, true, quality_modules, total, progress); !error.empty())
    return error;

  NOTICE_LOG_FMT(VIDEO, "Frame generation: prepared {} shaders from {}", total, GetDllPath());
  return {};
}
}
