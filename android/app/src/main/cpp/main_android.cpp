#include <android/log.h>
#include <android_native_app_glue.h>
#include <chrono>
#include <memory>
#include <thread>

#include "renderer_android.h"
#include "runtime/gpu/gpu.h"
#include "runtime/memory.h"

namespace {
constexpr const char *kTag = "PS1Recomp";

struct AppState {
  RendererAndroid renderer;
  ps1::gpu::GPU gpu;
  ps1::Memory memory;
  bool hasWindow = false;
  uint32_t frame = 0;
};

void drawSmoke(AppState &s) {
  const uint32_t phase = (s.frame / 60u) % 3u;
  const uint32_t color = phase == 0 ? 0x000000FFu
                       : phase == 1 ? 0x0000FF00u
                                    : 0x00FF0000u;
  s.gpu.writeGP0(0x02000000u | color);
  s.gpu.writeGP0(0x00000000u);
  s.gpu.writeGP0((240u << 16) | 320u);
  s.gpu.snapshotDisplayBuffer();
}

void onCommand(android_app *app, int32_t cmd) {
  auto *s = static_cast<AppState *>(app->userData);
  switch (cmd) {
  case APP_CMD_INIT_WINDOW:
    if (app->window) {
      s->hasWindow = s->renderer.init(app->window);
      __android_log_print(ANDROID_LOG_INFO, kTag, "INIT_WINDOW ready=%d",
                          s->hasWindow ? 1 : 0);
    }
    break;
  case APP_CMD_TERM_WINDOW:
    s->renderer.destroy();
    s->hasWindow = false;
    break;
  default:
    break;
  }
}
} // namespace

void android_main(android_app *app) {
  // PS1 RAM and BIOS exceed the native thread stack budget.
  // Keep the state on the heap for the lifetime of android_main.
  auto stateOwner = std::make_unique<AppState>();
  AppState &state = *stateOwner;
  state.memory.setGPU(&state.gpu);
  app->userData = &state;
  app->onAppCmd = onCommand;

  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "PS1Recomp Android bootstrap started");

  while (!app->destroyRequested) {
    int events = 0;
    android_poll_source *source = nullptr;
    while (ALooper_pollOnce(0, nullptr, &events,
                           reinterpret_cast<void **>(&source)) >= 0) {
      if (source)
        source->process(app, source);
      if (app->destroyRequested)
        break;
    }

    if (state.hasWindow && state.renderer.ready()) {
      drawSmoke(state);
      state.renderer.render(state.gpu);
      ++state.frame;
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }

  state.renderer.destroy();
  app->onAppCmd = nullptr;
  app->userData = nullptr;
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "PS1Recomp Android bootstrap stopped");
}
