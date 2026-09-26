#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>
#include <cstdint>
#include <vector>

#include "runtime/gpu/gpu.h"

class RendererAndroid {
public:
  bool init(ANativeWindow *window);
  void render(ps1::gpu::GPU &gpu);
  void destroy();
  bool ready() const { return ready_; }

private:
  bool createProgram();
  void destroyGl();

  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLSurface surface_ = EGL_NO_SURFACE;
  EGLContext context_ = EGL_NO_CONTEXT;
  ANativeWindow *window_ = nullptr;
  GLuint program_ = 0;
  GLuint texture_ = 0;
  GLint posLoc_ = -1;
  GLint uvLoc_ = -1;
  GLint texLoc_ = -1;
  std::vector<uint32_t> rgba_;
  bool ready_ = false;
};
