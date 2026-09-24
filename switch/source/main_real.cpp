#include <switch.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "renderer_switch.h"
#include "../../ps1Runtime/include/runtime/bios/bios.h"
#include "../../ps1Runtime/include/runtime/cdrom/cdrom_controller.h"
#include "../../ps1Runtime/include/runtime/cdrom/virtual_fs.h"
#include "../../ps1Runtime/include/runtime/cpu_context.h"
#include "../../ps1Runtime/include/runtime/dma/dma.h"
#include "../../ps1Runtime/include/runtime/emuptr.h"
#include "../../ps1Runtime/include/runtime/gpu/gpu.h"
#include "../../ps1Runtime/include/runtime/input/input.h"
#include "../../ps1Runtime/include/runtime/mdec/mdec.h"
#include "../../ps1Runtime/include/runtime/memory.h"
#include "../../ps1Runtime/include/runtime/ps1_runtime_macros.h"
#include "../../ps1Runtime/include/runtime/psyq/psyq_hle.h"
#include "../../ps1Runtime/include/runtime/psyq/psyq_registry.h"
#include "../../ps1Runtime/include/runtime/psyq/psyq_state.h"
#include "../../ps1Runtime/include/runtime/spu/spu.h"
#include "../../ps1Runtime/include/runtime/timers/timers.h"

void recomp_init_dispatch_table();
void recomp_dispatch(uint8_t *rdram, recomp_context *ctx, uint32_t addr);

namespace {

constexpr const char *kKernelPath = "sdmc:/switch/ps1recomp/KERNEL.BIN";
constexpr const char *kDiscCandidates[] = {
    "sdmc:/switch/ps1recomp/DISC.BIN",
    "sdmc:/switch/ps1recomp/GAME.BIN",
    "sdmc:/switch/ps1recomp/game.bin",
};

constexpr uint32_t CPU_CLOCK = 33868800;
constexpr uint32_t CYCLES_PER_FRAME = CPU_CLOCK / 60;
constexpr uint32_t CYCLES_PER_SCANLINE = 3413;
constexpr uint32_t SCANLINES_PER_FRAME = 263;

// This exact KERNEL uses the native PsyQ/libetc VBlank counter at 0x8005B2FC.
// Its VSync(-1) path reads this RAM word directly. The portable HLE counter
// lives in PsyqState instead, so native CD boot code that waits a few VBlanks
// can otherwise sit forever even while the host clock is advancing.
constexpr uint32_t kNativeVsyncCounterAddr = 0x8005B2FCu;

struct PsxExeBootInfo {
  uint32_t pc = 0;
  uint32_t gp = 0;
  uint32_t loadAddr = 0;
  uint32_t payloadSize = 0;
  uint32_t spBase = 0;
  uint32_t spOffset = 0;
};

bool readFile(const char *path, std::vector<uint8_t> &out,
              size_t maxBytes = 3 * 1024 * 1024) {
  FILE *fp = std::fopen(path, "rb");
  if (!fp)
    return false;
  if (std::fseek(fp, 0, SEEK_END) != 0) {
    std::fclose(fp);
    return false;
  }
  const long length = std::ftell(fp);
  if (length <= 0 || static_cast<size_t>(length) > maxBytes) {
    std::fclose(fp);
    return false;
  }
  std::rewind(fp);
  out.resize(static_cast<size_t>(length));
  const size_t got = std::fread(out.data(), 1, out.size(), fp);
  std::fclose(fp);
  return got == out.size();
}

bool parsePsxExe(const std::vector<uint8_t> &image, PsxExeBootInfo &out) {
  if (image.size() < 0x800 || std::memcmp(image.data(), "PS-X EXE", 8) != 0)
    return false;
  auto rd32 = [&](size_t off) -> uint32_t {
    return uint32_t(image[off]) | (uint32_t(image[off + 1]) << 8) |
           (uint32_t(image[off + 2]) << 16) |
           (uint32_t(image[off + 3]) << 24);
  };
  out.pc = rd32(0x10);
  out.gp = rd32(0x14);
  out.loadAddr = rd32(0x18);
  out.payloadSize = rd32(0x1C);
  out.spBase = rd32(0x30);
  out.spOffset = rd32(0x34);
  if (out.payloadSize == 0 || 0x800ull + out.payloadSize > image.size())
    return false;
  const uint32_t phys = out.loadAddr & 0x1FFFFFu;
  return phys + out.payloadSize <= 2u * 1024u * 1024u;
}

void mapPad(ps1::input::InputController &input, u64 held) {
  struct Binding {
    u64 hid;
    uint16_t ps;
  };
  static constexpr Binding bindings[] = {
      {HidNpadButton_Up, ps1::input::BTN_UP},
      {HidNpadButton_Down, ps1::input::BTN_DOWN},
      {HidNpadButton_Left, ps1::input::BTN_LEFT},
      {HidNpadButton_Right, ps1::input::BTN_RIGHT},
      // Physical-position mapping: bottom/right/left/top.
      {HidNpadButton_B, ps1::input::BTN_CROSS},
      {HidNpadButton_A, ps1::input::BTN_CIRCLE},
      {HidNpadButton_Y, ps1::input::BTN_SQUARE},
      {HidNpadButton_X, ps1::input::BTN_TRIANGLE},
      {HidNpadButton_L, ps1::input::BTN_L1},
      {HidNpadButton_R, ps1::input::BTN_R1},
      {HidNpadButton_ZL, ps1::input::BTN_L2},
      {HidNpadButton_ZR, ps1::input::BTN_R2},
      {HidNpadButton_Plus, ps1::input::BTN_START},
      {HidNpadButton_Minus, ps1::input::BTN_SELECT},
      {HidNpadButton_StickL, ps1::input::BTN_L3},
      {HidNpadButton_StickR, ps1::input::BTN_R3},
  };
  for (const auto &b : bindings) {
    if (held & b.hid)
      input.press(b.ps, 0);
    else
      input.release(b.ps, 0);
  }
}

[[noreturn]] void cleanExit(ps1::gpu::RendererSwitch &renderer) {
  renderer.destroy();
  consoleExit(nullptr);
  std::_Exit(0);
}

} // namespace

int main(int, char **) {
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  std::printf("ps1Recomp Switch - REAL GAME bring-up\n");
  std::printf("Kernel: %s\n", kKernelPath);

  std::vector<uint8_t> kernel;
  PsxExeBootInfo boot{};
  if (!readFile(kKernelPath, kernel) || !parsePsxExe(kernel, boot)) {
    std::printf("KERNEL.BIN missing or invalid PS-X EXE.\n");
    std::printf("Expected: sdmc:/switch/ps1recomp/KERNEL.BIN\n");
    std::printf("Press + to exit.\n");
    while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
        break;
      consoleUpdate(nullptr);
      svcSleepThread(16'000'000);
    }
    consoleExit(nullptr);
    return 1;
  }

  std::printf("KERNEL READY\n");
  std::printf("PC=%08X LOAD=%08X SIZE=%u GP=%08X SP=%08X\n",
              boot.pc, boot.loadAddr, boot.payloadSize, boot.gp,
              boot.spBase + boot.spOffset);

  const bool nativeVsyncMirror =
      boot.pc == 0x80010B08u && boot.loadAddr == 0x80010000u &&
      boot.payloadSize == 985088u;
  std::printf("Native VSync RAM mirror: %s\n",
              nativeVsyncMirror ? "ON" : "OFF");

  static ps1::Memory memory;
  static ps1::gpu::GPU gpu;
  static ps1::spu::SPU spu;
  static ps1::DMA dma;
  static ps1::InterruptController irq;
  static ps1::Timers timers;
  static ps1::input::InputController input;
  static ps1::mdec::MDEC mdec;
  static ps1::cdrom::CdromController cdrom;
  static ps1::cdrom::VirtualFs vfs;

  input.setPadType(0, ps1::input::PadType::Digital);
  ps1::emuptr_set_ram(memory.ramPtr());

  memory.setGPU(&gpu);
  memory.setSPU(&spu);
  memory.setDMA(&dma);
  memory.setCDROM(&cdrom);
  memory.setInput(&input);
  memory.setMDEC(&mdec);
  memory.setTimers(&timers);
  memory.setInterruptController(&irq);

  dma.setMemory(&memory);
  dma.setGPU(&gpu);
  dma.setSPU(&spu);
  dma.setCDROM(&cdrom);
  dma.setMDEC(&mdec);

  cdrom.attachVirtualFs(&vfs);
  cdrom.setXaCallback([&](const int16_t *samples, uint32_t count) {
    spu.pushXaSamples(samples, count);
  });

  const char *mountedDisc = nullptr;
  long mountedDiscBytes = 0;
  for (const char *candidate : kDiscCandidates) {
    FILE *f = std::fopen(candidate, "rb");
    if (!f)
      continue;
    if (std::fseek(f, 0, SEEK_END) == 0)
      mountedDiscBytes = std::ftell(f);
    std::fclose(f);
    if (vfs.loadDisc(candidate)) {
      mountedDisc = candidate;
      break;
    }
  }
  if (mountedDisc) {
    std::printf("DISC: mounted %s\n", mountedDisc);
    std::printf("DISC: bytes=%ld raw2352_rem=%ld\n", mountedDiscBytes,
                mountedDiscBytes > 0 ? (mountedDiscBytes % 2352) : -1L);
  } else {
    std::printf("DISC: not found (boot continues; CD reads may stop later)\n");
  }

  recomp_context ctx{};
  ctx.reset();
  ctx.mem = &memory;
  ctx.r[ps1::GP] = boot.gp;
  ctx.r[ps1::SP] = boot.spBase + boot.spOffset;

  ps1::bios::Bios bios(ctx, vfs, memory);
  ctx.bios = &bios;
  bios.setGPU(&gpu);
  bios.setInputController(&input);
  bios.setCdromController(&cdrom);
  bios.setDma(&dma);
  bios.setGameThreadId(std::this_thread::get_id());

  cdrom.setInterruptCallback(
      [&](uint8_t intType) { bios.queueCdromEvent(intType); });

  ps1::psyq::HleConfig hleCfg;
  hleCfg.drainCallbacks = [&]() { bios.drainPendingCallbacks(); };
  hleCfg.writeGP0 = [&](uint32_t word) { gpu.writeGP0(word); };
  hleCfg.writeGP1 = [&](uint32_t word) { gpu.writeGP1(word); };
  hleCfg.deliverVBlankEvent = [&]() { bios.triggerVBlankEvent(); };
  ps1::psyq::configure(hleCfg);
  psyq_registry_init_defaults();
  psyq_register_rayman_boot();

  const uint32_t loadPhys = boot.loadAddr & 0x1FFFFFu;
  std::memcpy(memory.ramPtr() + loadPhys, kernel.data() + 0x800,
              boot.payloadSize);

  // Real BIOS leaves display enabled before handing control to the EXE.
  gpu.writeGP1(0x03000000);

  ps1::gpu::RendererSwitch renderer(gpu);
  bool rendererActive = false;

  auto vblankTick = [&]() {
    using namespace std::chrono;
    static auto next = steady_clock::now();
    const auto now = steady_clock::now();
    if (now < next)
      return;
    next = now + microseconds(16667);

    if (!appletMainLoop())
      cleanExit(renderer);

    padUpdate(&pad);
    const u64 held = padGetButtons(&pad);
    const u64 down = padGetButtonsDown(&pad);
    if ((held & HidNpadButton_Plus) && (held & HidNpadButton_Minus))
      cleanExit(renderer);
    mapPad(input, held);

    cdrom.tick(CYCLES_PER_FRAME);

    uint32_t timerIrqs = 0;
    for (uint32_t line = 0; line < SCANLINES_PER_FRAME; ++line)
      timerIrqs |= timers.tick(CYCLES_PER_SCANLINE, true, false);
    if (timerIrqs & ps1::IRQ_TMR0)
      irq.raiseInterrupt(ps1::IRQ_TMR0);
    if (timerIrqs & ps1::IRQ_TMR1)
      irq.raiseInterrupt(ps1::IRQ_TMR1);
    if (timerIrqs & ps1::IRQ_TMR2)
      irq.raiseInterrupt(ps1::IRQ_TMR2);

    irq.raiseInterrupt(ps1::IRQ_VBLANK);
    if (cdrom.hasInterrupt())
      irq.raiseInterrupt(ps1::IRQ_CDROM);
    if (dma.hasInterrupt())
      irq.raiseInterrupt(ps1::IRQ_DMA);
    if (input.hasInterrupt()) {
      irq.raiseInterrupt(ps1::IRQ_PAD_MC);
      input.clearInterrupt();
    }
    if (spu.hasIrq()) {
      irq.raiseInterrupt(ps1::IRQ_SPU);
      spu.clearIrq();
    }

    auto &psyq = ps1::psyq::psyq_state();
    const uint32_t frame =
        psyq.vsyncCounter.fetch_add(1, std::memory_order_release) + 1;
    psyq.vblankPending.store(true, std::memory_order_release);

    // Bridge native libetc VSync(-1) to the cooperative host VBlank clock.
    // Increment instead of assigning the host frame so a guest-side reset of
    // the counter keeps its original semantics.
    if (nativeVsyncMirror) {
      const uint32_t nativeVb = memory.read32(kNativeVsyncCounterAddr);
      memory.write32(kNativeVsyncCounterAddr, nativeVb + 1);
    }

    bios.updatePadBuffers();
    gpu.snapshotDisplayBuffer();

    // Diagnostic-first bring-up: never continue running the guest after
    // detaching the libnx console. The synthetic framebuffer test is stable,
    // but keeping the real guest executing while the default window changes
    // ownership caused an Atmosphere crash on hardware. R3 therefore shows a
    // frozen snapshot only; + exits the preview cleanly.
    if (!rendererActive && (down & HidNpadButton_StickR)) {
      std::printf("[REAL] R3 -> frozen VRAM preview (+ exits)\n");
      consoleUpdate(nullptr);
      rendererActive = renderer.init();
      if (rendererActive) {
        while (appletMainLoop()) {
          padUpdate(&pad);
          if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
          renderer.renderFrame();
          svcSleepThread(16'000'000);
        }
        renderer.destroy();
        std::_Exit(0);
      } else {
        std::printf("[REAL] framebuffer init failed; console restored\n");
        consoleUpdate(nullptr);
      }
    }

    if ((frame % 60) == 0) {
      uint32_t dx = 0, dy = 0;
      gpu.getDisplayArea(dx, dy);
      const uint32_t cdSmState = memory.read32(0x80059704u);
      const uint8_t cdResp0 = memory.read8(0x800596FCu);
      const uint8_t cdResp1 = memory.read8(0x800596FDu);
      auto &psyqDbg = ps1::psyq::psyq_state();

      // Cheap once-per-second proof that the game is actually drawing or
      // uploading image data. Count non-black words across the full 1 MiB
      // VRAM snapshot and keep a small rolling hash so changes are visible
      // without switching away from the diagnostic console.
      const auto *vram = gpu.getDisplayVRAM();
      uint32_t vramNonZero = 0;
      uint32_t vramHash = 2166136261u;
      for (uint32_t i = 0; i < ps1::gpu::GPU::VRAM_WIDTH *
                                  ps1::gpu::GPU::VRAM_HEIGHT; ++i) {
        const uint16_t px = vram[i].raw;
        if (px != 0)
          ++vramNonZero;
        vramHash ^= px;
        vramHash *= 16777619u;
      }

      std::printf(
          "[REAL] vsync=%u site=%08X RA=%08X SP=%08X GP=%08X\n"
          "       GPUSTAT=%08X DISP=%u,%u mode=%s\n"
          "       CD hw=%u IF=%u sector=%u mode=%02X cmd=%02X disc=%s\n"
          "       CD sm=%u resp=%02X,%02X hleSync=%u hleReady=%u nativeVB=%u\n"
          "       VRAM nz=%u hash=%08X display=%s\n"
          "       MDEC dec=%llu mb=%llu in=%llu out=%llu ready=%u busy=%u\n",
          frame, ps1LastIndirectSite(), ctx.r[ps1::RA], ctx.r[ps1::SP],
          ctx.r[ps1::GP], gpu.readGPUSTAT(), dx, dy,
          gpu.isDisplayModeSet() ? "SET" : "DEFAULT",
          static_cast<unsigned>(cdrom.getState()),
          static_cast<unsigned>(cdrom.interruptFlag()),
          cdrom.hasSectorReady() ? 1u : 0u,
          static_cast<unsigned>(cdrom.getMode()),
          static_cast<unsigned>(cdrom.getLastCommand()),
          mountedDisc ? "YES" : "NO",
          cdSmState, cdResp0, cdResp1,
          static_cast<unsigned>(psyqDbg.cdSyncByte.load(std::memory_order_acquire)),
          static_cast<unsigned>(psyqDbg.cdReadyByte.load(std::memory_order_acquire)),
          nativeVsyncMirror ? memory.read32(kNativeVsyncCounterAddr) : 0u,
          vramNonZero, vramHash, gpu.isDisplayEnabled() ? "ON" : "OFF",
          static_cast<unsigned long long>(mdec.decodeCommandCount()),
          static_cast<unsigned long long>(mdec.macroblockCount()),
          static_cast<unsigned long long>(mdec.dmaInWordCount()),
          static_cast<unsigned long long>(mdec.dmaOutWordCount()),
          mdec.outputWordsReady(), mdec.isBusy() ? 1u : 0u);
      consoleUpdate(nullptr);
    }
  };
  bios.setVBlankPump(vblankTick);

  recomp_init_dispatch_table();
  ctx.pc = boot.pc;

  std::printf("Dispatch table ready. Starting PC=%08X\n", boot.pc);
  std::printf("Diagnostic console stays visible.\n");
  std::printf("R3 = frozen VRAM preview | PLUS+MINUS = exit\n");
  consoleUpdate(nullptr);

  // This call normally never returns: the recompiled game owns the thread.
  recomp_dispatch(memory.ramPtr(), &ctx, boot.pc);

  if (!rendererActive) {
    std::printf("GAME RETURNED from PC=%08X RA=%08X\n", ctx.pc, ctx.r[ps1::RA]);
    std::printf("Press + to exit.\n");
    while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
        break;
      consoleUpdate(nullptr);
      svcSleepThread(16'000'000);
    }
  }
  renderer.destroy();
  consoleExit(nullptr);
  return 0;
}
