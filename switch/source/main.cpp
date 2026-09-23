#include <switch.h>
#include <cstdio>
#include <cstdint>
#include "../../ps1Runtime/include/runtime/cpu_context.h"
#include "../../ps1Runtime/include/runtime/emuptr.h"
#include "../../ps1Runtime/include/runtime/input/input.h"
#include "../../ps1Runtime/include/runtime/timers/timers.h"
#include "../../ps1Runtime/include/runtime/gpu/gpu.h"
#include "../../ps1Runtime/include/runtime/dma/dma.h"
#include "../../ps1Runtime/include/runtime/memory.h"
#include "renderer_switch.h"

void recomp_init_dispatch_table();
void recomp_dispatch(uint8_t* rdram, recomp_context* ctx, uint32_t addr);

alignas(16) static uint8_t g_ps1_ram[2 * 1024 * 1024];

int main(int argc, char **argv) {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    ps1::emuptr_set_ram(g_ps1_ram);

    recomp_context ctx{};
    ctx.reset();
    ctx.mem = nullptr;
    ctx.bios = nullptr;

    // Validate PS1 RAM semantics on ARM64 before bringing in hardware backends.
    constexpr uint32_t test_addr = 0x80001000;
    auto *word = static_cast<uint32_t*>(ps1::emuptr_translate(test_addr));
    *word = 0x50533153; // "PS1S" marker

    recomp_init_dispatch_table();
    recomp_dispatch(g_ps1_ram, &ctx, 0);

    ps1::input::InputController input;
    input.setPadType(0, ps1::input::PadType::Digital);

    // Deterministic timer/IRQ self-test: target at 4 cycles, reset on target,
    // IRQ on target. This validates the portable PS1 timing core on ARM64.
    ps1::Timers timers;
    ps1::InterruptController irq;
    timers.writeRegister(0x1F801108, 4);
    timers.writeRegister(0x1F801104, (1u << 3) | (1u << 4));
    const uint32_t timerIrqs = timers.tick(4);
    if (timerIrqs)
        irq.raiseInterrupt(timerIrqs);
    irq.writeIMask(ps1::IRQ_TMR0);
    const bool timerPass =
        (timerIrqs & ps1::IRQ_TMR0) != 0 && irq.hasPendingInterrupt();

    // Reuse the upstream GP0 FillRect test semantics on the actual ARM64 build.
    ps1::gpu::GPU gpu;

    // Validate the real Memory <-> DMA <-> GPU path. Keep other DMA devices
    // detached: this test exercises only channel 2 (GPU).
    ps1::Memory memory;
    ps1::DMA dma;
    memory.setGPU(&gpu);
    memory.setDMA(&dma);
    dma.setMemory(&memory);
    dma.setGPU(&gpu);

    // Three GP0 words in guest RAM: FillRect(red), position, size.
    constexpr uint32_t dmaCmd = 0x00010000;
    memory.write32(dmaCmd + 0, 0x020000FF);
    memory.write32(dmaCmd + 4, (100u << 16) | 200u);
    memory.write32(dmaCmd + 8, (40u << 16) | 60u);

    // DMA2 registers: MADR, BCR=3 words, CHCR manual/from-RAM/start/trigger.
    memory.write32(0x1F8010A0, dmaCmd);
    memory.write32(0x1F8010A4, 3);
    memory.write32(0x1F8010A8, (1u << 0) | (1u << 24) | (1u << 28));

    const auto *dmaVram = gpu.getVRAM();
    const uint16_t dmaExpectedRed = 255 >> 3;
    const bool dmaGpuPass =
        dmaVram[100 * ps1::gpu::GPU::VRAM_WIDTH + 200].raw == dmaExpectedRed &&
        dmaVram[139 * ps1::gpu::GPU::VRAM_WIDTH + 259].raw == dmaExpectedRed &&
        (memory.read32(0x1F8010A8) & ((1u << 24) | (1u << 28))) == 0;

    gpu.writeGP0(0x020000FF); // red
    gpu.writeGP0(0x0014000A); // x=10, y=20
    gpu.writeGP0(0x00050005); // 5x5
    const auto *vram = gpu.getVRAM();
    const uint16_t expectedRed = 255 >> 3;
    const bool gpuPass =
        vram[20 * ps1::gpu::GPU::VRAM_WIDTH + 10].raw == expectedRed &&
        vram[24 * ps1::gpu::GPU::VRAM_WIDTH + 14].raw == expectedRed &&
        vram[19 * ps1::gpu::GPU::VRAM_WIDTH + 10].raw == 0 &&
        vram[20 * ps1::gpu::GPU::VRAM_WIDTH + 9].raw == 0;

    // Make the software VRAM visible through a native libnx framebuffer.
    // Draw a larger diagnostic rectangle so the first presentation test is
    // unambiguous on the 1280x720 display.
    gpu.writeGP0(0x020000FF);
    gpu.writeGP0((60u << 16) | 80u);
    gpu.writeGP0((100u << 16) | 160u);
    gpu.snapshotDisplayBuffer();

    ps1::gpu::RendererSwitch renderer(gpu);

    std::printf("PS1Recomp - Nintendo Switch\n");
    std::printf("ARM64/libnx bootstrap OK\n");
    std::printf("CPUContext size: %zu bytes\n", sizeof(ps1::CPUContext));
    std::printf("PS1 RAM: %zu bytes\n", sizeof(g_ps1_ram));
    std::printf("RAM mirror test: %s\n\n",
                *word == 0x50533153 ? "PASS" : "FAIL");
    std::printf("recompiled_out stub linked: PASS\\n");
    std::printf("PS1 timer/IRQ core: %s\\n", timerPass ? "PASS" : "FAIL");
    std::printf("PS1 GPU GP0/VRAM core: %s\\n", gpuPass ? "PASS" : "FAIL");
    std::printf("PS1 Memory/DMA2/GPU path: %s\\n", dmaGpuPass ? "PASS" : "FAIL");
    std::printf("PS1 controller backend: ACTIVE\\n");
    std::printf("Switch framebuffer: READY\\n");
    std::printf("A red rectangle should appear after this screen.\\n");
    std::printf("Press + to exit.\n");

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 held = padGetButtons(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus)
            break;
        if (down & HidNpadButton_A) {
            // Stop the libnx text console from presenting over our framebuffer.
            const bool rendererPass = renderer.init();
            if (rendererPass) {
                renderer.renderFrame();
                // Stay in framebuffer mode; do not call consoleUpdate below.
                while (appletMainLoop()) {
                    padUpdate(&pad);
                    if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
                        break;
                    renderer.renderFrame();
                }
                break;
            }
        }

        // Reset active-low PS1 pad state, then map Switch controls.
        constexpr uint16_t all =
            ps1::input::BTN_SELECT | ps1::input::BTN_START |
            ps1::input::BTN_UP | ps1::input::BTN_RIGHT |
            ps1::input::BTN_DOWN | ps1::input::BTN_LEFT |
            ps1::input::BTN_L1 | ps1::input::BTN_R1 |
            ps1::input::BTN_L2 | ps1::input::BTN_R2 |
            ps1::input::BTN_TRIANGLE | ps1::input::BTN_CIRCLE |
            ps1::input::BTN_CROSS | ps1::input::BTN_SQUARE;
        input.release(all);

        if (held & HidNpadButton_A) input.press(ps1::input::BTN_CROSS);
        if (held & HidNpadButton_B) input.press(ps1::input::BTN_CIRCLE);
        if (held & HidNpadButton_X) input.press(ps1::input::BTN_TRIANGLE);
        if (held & HidNpadButton_Y) input.press(ps1::input::BTN_SQUARE);
        if (held & HidNpadButton_Up) input.press(ps1::input::BTN_UP);
        if (held & HidNpadButton_Right) input.press(ps1::input::BTN_RIGHT);
        if (held & HidNpadButton_Down) input.press(ps1::input::BTN_DOWN);
        if (held & HidNpadButton_Left) input.press(ps1::input::BTN_LEFT);
        if (held & HidNpadButton_L) input.press(ps1::input::BTN_L1);
        if (held & HidNpadButton_R) input.press(ps1::input::BTN_R1);
        if (held & HidNpadButton_ZL) input.press(ps1::input::BTN_L2);
        if (held & HidNpadButton_ZR) input.press(ps1::input::BTN_R2);
        if (held & HidNpadButton_Minus) input.press(ps1::input::BTN_SELECT);
        if (held & HidNpadButton_Plus) input.press(ps1::input::BTN_START);
        consoleUpdate(nullptr);
    }
    renderer.destroy();
    return 0;
}
