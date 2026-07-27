// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace EMM
{
void InstallExceptionHandler();
void UninstallExceptionHandler();
bool IsExceptionHandlerSupported();

#ifdef __SWITCH__
void SetLazyRegionInfo(uintptr_t base, size_t size);
#endif
}  // namespace EMM
