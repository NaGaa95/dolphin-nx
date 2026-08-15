// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>

namespace Core
{
class System;
}
struct BootParameters;
struct WindowSystemInfo;

namespace BootManager
{
// Called after title metadata and the global/local game INI layers have been installed, but before
// Dolphin initializes any emulated hardware. Returning false aborts the boot and removes the
// temporary game configuration layers again.
using ConfigReadyCallback = std::function<bool()>;

bool BootCore(Core::System& system, std::unique_ptr<BootParameters> parameters,
              const WindowSystemInfo& wsi, ConfigReadyCallback config_ready_callback = {});

// Synchronise Dolphin's configuration with the SYSCONF (which may have changed during emulation),
// and restore settings that were overridden by per-game INIs or for some other reason.
void RestoreConfig();
}  // namespace BootManager
