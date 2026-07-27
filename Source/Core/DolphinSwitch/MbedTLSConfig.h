// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Networking and timing are supplied by libnx.
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C

// randomGet() supplies platform entropy.
#define MBEDTLS_ENTROPY_HARDWARE_ALT
