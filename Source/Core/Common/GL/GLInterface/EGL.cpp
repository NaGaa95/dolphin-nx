// Copyright 2012 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/GL/GLInterface/EGL.h"

#include <array>
#include <sstream>
#include <vector>

#ifdef __SWITCH__
#include <EGL/eglmesaext.h>
#include <switch.h>
#endif

#include "Common/Logging/Log.h"

#ifndef EGL_KHR_create_context
#define EGL_KHR_create_context 1
#define EGL_CONTEXT_MAJOR_VERSION_KHR 0x3098
#define EGL_CONTEXT_MINOR_VERSION_KHR 0x30FB
#define EGL_CONTEXT_FLAGS_KHR 0x30FC
#define EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR 0x30FD
#define EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_KHR 0x31BD
#define EGL_NO_RESET_NOTIFICATION_KHR 0x31BE
#define EGL_LOSE_CONTEXT_ON_RESET_KHR 0x31BF
#define EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR 0x00000001
#define EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR 0x00000002
#define EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR 0x00000004
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR 0x00000001
#define EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR 0x00000002
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif /* EGL_KHR_create_context */

GLContextEGL::~GLContextEGL()
{
  DestroyWindowSurface();
  DestroyContext();
}

bool GLContextEGL::IsHeadless() const
{
  return m_wsi.type == WindowSystemType::Headless;
}

void GLContextEGL::Swap()
{
  if (m_egl_surface != EGL_NO_SURFACE)
    eglSwapBuffers(m_egl_display, m_egl_surface);
}
void GLContextEGL::SwapInterval(int interval)
{
  eglSwapInterval(m_egl_display, interval);
}

void* GLContextEGL::GetFuncAddress(const std::string& name)
{
  return (void*)eglGetProcAddress(name.c_str());
}

void GLContextEGL::DetectMode()
{
  EGLint num_configs;
  bool supportsGL = false, supportsGLES3 = false;
  std::array<int, 2> renderable_types{{EGL_OPENGL_BIT, EGL_OPENGL_ES3_BIT_KHR}};

  for (auto renderable_type : renderable_types)
  {
    // attributes for a visual in RGBA format with at least
    // 8 bits per color
    int attribs[] = {EGL_RED_SIZE,
                     8,
                     EGL_GREEN_SIZE,
                     8,
                     EGL_BLUE_SIZE,
                     8,
                     EGL_RENDERABLE_TYPE,
                     renderable_type,
                     EGL_SURFACE_TYPE,
                     IsHeadless() ? 0 : EGL_WINDOW_BIT,
                     EGL_NONE};

    // Get how many configs there are
    if (!eglChooseConfig(m_egl_display, attribs, nullptr, 0, &num_configs))
    {
      INFO_LOG_FMT(VIDEO, "Error: couldn't get an EGL visual config");
      continue;
    }

    EGLConfig* config = new EGLConfig[num_configs];

    // Get all the configurations
    if (!eglChooseConfig(m_egl_display, attribs, config, num_configs, &num_configs))
    {
      INFO_LOG_FMT(VIDEO, "Error: couldn't get an EGL visual config");
      delete[] config;
      continue;
    }

    for (int i = 0; i < num_configs; ++i)
    {
      EGLint attribVal;
      bool ret;
      ret = eglGetConfigAttrib(m_egl_display, config[i], EGL_RENDERABLE_TYPE, &attribVal);
      if (ret)
      {
        if (attribVal & EGL_OPENGL_BIT)
          supportsGL = true;
        if (attribVal & EGL_OPENGL_ES3_BIT_KHR)
          supportsGLES3 = true;
      }
    }
    delete[] config;
  }

  if (supportsGL)
  {
    INFO_LOG_FMT(VIDEO, "Using OpenGL");
    m_opengl_mode = Mode::OpenGL;
  }
  else if (supportsGLES3)
  {
    INFO_LOG_FMT(VIDEO, "Using OpenGL|ES");
    m_opengl_mode = Mode::OpenGLES;
  }
  else
  {
    // Errored before we found a mode
    ERROR_LOG_FMT(VIDEO, "Error: Failed to detect OpenGL flavour, falling back to OpenGL");
    // This will fail to create a context, as it'll try to use the same attribs we just failed to
    // find a matching config with
    m_opengl_mode = Mode::OpenGL;
  }
}

EGLDisplay GLContextEGL::OpenEGLDisplay()
{
#ifdef __SWITCH__
  return eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
  return eglGetDisplay(static_cast<EGLNativeDisplayType>(m_wsi.render_surface));
#endif
}

EGLNativeWindowType GLContextEGL::GetEGLNativeWindow(EGLConfig config)
{
#ifdef __SWITCH__
  void* const window = m_wsi.render_surface ? m_wsi.render_surface : m_wsi.render_window;
  return reinterpret_cast<EGLNativeWindowType>(window);
#else
  return reinterpret_cast<EGLNativeWindowType>(m_wsi.display_connection);
#endif
}

// Create rendering window.
// Call browser: Core.cpp:EmuThread() > main.cpp:Video_Initialize()
bool GLContextEGL::Initialize(const WindowSystemInfo& wsi, bool stereo, bool core)
{
  EGLint egl_major, egl_minor;
  bool supports_core_profile = false;

  m_wsi = wsi;
  m_egl_display = OpenEGLDisplay();
  if (!m_egl_display)
  {
    INFO_LOG_FMT(VIDEO, "Error: eglGetDisplay() failed");
    return false;
  }

  if (!eglInitialize(m_egl_display, &egl_major, &egl_minor))
  {
    INFO_LOG_FMT(VIDEO, "Error: eglInitialize() failed");
    return false;
  }

  /* Detection code */
  EGLint num_configs = 0;

  if (m_opengl_mode == Mode::Detect)
    DetectMode();

  // attributes for a visual in RGBA format with at least
  // 8 bits per color
  int attribs[] = {EGL_RENDERABLE_TYPE,
                   0,
                   EGL_RED_SIZE,
                   8,
                   EGL_GREEN_SIZE,
                   8,
                   EGL_BLUE_SIZE,
                   8,
                   EGL_SURFACE_TYPE,
                   IsHeadless() ? 0 : EGL_WINDOW_BIT,
                   EGL_NONE};

  std::vector<EGLint> ctx_attribs;
  switch (m_opengl_mode)
  {
  case Mode::OpenGL:
    attribs[1] = EGL_OPENGL_BIT;
    ctx_attribs = {EGL_NONE};
    break;
  case Mode::OpenGLES:
    attribs[1] = EGL_OPENGL_ES3_BIT_KHR;
    ctx_attribs = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    break;
  default:
    ERROR_LOG_FMT(VIDEO, "Unknown opengl mode set");
    return false;
  }

  if (!eglChooseConfig(m_egl_display, attribs, &m_config, 1, &num_configs) || num_configs < 1)
  {
    INFO_LOG_FMT(VIDEO, "Error: couldn't get an EGL visual config");
    return false;
  }

  const EGLenum api = m_opengl_mode == Mode::OpenGL ? EGL_OPENGL_API : EGL_OPENGL_ES_API;
  if (!eglBindAPI(api))
  {
    return false;
  }

  std::string tmp;
  const char* const extensions = eglQueryString(m_egl_display, EGL_EXTENSIONS);
  std::istringstream buffer(extensions ? extensions : "");
  while (buffer >> tmp)
  {
    if (tmp == "EGL_KHR_surfaceless_context")
      m_supports_surfaceless = true;
    else if (tmp == "EGL_KHR_create_context")
      supports_core_profile = true;
  }

  if (supports_core_profile && core && m_opengl_mode == Mode::OpenGL)
  {
    for (const auto& version : s_desktop_opengl_versions)
    {
      std::vector<EGLint> core_attribs = {EGL_CONTEXT_MAJOR_VERSION_KHR,
                                          version.first,
                                          EGL_CONTEXT_MINOR_VERSION_KHR,
                                          version.second,
                                          EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                                          EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                                          EGL_CONTEXT_FLAGS_KHR,
                                          EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR,
                                          EGL_NONE};

      m_egl_context = eglCreateContext(m_egl_display, m_config, EGL_NO_CONTEXT, &core_attribs[0]);
      if (m_egl_context)
      {
        m_attribs = std::move(core_attribs);
        break;
      }
    }
  }

  if (!m_egl_context)
  {
    m_egl_context = eglCreateContext(m_egl_display, m_config, EGL_NO_CONTEXT, &ctx_attribs[0]);
    m_attribs = std::move(ctx_attribs);
  }

  if (!m_egl_context)
  {
    INFO_LOG_FMT(VIDEO, "Error: eglCreateContext failed");
    return false;
  }

  if (!CreateWindowSurface())
  {
    const EGLint error = eglGetError();
    ERROR_LOG_FMT(VIDEO, "Error: CreateWindowSurface failed {:#06x}", error);
    return false;
  }

  const bool current = MakeCurrent();
  return current;
}

std::unique_ptr<GLContext> GLContextEGL::CreateSharedContext()
{
  eglBindAPI(m_opengl_mode == Mode::OpenGL ? EGL_OPENGL_API : EGL_OPENGL_ES_API);
  EGLContext new_egl_context =
      eglCreateContext(m_egl_display, m_config, m_egl_context, m_attribs.data());
  if (!new_egl_context)
  {
    INFO_LOG_FMT(VIDEO, "Error: eglCreateContext failed {:#06x}", eglGetError());
    return nullptr;
  }

  std::unique_ptr<GLContextEGL> new_context = std::make_unique<GLContextEGL>();
  new_context->m_opengl_mode = m_opengl_mode;
  new_context->m_egl_context = new_egl_context;
#ifdef __SWITCH__
  // A second EGL window surface cannot own the same NWindow buffer set. Shared
  // shader-compilation contexts are surfaceless (or pbuffer-backed) instead.
  new_context->m_wsi = m_wsi;
  new_context->m_wsi.type = WindowSystemType::Headless;
  new_context->m_wsi.render_window = nullptr;
  new_context->m_wsi.render_surface = nullptr;
#else
  new_context->m_wsi.display_connection = m_wsi.display_connection;
#endif
  new_context->m_egl_display = m_egl_display;
  new_context->m_config = m_config;
  new_context->m_attribs = m_attribs;
  new_context->m_supports_surfaceless = m_supports_surfaceless;
  new_context->m_is_shared = true;
  if (!new_context->CreateWindowSurface())
  {
    ERROR_LOG_FMT(VIDEO, "Error: CreateWindowSurface failed {:#06x}", eglGetError());
    return nullptr;
  }

  return new_context;
}

bool GLContextEGL::CreateWindowSurface()
{
  if (!IsHeadless())
  {
    EGLNativeWindowType native_window = GetEGLNativeWindow(m_config);
    m_egl_surface = eglCreateWindowSurface(m_egl_display, m_config, native_window, nullptr);
    if (!m_egl_surface)
    {
      INFO_LOG_FMT(VIDEO, "Error: eglCreateWindowSurface failed");
      return false;
    }

    // Get dimensions from the surface.
    EGLint surface_width = 1, surface_height = 1;
    if (!eglQuerySurface(m_egl_display, m_egl_surface, EGL_WIDTH, &surface_width) ||
        !eglQuerySurface(m_egl_display, m_egl_surface, EGL_HEIGHT, &surface_height))
    {
      WARN_LOG_FMT(VIDEO,
                   "Failed to get surface dimensions via eglQuerySurface. Size may be incorrect.");
    }
    m_backbuffer_width = static_cast<int>(surface_width);
    m_backbuffer_height = static_cast<int>(surface_height);
  }
  else if (!m_supports_surfaceless)
  {
    EGLint attrib_list[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    m_egl_surface = eglCreatePbufferSurface(m_egl_display, m_config, attrib_list);
    if (!m_egl_surface)
    {
      INFO_LOG_FMT(VIDEO, "Error: eglCreatePbufferSurface failed");
      return false;
    }
  }
  else
  {
    m_egl_surface = EGL_NO_SURFACE;
  }
  return true;
}

void GLContextEGL::DestroyWindowSurface()
{
  if (m_egl_surface == EGL_NO_SURFACE)
    return;

  if (eglGetCurrentSurface(EGL_DRAW) == m_egl_surface)
    eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (!eglDestroySurface(m_egl_display, m_egl_surface))
    NOTICE_LOG_FMT(VIDEO, "Could not destroy window surface.");
  m_egl_surface = EGL_NO_SURFACE;
}

bool GLContextEGL::MakeCurrent()
{
  return eglMakeCurrent(m_egl_display, m_egl_surface, m_egl_surface, m_egl_context);
}

void GLContextEGL::Update()
{
  if (m_egl_surface == EGL_NO_SURFACE)
    return;

#ifdef __SWITCH__
  const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
  const EGLint target_width = docked ? 1920 : 1280;
  const EGLint target_height = docked ? 1080 : 720;
  EGLint current_width = 0;
  EGLint current_height = 0;
  if (eglQuerySurface(m_egl_display, m_egl_surface, EGL_WIDTH, &current_width) &&
      eglQuerySurface(m_egl_display, m_egl_surface, EGL_HEIGHT, &current_height) &&
      (current_width != target_width || current_height != target_height))
  {
    const auto resize_surface =
        reinterpret_cast<PFNEGLRESIZESURFACEMESAPROC>(eglGetProcAddress("eglResizeSurfaceMESA"));
    if (!resize_surface ||
        resize_surface(m_egl_display, m_egl_surface, target_width, target_height) != EGL_TRUE)
    {
      ERROR_LOG_FMT(VIDEO, "Failed to resize the Horizon EGL surface to {}x{}: {:#06x}",
                    target_width, target_height, eglGetError());
    }
  }
#endif

  EGLint surface_width = 0;
  EGLint surface_height = 0;
  if (eglQuerySurface(m_egl_display, m_egl_surface, EGL_WIDTH, &surface_width) &&
      eglQuerySurface(m_egl_display, m_egl_surface, EGL_HEIGHT, &surface_height))
  {
    m_backbuffer_width = static_cast<u32>(surface_width);
    m_backbuffer_height = static_cast<u32>(surface_height);
  }
}

void GLContextEGL::UpdateSurface(void* window_handle)
{
  m_wsi.render_surface = window_handle;
  ClearCurrent();
  DestroyWindowSurface();
  CreateWindowSurface();
  MakeCurrent();
}

bool GLContextEGL::ClearCurrent()
{
  return eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

// Close backend
void GLContextEGL::DestroyContext()
{
  if (!m_egl_context)
    return;

  if (eglGetCurrentContext() == m_egl_context)
    eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (!eglDestroyContext(m_egl_display, m_egl_context))
    NOTICE_LOG_FMT(VIDEO, "Could not destroy drawing context.");
  if (!m_is_shared && !eglTerminate(m_egl_display))
    NOTICE_LOG_FMT(VIDEO, "Could not destroy display connection.");
  m_egl_context = EGL_NO_CONTEXT;
  m_egl_display = EGL_NO_DISPLAY;
}
