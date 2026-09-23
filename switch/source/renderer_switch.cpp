#include "renderer_switch.h"
#include <algorithm>
#include <cstdint>

namespace ps1::gpu {

bool RendererSwitch::init() {
  if (initialized_)
    return true;
  window_ = nwindowGetDefault();
  if (!window_)
    return false;

  // The libnx console already owns the default window. Detach it before
  // creating our framebuffer layer, otherwise consoleUpdate() can immediately
  // present over the frame we just drew.
  consoleExit(nullptr);

  const Result rc = framebufferCreate(&fb_, window_, 1280, 720,
                                      PIXEL_FORMAT_RGBA_8888, 2);
  if (R_FAILED(rc))
    return false;
  framebufferMakeLinear(&fb_);
  initialized_ = true;
  return true;
}

void RendererSwitch::renderFrame() {
  if (!initialized_)
    return;

  uint32_t stride = 0;
  auto *dst = static_cast<uint32_t *>(framebufferBegin(&fb_, &stride));
  if (!dst)
    return;

  // Match the desktop renderer's safe defaults until GP1(08h) sets a mode.
  uint32_t srcX = 0, srcY = 0;
  gpu_.getDisplayArea(srcX, srcY);
  uint32_t srcW = 320, srcH = 240;
  const uint32_t stat = gpu_.readGPUSTAT();
  if (gpu_.isDisplayModeSet()) {
    srcH = (stat & (1u << 20)) ? 480u : 240u;
    const uint32_t hres = (stat >> 17) & 3u;
    srcW = (stat & (1u << 16)) ? 368u
         : hres == 0 ? 256u : hres == 1 ? 320u : hres == 2 ? 512u : 640u;
  }

  srcX = std::min(srcX, GPU::VRAM_WIDTH - 1);
  srcY = std::min(srcY, GPU::VRAM_HEIGHT - 1);
  srcW = std::min(srcW, GPU::VRAM_WIDTH - srcX);
  srcH = std::min(srcH, GPU::VRAM_HEIGHT - srcY);

  const auto *vram = gpu_.getDisplayVRAM();

  // Integer nearest-neighbour scale, centered, preserving the PS1 aspect.
  const uint32_t scaleX = 1280 / srcW;
  const uint32_t scaleY = 720 / srcH;
  const uint32_t scale = std::max(1u, std::min(scaleX, scaleY));
  const uint32_t outW = srcW * scale;
  const uint32_t outH = srcH * scale;
  const uint32_t offX = (1280 - outW) / 2;
  const uint32_t offY = (720 - outH) / 2;

  // libnx returns stride in BYTES, not pixels.
  const size_t stridePixels = stride / sizeof(uint32_t);
  std::fill(dst, dst + stridePixels * 720, 0xFF000000u);

  for (uint32_t y = 0; y < outH; ++y) {
    const uint32_t sy = srcY + y / scale;
    auto *row = dst + static_cast<size_t>(offY + y) * stridePixels + offX;
    for (uint32_t x = 0; x < outW; ++x) {
      const uint16_t p = vram[sy * GPU::VRAM_WIDTH + srcX + x / scale].raw;
      const uint8_t r = static_cast<uint8_t>((p & 0x1F) * 255 / 31);
      const uint8_t g = static_cast<uint8_t>(((p >> 5) & 0x1F) * 255 / 31);
      const uint8_t b = static_cast<uint8_t>(((p >> 10) & 0x1F) * 255 / 31);
      row[x] = 0xFF000000u | (static_cast<uint32_t>(b) << 16) |
               (static_cast<uint32_t>(g) << 8) | r;
    }
  }

  framebufferEnd(&fb_);
}

void RendererSwitch::destroy() {
  if (!initialized_)
    return;
  framebufferClose(&fb_);
  initialized_ = false;
}

} // namespace ps1::gpu
