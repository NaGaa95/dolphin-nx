// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

class AbstractTexture;
class GBAHostInterface;

namespace HW::GBA
{
class Core;
}

namespace DolphinSwitch::GBAOverlay
{
std::unique_ptr<GBAHostInterface> CreateHost(std::weak_ptr<HW::GBA::Core> core);

void Draw();

void ReleaseTextures();

// GameShare: the GBA on this port is on the guest's own screen, not drawn over this console's
// picture (-1: none)
void SetGuestPort(int port);
// The port's GBA screen as Draw last uploaded it (null while that GBA has shown nothing)
const AbstractTexture* GetTexture(int port);
}
