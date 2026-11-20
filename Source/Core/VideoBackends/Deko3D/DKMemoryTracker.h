// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

namespace Deko3D::MemoryTracker
{
void Register(u64 addr, u64 size, std::string label);
void Unregister(u64 addr);

void RegisterMemBlock(DkMemBlock block, std::string label);
void UnregisterMemBlock(DkMemBlock block);

void ReportAddress(u64 addr);

void ReportFaultMessage(std::string_view message);
}
