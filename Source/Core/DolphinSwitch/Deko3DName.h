// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The native renderer's name in the user interface: drako3d on the Switch 2. Its config value stays "Deko3D".
#ifdef __SWITCH2__
#define DEKO3D_DISPLAY_NAME "Drako3D"
#define DEKO3D_LAUNCHER_NRO "dolphin-nx2.nro"
#else
#define DEKO3D_DISPLAY_NAME "Deko3D"
#define DEKO3D_LAUNCHER_NRO "dolphin.nro"
#endif
