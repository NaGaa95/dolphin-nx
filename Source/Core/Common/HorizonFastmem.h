// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef __SWITCH__

#include <cstddef>

namespace Common::HorizonFastmem
{
constexpr std::size_t ARENA_GUARD_SIZE = 0;
constexpr std::size_t ARENA_SIZE = 2 * 0x1'0000'0000ull + 3 * ARENA_GUARD_SIZE;

void ReserveArenaAddressSpace();

bool IsArenaSupported();
bool AreReadOnlyMappingsSupported();
}  // namespace Common::HorizonFastmem

#endif
