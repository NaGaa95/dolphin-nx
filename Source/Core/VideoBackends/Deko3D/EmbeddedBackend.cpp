// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Deko3D/VideoBackend.h"

#include "Common/MsgHandler.h"

namespace Deko3D
{
bool VideoBackend::Initialize(const WindowSystemInfo&)
{
  PanicAlertFmt(DEKO3D_DISPLAY_NAME " must be launched through Dolphin's embedded runtime.");
  return false;
}

void VideoBackend::Shutdown()
{
}
}
