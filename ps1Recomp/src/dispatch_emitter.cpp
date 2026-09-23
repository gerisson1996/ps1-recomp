#include "ps1recomp/dispatch_emitter.h"

namespace ps1recomp {

#ifndef __SWITCH__
#define PS1_FMT_PRINT(...) fmt::print(__VA_ARGS__)
#else
#define PS1_FMT_PRINT(...) std::printf(__VA_ARGS__)
#endif

std::string emitDispatchBody() {
  return R"CPP(void recomp_dispatch(uint8_t* rdram, recomp_context* ctx, uint32_t addr) {
    // Lazy-init on first call
    if (!recomp_table_ready) recomp_init_dispatch_table();

    // 1. NULL pointer guard
    if (addr == 0) [[unlikely]] {
        static bool nullDispatchWarned = false;
        if (!nullDispatchWarned) {
            nullDispatchWarned = true;
            PS1_FMT_PRINT("[DISPATCH] null addr suppressed (startup transient, RA=0x{:08X})\n", ctx->r[31]);
        }
        return;
    }

    // 2. Direct lookup in dispatch table
    recomp_func_t fn = recomp_lookup(addr);
    if (fn) { fn(rdram, ctx); return; }

    // 3. Address normalization (KSEG mirrors) and retry
    uint32_t phys = addr & 0x1FFFFFFFu;
    uint32_t normalized = phys | 0x80000000u;
    if (normalized != addr) {
        fn = recomp_lookup(normalized);
        if (fn) { fn(rdram, ctx); return; }
    }

    // 4. BIOS entry points (A0, B0, C0 -- any KSEG mirror)
    if (ctx->bios) {
        if (phys == 0xA0) { ctx->bios->executeA0(); return; }
        if (phys == 0xB0) { ctx->bios->executeB0(); return; }
        if (phys == 0xC0) { ctx->bios->executeC0(); return; }
    }

    // 5. BIOS table sentinel dispatch (B0:xx / C0:xx)
    if (ctx->bios) {
        if (phys >= 0xB000 && phys < 0xB100) {
            ctx->r[9] = phys & 0xFF; // set $t1 = function index
            ctx->bios->executeB0();
            return;
        }
        if (phys >= 0xC000 && phys < 0xC100) {
            ctx->r[9] = phys & 0xFF;
            ctx->bios->executeC0();
            return;
        }
        if (phys >= 0xA000 && phys < 0xA100) {
            ctx->r[9] = phys & 0xFF;
            ctx->bios->executeA0();
            return;
        }
    }

    // 6. JR RA trampoline detection in RAM
    if (phys < 0x200000u) { // Within 2MB main RAM
        uint32_t instr = (uint32_t)rdram[phys] | ((uint32_t)rdram[phys+1] << 8) |
                         ((uint32_t)rdram[phys+2] << 16) | ((uint32_t)rdram[phys+3] << 24);
        if (instr == 0x03E00008u) {
            return; // JR RA trampoline -- no-op return
        }
    }

    // 7. Unmapped target -- fatal unless PS1_DISPATCH_PERMISSIVE
    //
    // Anything reaching here is code the recompiler never emitted. Logging and
    // continuing turns the miss into a silent no-op: execution proceeds with
    // corrupted state and the symptom surfaces far from its cause. Aborting
    // makes the failure point at the cause, which is what makes the fix loop
    // converge -- run, read the address and caller, emit it, repeat.
    static const bool s_permissive = []() {
        const char* e = std::getenv("PS1_DISPATCH_PERMISSIVE");
        return e && e[0] != '\0' && std::strcmp(e, "0") != 0;
    }();
    if (!s_permissive) {
        // `RA` names the caller only when the guest reached here through JALR.
        // A direct JAL leaves the previous value in r31, so on those paths it
        // points somewhere unrelated and has sent more than one investigation
        // to the wrong function.  The host stack always names the emitted
        // function that issued the dispatch; resolve it with
        //   addr2line -f -C -e build/ps1Runtime/ps1Runtime <offset>
        PS1_FMT_PRINT(stderr,
                   "[DISPATCH] FATAL: unmapped call to 0x{:08X} (phys=0x{:08X})\n"
                   "           issued from guest site 0x{:08X}; RA=0x{:08X}\n"
                   "           This address was never emitted by the recompiler.\n"
                   "           RA is stale on direct-JAL paths -- trust the site and the stack.\n"
                   "           Set PS1_DISPATCH_PERMISSIVE=1 to log and continue instead.\n",
                   addr, phys, ps1LastIndirectSite(), ctx->r[31]);
        std::fflush(stderr);
#ifndef __SWITCH__
        void* bt[24];
        int frames = backtrace(bt, 24);
        backtrace_symbols_fd(bt, frames, 2);
#endif
        std::abort();
    }
    static std::unordered_map<uint32_t, uint32_t> s_unknownHits;
    auto& hitCount = s_unknownHits[addr];
    if (hitCount < 5) {
        PS1_FMT_PRINT(stderr, "[DISPATCH] Unknown target: 0x{:08X} (RA=0x{:08X}, phys=0x{:08X})\n",
                   addr, ctx->r[31], phys);
    } else if (hitCount == 5) {
        PS1_FMT_PRINT(stderr, "[DISPATCH] Unknown target: 0x{:08X} -- suppressing further logs\n", addr);
    }
    hitCount++;
}
)CPP";
}

} // namespace ps1recomp
