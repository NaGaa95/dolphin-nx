// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Select SFML's POSIX network configuration for libnx/newlib.
#ifdef __SWITCH__
#ifndef __unix__
#define __unix__ 1
#define DOLPHIN_SFML_UNDEF_UNIX
#endif
#ifndef __linux__
#define __linux__ 1
#define DOLPHIN_SFML_UNDEF_LINUX
#endif
#include <SFML/Config.hpp>
#ifdef DOLPHIN_SFML_UNDEF_LINUX
#undef DOLPHIN_SFML_UNDEF_LINUX
#undef __linux__
#endif
#ifdef DOLPHIN_SFML_UNDEF_UNIX
#undef DOLPHIN_SFML_UNDEF_UNIX
#undef __unix__
#endif
#endif
