// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The Switch 2 build's UamBridge (UamBridge.h): GLSL goes through uam2rt, drako3d's shader compiler (UAM2: Mesa's
// GLSL front end and NAK) built into the program, in place of uam's Maxwell code generator. Its DKSH is complete as
// returned. A uniform block binding N is the hardware's constant buffer N + 3: graphics stages have bindings 0-14
// (the backend uses 0-3).

#include "VideoBackends/Deko3D/UamBridge.h"

#include <cstdlib>
#include <cstring>

#include <uam2rt.h>

static_assert(int(UamStage_Vertex) == int(Uam2rtStage_Vertex) && int(UamStage_TessControl) == int(Uam2rtStage_TessCtrl) &&
                  int(UamStage_TessEval) == int(Uam2rtStage_TessEval) && int(UamStage_Geometry) == int(Uam2rtStage_Geometry) &&
                  int(UamStage_Fragment) == int(Uam2rtStage_Fragment) && int(UamStage_Compute) == int(Uam2rtStage_Compute),
              "uam2rt numbers the stages as uam");

void* UamCompileGlsl(const char* glsl, int stage, size_t* out_size, char** out_log)
{
  *out_size = 0;
  *out_log = nullptr;
  size_t size = 0;
  char* log = nullptr;
  void* image = uam2rtCompileGlsl(glsl, stage, &size, &log, nullptr);
  if (log)
  {
    // (the callers free the log and the image with free(), as uam2rt allocates them)
    *out_log = log;
  }
  if (!image)
    return nullptr;
  *out_size = size;
  return image;
}
