// switch/recompiled_minimal.cpp
// A tiny deterministic output shaped exactly like ps1Recomp-generated code.
// Guest MIPS represented here:
//   addiu $t0,$zero,40
//   addiu $t1,$zero,2
//   addu  $v0,$t0,$t1
//   lui   $t2,0x1234
//   ori   $t2,$t2,0x5678
//   jr    $ra
//   nop
#include <cstdint>
#include <runtime/cpu_context.h>

static void func_80010000(uint8_t* /*rdram*/, recomp_context* ctx) {
    ctx->r8 = (int32_t)((uint32_t)0 + (uint32_t)(int32_t)40);
    ctx->r9 = (int32_t)((uint32_t)0 + (uint32_t)(int32_t)2);
    ctx->r2 = (int32_t)((uint32_t)ctx->r8 + (uint32_t)ctx->r9);
    ctx->r10 = 0x12340000u;
    ctx->r10 = ctx->r10 | 0x5678u;
    ctx->enforceR0();
}

void recomp_init_dispatch_table() {}

void recomp_dispatch(uint8_t* rdram, recomp_context* ctx, uint32_t addr) {
    const uint32_t normalized = (addr & 0x1FFFFFFFu) | 0x80000000u;
    if (normalized == 0x80010000u)
        func_80010000(rdram, ctx);
}
