// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <switch.h>

extern u8 __nx_exception_stack[0x10000];
extern u64 __nx_exception_stack_size;

__attribute__((aligned(16))) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
