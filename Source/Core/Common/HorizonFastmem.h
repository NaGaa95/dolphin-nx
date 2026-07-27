// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef __SWITCH__

namespace Common::HorizonFastmem
{
bool IsArenaSupported();
bool AreReadOnlyMappingsSupported();
}  // namespace Common::HorizonFastmem

#endif
