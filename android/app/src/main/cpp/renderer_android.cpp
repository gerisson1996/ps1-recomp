#include "renderer_android.h"

#include <android/log.h>
#include <algorithm>

namespace {
constexpr const char *kTag = "PS1Recomp";

GLuint compileShader(GLenum type, const char *src) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512]{};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    __android_log_print(ANDROID_LOG_ERROR, kTag, "shader compile: %s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}
} // namespace

bool RendererAndroid::createProgram() {
  static const char *vs =
      "attribute vec2 aPos;"
      "attribute vec2 aUv;"
      "varying vec2 vUv;"
      "void main(){ vUv=aUv; gl_Position=vec4(aPos,0.0,1.0); }";
  static const char *fs =
      "precision mediump float;"
      "varying vec2 vUv;"
      "uniform sampler2D uTex;"
      "void main(){ gl_FragColor=texture2D(uTex,vUv); }";

  GLuint v = compileShader(GL_VERTEX_SHADER, vs);
  GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
  if (!v || !f)
    return false;

  program_ = glCreateProgram();
  glAttachShader(program_, v);
  glAttachShader(program_, f);
  glLinkProgram(program_);
  glDeleteShader(v);
  glDeleteShader(f);

  GLint ok = GL_FALSE;
  glGetProgramiv(program_, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(program_);
    program_ = 0;
    return false;
  }

  posLoc_ = glGetAttribLocation(program_, "aPos");
  uvLoc_ = glGetAttribLocation(program_, "aUv");
  texLoc_ = glGetUniformLocation(program_, "uTex");

  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return true;
}

bool RendererAndroid::init(ANativeWindow *window) {
  destroy();
  if (!window)
    return false;

  window_ = window;
  display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr))
    return false;

  const EGLint attrs[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig cfg{};
  EGLint count = 0;
  if (!eglChooseConfig(display_, attrs, &cfg, 1, &count) || count != 1)
    return false;

  EGLint format = 0;
  eglGetConfigAttrib(display_, cfg, EGL_NATIVE_VISUAL_ID, &format);
  ANativeWindow_setBuffersGeometry(window_, 0, 0, format);

  surface_ = eglCreateWindowSurface(display_, cfg, window_, nullptr);
  const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  context_ = eglCreateContext(display_, cfg, EGL_NO_CONTEXT, ctxAttrs);
  if (surface_ == EGL_NO_SURFACE || context_ == EGL_NO_CONTEXT)
    return false;
  if (!eglMakeCurrent(display_, surface_, surface_, context_))
    return false;

  if (!createProgram())
    return false;

  rgba_.resize(640u * 480u);
  ready_ = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "renderer ready");
  return true;
}

void RendererAndroid::render(ps1::gpu::GPU &gpu) {
  if (!ready_)
    return;

  uint32_t sx = 0, sy = 0;
  gpu.getDisplayArea(sx, sy);
  uint32_t sw = 320, sh = 240;
  if (gpu.isDisplayModeSet()) {
    const uint32_t stat = gpu.readGPUSTAT();
    sh = (stat & (1u << 20)) ? 480u : 240u;
    const uint32_t hres = (stat >> 17) & 3u;
    sw = (stat & (1u << 16)) ? 368u
       : hres == 0 ? 256u : hres == 1 ? 320u : hres == 2 ? 512u : 640u;
  }

  sx = std::min(sx, ps1::gpu::GPU::VRAM_WIDTH - 1);
  sy = std::min(sy, ps1::gpu::GPU::VRAM_HEIGHT - 1);
  sw = std::min(sw, ps1::gpu::GPU::VRAM_WIDTH - sx);
  sh = std::min(sh, ps1::gpu::GPU::VRAM_HEIGHT - sy);
  if (rgba_.size() < static_cast<size_t>(sw) * sh)
    rgba_.resize(static_cast<size_t>(sw) * sh);

  const auto *vram = gpu.getDisplayVRAM();
  for (uint32_t y = 0; y < sh; ++y) {
    for (uint32_t x = 0; x < sw; ++x) {
      const uint16_t p =
          vram[(sy + y) * ps1::gpu::GPU::VRAM_WIDTH + sx + x].raw;
      const uint8_t r = static_cast<uint8_t>((p & 31u) * 255u / 31u);
      const uint8_t g = static_cast<uint8_t>(((p >> 5) & 31u) * 255u / 31u);
      const uint8_t b = static_cast<uint8_t>(((p >> 10) & 31u) * 255u / 31u);
      rgba_[static_cast<size_t>(y) * sw + x] =
          0xFF000000u | (static_cast<uint32_t>(b) << 16) |
          (static_cast<uint32_t>(g) << 8) | r;
    }
  }

  EGLint ww = 0, wh = 0;
  eglQuerySurface(display_, surface_, EGL_WIDTH, &ww);
  eglQuerySurface(display_, surface_, EGL_HEIGHT, &wh);
  glViewport(0, 0, ww, wh);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);

  const float srcAspect = static_cast<float>(sw) / static_cast<float>(sh);
  const float dstAspect = static_cast<float>(ww) / static_cast<float>(wh);
  float xs = 1.f, ys = 1.f;
  if (dstAspect > srcAspect)
    xs = srcAspect / dstAspect;
  else
    ys = dstAspect / srcAspect;

  const GLfloat verts[] = {
      -xs, -ys, 0.f, 1.f,
       xs, -ys, 1.f, 1.f,
      -xs,  ys, 0.f, 0.f,
       xs,  ys, 1.f, 0.f,
  };

  glUseProgram(program_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sw, sh, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, rgba_.data());
  glUniform1i(texLoc_, 0);
  glEnableVertexAttribArray(posLoc_);
  glEnableVertexAttribArray(uvLoc_);
  glVertexAttribPointer(posLoc_, 2, GL_FLOAT, GL_FALSE,
                        4 * sizeof(GLfloat), verts);
  glVertexAttribPointer(uvLoc_, 2, GL_FLOAT, GL_FALSE,
                        4 * sizeof(GLfloat), verts + 2);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  eglSwapBuffers(display_, surface_);
}

void RendererAndroid::destroyGl() {
  if (texture_) {
    glDeleteTextures(1, &texture_);
    texture_ = 0;
  }
  if (program_) {
    glDeleteProgram(program_);
    program_ = 0;
  }
}

void RendererAndroid::destroy() {
  if (display_ != EGL_NO_DISPLAY) {
    if (context_ != EGL_NO_CONTEXT)
      eglMakeCurrent(display_, surface_, surface_, context_);
    destroyGl();
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context_ != EGL_NO_CONTEXT)
      eglDestroyContext(display_, context_);
    if (surface_ != EGL_NO_SURFACE)
      eglDestroySurface(display_, surface_);
    eglTerminate(display_);
  }
  display_ = EGL_NO_DISPLAY;
  surface_ = EGL_NO_SURFACE;
  context_ = EGL_NO_CONTEXT;
  window_ = nullptr;
  ready_ = false;
}
