#include <switch.h>
#include <cstdio>

int main(int argc, char **argv) {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);

    std::printf("PS1Recomp - Nintendo Switch\n");
    std::printf("ARM64/libnx bootstrap OK\n\n");
    std::printf("This build only validates the Switch host toolchain.\n");
    std::printf("Next: attach the portable PS1 runtime subsystems.\n\n");
    std::printf("Press + to exit.\n");

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus)
            break;
        consoleUpdate(nullptr);
    }

    consoleExit(nullptr);
    return 0;
}
