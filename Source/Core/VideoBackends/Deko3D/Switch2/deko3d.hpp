// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The Switch 2 build's deko3d.hpp (CMake/Deko3D.cmake puts this directory first): drako3d, deko3d's API on the
// Switch 2's Ampere GPU, with the same dk:: names, so the backend builds unchanged against it.
#pragma once

#if !defined(__SWITCH2__)
#error "VideoBackends/Deko3D/Switch2 is only for the Switch 2 build"
#endif

#include <drako3d.hpp>
