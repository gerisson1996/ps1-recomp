#pragma once
#include "runtime/gpu/gpu.h"
#include <switch.h>

namespace ps1::gpu {

class RendererSwitch {
public:
  explicit RendererSwitch(GPU &gpu) : gpu_(gpu) {}
  bool init();
  void renderFrame();
  void destroy();

private:
  GPU &gpu_;
  Framebuffer fb_{};
  NWindow *window_ = nullptr;
  bool initialized_ = false;
};

} // namespace ps1::gpu
