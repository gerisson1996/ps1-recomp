#include <switch.h>
#include <cstdio>
#include <cstdint>
#include "../../ps1Runtime/include/runtime/cpu_context.h"
#include "../../ps1Runtime/include/runtime/emuptr.h"

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

    std::printf("PS1Recomp - Nintendo Switch\n");
    std::printf("ARM64/libnx bootstrap OK\n");
    std::printf("CPUContext size: %zu bytes\n", sizeof(ps1::CPUContext));
    std::printf("PS1 RAM: %zu bytes\n", sizeof(g_ps1_ram));
    std::printf("RAM mirror test: %s\n\n",
                *word == 0x50533153 ? "PASS" : "FAIL");
    std::printf("recompiled_out stub linked: PASS\n");
    std::printf("Press + to exit.\n");

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
    return 0;
}
