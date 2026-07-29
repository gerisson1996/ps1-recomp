// Tests for the Group 1.A libgpu/libetc/libgs HLEs:
//   SetDispMask, LoadImage, StoreImage, MoveImage, ClearImage,
//   DrawSyncCallback, VSyncCallback, SetVideoMode, GetVideoMode,
//   GsInitGraph, GsDefDispBuff, GsSetWorkBase, GsSortClear.
//
// The libgpu transfer/clear ops translate PsyQ args into GP0/GP1 commands.
// The fixture installs a recording HleConfig so each test can assert the exact
// command sequence written by the HLE.

#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_libgpu.h"
#include "runtime/psyq/psyq_registry.h"
#include "runtime/psyq/psyq_state.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

using namespace ps1;
using namespace ps1::psyq;

namespace {

class PsyqGpuTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  std::vector<uint32_t> gp0;
  std::vector<uint32_t> gp1;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    gp0.clear();
    gp1.clear();
    HleConfig cfg;
    cfg.writeGP0 = [this](uint32_t w) { gp0.push_back(w); };
    cfg.writeGP1 = [this](uint32_t w) { gp1.push_back(w); };
    configure(cfg);
    psyq_state().reset();
  }

  void TearDown() override { psyq_state().reset(); }

  // Write a 4-int16 PsyqRect at `p`.
  void writeRect(uint32_t p, int16_t x, int16_t y, int16_t w, int16_t h) {
    mem.write16(p + 0, static_cast<uint16_t>(x));
    mem.write16(p + 2, static_cast<uint16_t>(y));
    mem.write16(p + 4, static_cast<uint16_t>(w));
    mem.write16(p + 6, static_cast<uint16_t>(h));
  }
};

} // namespace

// SetDispMask

TEST_F(PsyqGpuTest, SetDispMaskEnableSendsGP1_03_With0) {
  ctx.r[A0] = 1; // enable
  hle_libgpu_SetDispMask(&ctx);
  ASSERT_EQ(gp1.size(), 1u);
  EXPECT_EQ(gp1[0], 0x03000000u); // bit 0 = 0 -> display ON
}

TEST_F(PsyqGpuTest, SetDispMaskDisableSendsGP1_03_With1) {
  ctx.r[A0] = 0;
  hle_libgpu_SetDispMask(&ctx);
  ASSERT_EQ(gp1.size(), 1u);
  EXPECT_EQ(gp1[0], 0x03000001u);
}

// LoadImage

TEST_F(PsyqGpuTest, LoadImageEmitsCpuToVramHeaderAndData) {
  // 4x2 pixels = 8 px = 4 words of pixel data.
  uint32_t rectP = 0x80100000u;
  uint32_t srcP  = 0x80100100u;
  writeRect(rectP, 320, 0, 4, 2);
  uint32_t pixels[4] = {0x12345678u, 0x9ABCDEF0u, 0x11112222u, 0x33334444u};
  for (int i = 0; i < 4; ++i) mem.write32(srcP + i * 4, pixels[i]);

  ctx.r[A0] = rectP;
  ctx.r[A1] = srcP;
  hle_libgpu_LoadImage(&ctx);

  ASSERT_EQ(gp0.size(), 3u + 4u);
  EXPECT_EQ(gp0[0], 0xA0000000u);              // CPU->VRAM cmd
  EXPECT_EQ(gp0[1], (0u << 16) | 320u);        // Y|X
  EXPECT_EQ(gp0[2], (2u << 16) | 4u);          // H|W
  for (int i = 0; i < 4; ++i)
    EXPECT_EQ(gp0[3 + i], pixels[i]);
}

TEST_F(PsyqGpuTest, LoadImageOddPixelCountRoundsUpDataWords) {
  // 3x1 pixels = 3 px -> ceil(3/2)=2 words.
  uint32_t rectP = 0x80100000u;
  uint32_t srcP  = 0x80100100u;
  writeRect(rectP, 0, 0, 3, 1);
  mem.write32(srcP + 0, 0xCAFEBABEu);
  mem.write32(srcP + 4, 0xDEADBEEFu);

  ctx.r[A0] = rectP;
  ctx.r[A1] = srcP;
  hle_libgpu_LoadImage(&ctx);
  ASSERT_EQ(gp0.size(), 5u); // 3 hdr + 2 data
  EXPECT_EQ(gp0[3], 0xCAFEBABEu);
  EXPECT_EQ(gp0[4], 0xDEADBEEFu);
}

TEST_F(PsyqGpuTest, LoadImageZeroSizeIsNoop) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 0, 0, 0, 0);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 0x80100100u;
  hle_libgpu_LoadImage(&ctx);
  EXPECT_TRUE(gp0.empty());
}

// StoreImage

TEST_F(PsyqGpuTest, StoreImageEmitsVramToCpuHeader) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 64, 240, 16, 8);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 0x80100200u;
  hle_libgpu_StoreImage(&ctx);
  ASSERT_EQ(gp0.size(), 3u);
  EXPECT_EQ(gp0[0], 0xC0000000u);
  EXPECT_EQ(gp0[1], (240u << 16) | 64u);
  EXPECT_EQ(gp0[2], (8u   << 16) | 16u);
}

// MoveImage

TEST_F(PsyqGpuTest, MoveImageEmitsVramToVramSequence) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 0, 0, 32, 32);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 320; // dx
  ctx.r[A2] = 256; // dy
  hle_libgpu_MoveImage(&ctx);
  ASSERT_EQ(gp0.size(), 4u);
  EXPECT_EQ(gp0[0], 0x80000000u);
  EXPECT_EQ(gp0[1], (0u   << 16) | 0u);
  EXPECT_EQ(gp0[2], (256u << 16) | 320u);
  EXPECT_EQ(gp0[3], (32u  << 16) | 32u);
}

// ClearImage

TEST_F(PsyqGpuTest, ClearImageEmitsFillRect) {
  uint32_t rectP = 0x80100000u;
  writeRect(rectP, 0, 0, 320, 240);
  ctx.r[A0] = rectP;
  ctx.r[A1] = 0x12; // r
  ctx.r[A2] = 0x34; // g
  ctx.r[A3] = 0x56; // b
  hle_libgpu_ClearImage(&ctx);
  ASSERT_EQ(gp0.size(), 3u);
  EXPECT_EQ(gp0[0], 0x02000000u | (0x56u << 16) | (0x34u << 8) | 0x12u);
  EXPECT_EQ(gp0[1], (0u   << 16) | 0u);
  EXPECT_EQ(gp0[2], (240u << 16) | 320u);
}

// Sync callbacks

TEST_F(PsyqGpuTest, DrawSyncCallbackStoresAndReturnsPrev) {
  ctx.r[A0] = 0x80012340u;
  ctx.r[V0] = 0xDEADu;
  hle_libgpu_DrawSyncCallback(&ctx);
  // First call: previous should be 0 (registry just-installed).
  EXPECT_EQ(ctx.r[V0], 0u);

  ctx.r[A0] = 0xBEEFCAFEu;
  hle_libgpu_DrawSyncCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0x80012340u);
}

TEST_F(PsyqGpuTest, VSyncCallbackStoresAndReturnsPrev) {
  ctx.r[A0] = 0x80044000u;
  hle_libgpu_VSyncCallback(&ctx);
  // First install returns previous (0 -- singleton was reset in SetUp).
  EXPECT_EQ(ctx.r[V0], 0u);
  // The HLE writes through to psyq_state().gpuSwapCb so bios.cpp's VBlank
  // dispatch can find the swap callback without per-game BSS configuration.
  EXPECT_EQ(psyq_state().gpuSwapCb, 0x80044000u);

  // Subsequent install returns whatever we just wrote.
  ctx.r[A0] = 0;
  hle_libgpu_VSyncCallback(&ctx);
  EXPECT_EQ(ctx.r[V0], 0x80044000u);
  EXPECT_EQ(psyq_state().gpuSwapCb, 0u);
}

// Video mode

TEST_F(PsyqGpuTest, SetGetVideoModeRoundTrip) {
  ctx.r[A0] = 0; // NTSC
  hle_libgpu_SetVideoMode(&ctx);
  hle_libgpu_GetVideoMode(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);

  ctx.r[A0] = 1; // PAL
  hle_libgpu_SetVideoMode(&ctx);
  // Prev should have been NTSC=0
  EXPECT_EQ(ctx.r[V0], 0u);
  hle_libgpu_GetVideoMode(&ctx);
  EXPECT_EQ(ctx.r[V0], 1u);

  // Restore to NTSC so other tests start clean.
  ctx.r[A0] = 0;
  hle_libgpu_SetVideoMode(&ctx);
}

// libgs stubs

TEST_F(PsyqGpuTest, LibgsStubsAreNoopAndDoNotCrash) {
  ctx.r[A0] = 0; ctx.r[A1] = 0; ctx.r[A2] = 0; ctx.r[A3] = 0;
  EXPECT_NO_FATAL_FAILURE(hle_libgs_GsInitGraph(&ctx));
  EXPECT_NO_FATAL_FAILURE(hle_libgs_GsDefDispBuff(&ctx));
  EXPECT_NO_FATAL_FAILURE(hle_libgs_GsSetWorkBase(&ctx));
  EXPECT_NO_FATAL_FAILURE(hle_libgs_GsSortClear(&ctx));
  EXPECT_TRUE(gp0.empty());
  EXPECT_TRUE(gp1.empty());
}

// SetDrawMode
//
// SetDrawMode(DR_MODE *p, int dfe, int dtd, int tpage, RECT *tw) must encode
// its arguments into the two GP0 words of the DR_MODE primitive.  The
// pre-2026-07 implementation wrote fixed 0xE1000000 / 0xE2000000
// placeholders, which forced texpage 0 on every textured primitive the game
// queued -- 456720 GP0(0x7C) sprites per 15s run, measured 2026-07-27.
// Texpage 0 points at VRAM (0,0), where the framebuffer band lives, instead
// of the loaded texture pages at (512..1023, 384). Those pages do hold real
// data (7944 distinct colours), so this is a wrong-address bug, not missing
// data. Encoding confirmed against the psyz decomp reference (workspace
// clone at PS1Recomp-workspace/psyz/decomp/src/libgpu/sys.c):
//   SetDrawMode: sys.c:503-506.
//   get_mode (retail/production-GPU branch -- info.version 0/3, per the 1MB
//     VRAM height table at sys.c:108; Crash targets retail hardware, not the
//     2MB-VRAM devkit branch taken when info.version is 1 or 2): sys.c:624-630.
//   get_tw: sys.c:662-673.
// 5th arg (RECT *tw) travels on the stack at sp+16 (o32 ABI), confirmed via
// the recompiled_out.cpp callers of func_80041054 (SetDrawMode's HLE'd
// address): MEM_WRITE32(ctx, ctx->r29 + 16, ...).

TEST_F(PsyqGpuTest, SetDrawModeEncodesTexpageAndFlagsIntoGp0Word) {
  psyq_register_libgpu_extras();
  const uint32_t p = 0x80100000u;
  const int dfe = 1, dtd = 0;
  const int tpage = 0x1A; // texture page base the game would pass

  ctx.r[SP] = 0x801FFF00u;
  ctx.r[A0] = p;
  ctx.r[A1] = static_cast<uint32_t>(dfe);
  ctx.r[A2] = static_cast<uint32_t>(dtd);
  ctx.r[A3] = static_cast<uint32_t>(tpage);
  mem.write32(ctx.r[SP] + 16u, 0); // RECT *tw = NULL

  psyq_dispatch("libgpu_SetDrawMode", &ctx);

  const uint32_t mode = mem.read32(p + 4);
  EXPECT_EQ(mode >> 24, 0xE1u) << "must remain a GP0(E1) command";
  EXPECT_EQ(mode & 0x1FFu, static_cast<uint32_t>(tpage) & 0x1FFu)
      << "texpage bits were dropped -- this is the 2026-07 regression";
  EXPECT_EQ((mode >> 10) & 1u, static_cast<uint32_t>(dfe));
  EXPECT_EQ((mode >> 9) & 1u, static_cast<uint32_t>(dtd));
  EXPECT_EQ(mem.read8(p + 3), 2u) << "DR_MODE len must stay 2";
}

// get_tw(NULL) returns the literal value 0 (sys.c:672 `return 0;`), not a
// bare GP0(0xE2) command -- a null texture window becomes a GP0(0x00) NOP,
// which psx-spx notes is often inserted between Texpage and Rectangle
// commands anyway.
TEST_F(PsyqGpuTest, SetDrawModeWithNullTextureWindowEmitsZeroWord) {
  psyq_register_libgpu_extras();
  const uint32_t p = 0x80100000u;
  ctx.r[SP] = 0x801FFF00u;
  ctx.r[A0] = p;
  ctx.r[A1] = 0;
  ctx.r[A2] = 0;
  ctx.r[A3] = 0;
  mem.write32(ctx.r[SP] + 16u, 0); // RECT *tw = NULL

  psyq_dispatch("libgpu_SetDrawMode", &ctx);

  EXPECT_EQ(mem.read32(p + 8), 0x00000000u);
}

TEST_F(PsyqGpuTest, SetDrawModeEncodesTextureWindowRectIntoGp0Word) {
  psyq_register_libgpu_extras();
  const uint32_t p  = 0x80100000u;
  const uint32_t tw = 0x80100100u;
  ctx.r[SP] = 0x801FFF00u;
  ctx.r[A0] = p;
  ctx.r[A1] = 0;
  ctx.r[A2] = 0;
  ctx.r[A3] = 0;
  writeRect(tw, /*x=*/8, /*y=*/16, /*w=*/64, /*h=*/32);
  mem.write32(ctx.r[SP] + 16u, tw);

  psyq_dispatch("libgpu_SetDrawMode", &ctx);

  // maskX = (-w & 0xFF) >> 3 = (-64 & 0xFF) >> 3 = 0x18
  // maskY = (-h & 0xFF) >> 3 = (-32 & 0xFF) >> 3 = 0x1C
  // offsX = (x & 0xFF) >> 3  = (8 & 0xFF)  >> 3  = 0x01
  // offsY = (y & 0xFF) >> 3  = (16 & 0xFF) >> 3  = 0x02
  // word = E2000000 | offsY<<15 | offsX<<10 | maskY<<5 | maskX
  const uint32_t expected = 0xE2000000u | (0x02u << 15) | (0x01u << 10) |
                             (0x1Cu << 5) | 0x18u;
  EXPECT_EQ(mem.read32(p + 8), expected);
}

// GetTPage
//
// GetTPage(tp, abr, x, y) packs a texpage description into the u_short that
// feeds SetDrawMode's `tpage` argument (see the SetDrawMode tests above) --
// an error here reintroduces the same wrong-texpage symptom through a
// different HLE. Encoding confirmed against the psyz decomp reference
// (workspace clone, PS1Recomp-workspace/psyz/): GetTPage forwards directly
// to the getTPage() macro (decomp/src/libgpu/prim.c:5-6 -- `return
// getTPage(tp, abr, x, y);`), defined at psyz/include/libgpu.h:183-185:
//   #define getTPage(tp, abr, x, y) \
//       ((((tp) & 0x3) << 7) | (((abr) & 0x3) << 5) | (((y) & 0x100) >> 4) | \
//        (((x) & 0x3ff) >> 6) | (((y) & 0x200) << 2))
// This is byte-for-byte what ps1Runtime's HLE already computes.
//
// Bit 11 (the `(y & 0x200) << 2` term) is psx-spx's GP0(E1h) "Texture page Y
// Base 2 (N*512), only for 2 MB VRAM" (workspace clone,
// PS1Recomp-workspace/psx-spx.github.io/docs/graphicsprocessingunitgpu.md:385,
// 899). It is part of the confirmed PsyQ macro -- the source does not mask
// it out -- so it stays. It is inert on this project's target: SetDrawMode
// already forwards it unmasked (`tpage & 0x9FF`), but gpu.cpp separately
// discards bit 11 when latching the mode word (`currentTexpage_ = cmd &
// 0x1FF`, gpu.cpp:766), and this target's y coordinates never reach 512, so
// no rendering path currently observes it (2026-07-22 project finding).
// Dropping the term here would be an undocumented deviation from the
// confirmed source, not a fix -- so the audit keeps it and documents why.

namespace {
uint32_t referenceGetTPage(int tp, int abr, int x, int y) {
  return static_cast<uint32_t>(((tp & 0x3) << 7) | ((abr & 0x3) << 5) |
                                ((y & 0x100) >> 4) | ((x & 0x3FF) >> 6) |
                                ((y & 0x200) << 2)) &
         0xFFFFu;
}
} // namespace

// x=0,y=0 / x=512,y=0 / x=768,y=384 are the real Crash Bandicoot texture
// page origins (see the SetDrawMode regression comment above). Sweep every
// tp (0-3) and abr (0-3) at each origin against the source-derived formula,
// not the implementation under test.
TEST_F(PsyqGpuTest, GetTPageEncodesRealCrashTexturePages) {
  psyq_register_libgpu_extras();
  struct Origin {
    int x, y;
  };
  const Origin origins[] = {{0, 0}, {512, 0}, {768, 384}};
  for (const Origin &o : origins) {
    for (int tp = 0; tp < 4; ++tp) {
      for (int abr = 0; abr < 4; ++abr) {
        ctx.reset();
        ctx.mem = &mem;
        ctx.r[A0] = static_cast<uint32_t>(tp);
        ctx.r[A1] = static_cast<uint32_t>(abr);
        ctx.r[A2] = static_cast<uint32_t>(o.x);
        ctx.r[A3] = static_cast<uint32_t>(o.y);

        psyq_dispatch("libgpu_GetTPage", &ctx);

        EXPECT_EQ(ctx.r[V0], referenceGetTPage(tp, abr, o.x, o.y))
            << "tp=" << tp << " abr=" << abr << " x=" << o.x
            << " y=" << o.y;
      }
    }
  }
}

// Isolates the two y-derived bits the table sweep above exercises only in
// combination: bit 4 of the result (y's bit 8, in-range VRAM Y) and bit 11
// (y's bit 9, the 2 MB-VRAM term kept per the source, see block comment
// above).
TEST_F(PsyqGpuTest, GetTPageEncodesYBit8AndBit11FromSource) {
  psyq_register_libgpu_extras();
  struct Case {
    int y;
    uint32_t expected;
  };
  const Case cases[] = {
      {0, 0x0000u},
      {256, 0x0010u}, // y bit 8 -> result bit 4
      {512, 0x0800u}, // y bit 9 -> result bit 11 (2 MB VRAM term)
      {768, 0x0810u}, // both bits set together
  };
  for (const Case &c : cases) {
    ctx.reset();
    ctx.mem = &mem;
    ctx.r[A0] = 0;
    ctx.r[A1] = 0;
    ctx.r[A2] = 0;
    ctx.r[A3] = static_cast<uint32_t>(c.y);

    psyq_dispatch("libgpu_GetTPage", &ctx);

    EXPECT_EQ(ctx.r[V0], c.expected) << "y=" << c.y;
  }
}

// Registry wiring

TEST_F(PsyqGpuTest, RegistryDispatchesAllNewNames) {
  psyq_register_libgpu_extras();
  // Each name must dispatch without aborting. For HLEs that read RAM (rect /
  // src), give them a valid empty rect so no actual GP0 traffic is generated.
  uint32_t rectP = 0x80100400u;
  writeRect(rectP, 0, 0, 0, 0);

  const char *names[] = {
      "libgpu_SetDispMask",      "libgpu_LoadImage",
      "libgpu_StoreImage",       "libgpu_MoveImage",
      "libgpu_ClearImage",       "libgpu_DrawSyncCallback",
      "libetc_VSyncCallback",    "libetc_SetVideoMode",
      "libetc_GetVideoMode",
      "libgs_GsInitGraph",       "libgs_GsDefDispBuff",
      "libgs_GsSetWorkBase",     "libgs_GsSortClear",
  };
  for (const char *n : names) {
    ctx.reset();
    ctx.mem = &mem;
    ctx.r[A0] = rectP; // safe for both rect-takers and arg-takers
    EXPECT_NO_FATAL_FAILURE(psyq_dispatch(n, &ctx))
        << "dispatch failed for: " << n;
  }
}
