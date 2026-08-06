#include "runtime/psyq/psyq_hle.h"
#include "runtime/emuptr.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/psyq/psyq_state.h"
#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <thread>

namespace ps1::psyq {

// Module-level config
static HleConfig g_cfg;

void configure(const HleConfig &cfg) { g_cfg = cfg; }

const HleConfig &getConfig() { return g_cfg; }

// Helpers

static inline void drainOnce() {
  if (g_cfg.drainCallbacks)
    g_cfg.drainCallbacks();
}

// VSync
//
// PsyQ VSync(n):
//   n == 0 -> sync to next VBlank (wait for counter to change)
//   n  > 0 -> wait until n more VBlanks have elapsed
//   Returns the total VBlank counter value.
//
// Reads psyq_state().vsyncCounter, the C++-side singleton incremented by
// the host VBlank thread (~60 Hz, ~16.6 ms period).  Each iteration:
// drainOnce() pumps the BIOS callback queue (no-op fast path when empty),
// then sleep 100 us to yield CPU before the next atomic poll.  A 1 s
// wall-clock deadline (~60 frames at 60 Hz) guards against deadlock when
// the VBlank thread is not running.
//
// Phase 3.2: after the wait loop, exchange `psyq_state().vblankPending`
// to false.  When the flag was observed `true`, run `deliverVBlankEvent`
// on this (game) thread -- that is what now performs the actual PsyQ
// VBlank work (event-system triggers, queued swap callback, drawSync
// stamping) which used to run on the IRQ-context VBlank thread.  The
// exchange both observes and clears the flag atomically, so two
// consecutive `hle_VSync` calls without an intervening VBlank tick
// cannot drain the same VBlank twice.
//
void hle_VSync(recomp_context *ctx) {
  int32_t n = static_cast<int32_t>(ctx->r[A0]);
  uint32_t frames = (n <= 0) ? 1u : static_cast<uint32_t>(n);

  auto &counter = psyq_state().vsyncCounter;
  uint32_t start = counter.load(std::memory_order_acquire);
  uint32_t target = start + frames;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (counter.load(std::memory_order_acquire) < target &&
         std::chrono::steady_clock::now() < deadline) {
    drainOnce();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  if (psyq_state().vblankPending.exchange(false, std::memory_order_acq_rel) &&
      g_cfg.deliverVBlankEvent) {
    g_cfg.deliverVBlankEvent();
  }

  ctx->r[V0] = counter.load(std::memory_order_acquire);
}

// DrawSync
//
// PsyQ DrawSync(mode):
//   mode 0 -> wait until GPU drawing is complete, return 0
//   mode 1 -> return number of primitives remaining (non-blocking)
//
// Since the runtime GPU processes GP0 commands synchronously, drawing is
// always "complete".  Return 0 for both modes.
//
void hle_DrawSync(recomp_context *ctx) {
  // Drain once to keep event queue healthy
  drainOnce();
  ctx->r[V0] = 0; // 0 = complete / 0 primitives remaining
}

// ResetGraph
//
// PsyQ ResetGraph(mode):
//   mode 0 -> reset + flush + clear display list
//   mode 3 -> flush only
//
// The runtime GPU has no queued command list to flush, so this is a NOP.
//
void hle_ResetGraph(recomp_context *ctx) {
  (void)ctx;
  // NOP: runtime GPU is synchronous, no list to flush
}

// ClearOTag
//
// PsyQ ClearOTag(ot, n):
//   Fills the first `n` entries of ordering table `ot` with end-of-list
//   terminators.  On real hardware each entry is a 24-bit linked-list pointer
//   that forms a chain; the last entry (ot[0]) must hold 0x00FFFFFF.
//
//   a0 = base address of the OT (uint32_t*)
//   a1 = number of entries
//
void hle_ClearOTag(recomp_context *ctx) {
  uint32_t base = ctx->r[A0];
  uint32_t n = ctx->r[A1];
  if (n == 0) {
    ctx->r[V0] = base;
    return;
  }
  // Fill [1 .. n-1] with self-pointers (each entry points to previous)
  for (uint32_t i = 0; i < n - 1; i++) {
    ctx->mem->write32(base + i * 4, base + (i - 1) * 4);
  }
  // Last entry = 0x00FFFFFF (end-of-list sentinel)
  ctx->mem->write32(base + (n - 1) * 4, 0x00FFFFFFu);
  ctx->r[V0] = base; // return pointer to ot
}

// ClearOTagR
//
// Same as ClearOTag but fills in reverse order -- entries are linked
// high-to-low so GPU traverses them from ot[n-1] down to ot[0].
//
void hle_ClearOTagR(recomp_context *ctx) {
  uint32_t base = ctx->r[A0];
  uint32_t n = ctx->r[A1];
  if (n == 0) {
    ctx->r[V0] = base;
    return;
  }
  // ot[n-1] = end-of-list
  ctx->mem->write32(base + (n - 1) * 4, 0x00FFFFFFu);
  // ot[i] links forward: ot[i] -> ot[i+1]
  for (uint32_t i = 0; i < n - 1; i++) {
    ctx->mem->write32(base + i * 4, base + (i + 1) * 4);
  }
  ctx->r[V0] = base;
}

// DrawOTag
//
// PsyQ DrawOTag(ot):
//   Traverses the ordering-table linked list and submits each primitive's
//   GP0 command words to the GPU, in order from tail (a0) to head.
//
//   Node format: header word [31:24]=word_count [23:0]=next_ptr_physical
//   Followed by word_count GP0 data words.
//   Terminal node: next_ptr == 0x00FFFFFF.
//
// Audited 2026-08-06: DrawOTag(p) itself (psyz decomp/src/libgpu/sys.c:354-360)
// just forwards to `addque2(cwc, p, 0, 0)`, and _cwc (sys.c:850-855) programs
// DMA2 in "linked-list" mode (CHCR=0x01000401) with MADR=p -- on real
// hardware the GPU's DMA controller itself walks the OT and streams the
// words to GP0, the CPU does not decode headers at all. This function is
// this project's software model of that DMA-controller traversal, so the
// node format it must match is the DMA2 linked-list format, not sys.c
// (which never decodes it in software). That format is documented directly
// in psx-spx (docs/dmachannels.md:173-190, "Linked List DMA"):
//   bits 0-23  = address of next node (or all-1s end marker, 0xFFFFFF)
//   bits 24-31 = number of extra (data) words in this node
// and in docs/graphicsprocessingunitgpu.md:1084-1136 ("Depth Ordering
// Table"), whose worked example confirms only the N data words after the
// header are sent to GP0 -- the header word itself is link-chain metadata,
// never pushed as a GP0 command. Below already matches this exactly: header
// decode (wordCount = bits 31:24, next = bits 23:0), per-node GP0 pushes
// limited to the N words following the header, KSEG0-bit restore on the
// 24-bit physical next-pointer, and termination on next == 0xFFFFFF. No
// change was needed; see DrawOTagEmptyListEmitsNothing /
// DrawOTagSubmitsSinglePrimitive / DrawOTagTraversesChain /
// DrawOTagHeaderWordItselfIsNeverPushedToGp0 in test_psyq_hle.cpp.
//
void hle_DrawOTag(recomp_context *ctx) {
  if (!g_cfg.writeGP0) {
    ctx->r[V0] = 0;
    return;
  }
  ps1::metrics::count("draw_otag.calls");
  uint32_t ptr = ctx->r[A0];
  int safety = 0;
  while ((ptr & 0xFFFFFFu) != 0xFFFFFFu && safety++ < 100000) {
    uint32_t hdr = ctx->mem->read32(ptr);
    uint32_t wordCount = (hdr >> 24) & 0xFF;
    ps1::metrics::count("draw_otag.nodes");
    ps1::metrics::count("draw_otag.words", wordCount);
    for (uint32_t i = 0; i < wordCount; i++) {
      g_cfg.writeGP0(ctx->mem->read32(ptr + 4 + i * 4));
    }
    uint32_t next = hdr & 0xFFFFFFu;
    if (next == 0xFFFFFFu)
      break;
    ptr = next | 0x80000000u; // restore KSEG0 bit
  }
  ctx->r[V0] = 0;
}

// SetDefDispEnv
//
// PsyQ SetDefDispEnv(env, x, y, w, h):
//   Initialises a DispEnv struct in PS1 RAM.
//
//   PS1 DispEnv layout (from PsyQ SDK headers):
//     +0  disp.x   (int16)  display area X in VRAM
//     +2  disp.y   (int16)  display area Y in VRAM
//     +4  disp.w   (int16)  display width
//     +6  disp.h   (int16)  display height
//     +8  screen.x (int16)  (unused / screen clip)
//     +10 screen.y (int16)
//     +12 screen.w (int16)
//     +14 screen.h (int16)
//     +16 isinter  (uint8)  interlace enable
//     +17 isrgb24  (uint8)  24bpp enable
//     +18 pad[2]
//
namespace {
struct DispEnv {
  int16_t disp_x, disp_y;     // 0, 2 -- VRAM display area origin
  int16_t disp_w, disp_h;     // 4, 6
  int16_t screen_x, screen_y; // 8, 10
  int16_t screen_w, screen_h; // 12, 14
  uint8_t isinter;            // 16
  uint8_t isrgb24;            // 17
  uint8_t pad[2];             // 18, 19
};
static_assert(sizeof(DispEnv) == 20, "PsyQ DispEnv is 20 bytes");
} // namespace

void hle_SetDefDispEnv(recomp_context *ctx) {
  ps1::emuptr<DispEnv> env(ctx->r[A0]);
  int16_t x = static_cast<int16_t>(ctx->r[A1]);
  int16_t y = static_cast<int16_t>(ctx->r[A2]);
  int16_t w = static_cast<int16_t>(ctx->r[A3]);
  // h is on the stack (5th argument)
  int16_t h = static_cast<int16_t>(ctx->mem->read32(ctx->r[SP] + 16));

  env->disp_x   = x;
  env->disp_y   = y;
  env->disp_w   = w;
  env->disp_h   = h;
  env->screen_x = 0;
  env->screen_y = 0;
  env->screen_w = w;
  env->screen_h = h;
  env->isinter  = 0; // non-interlaced
  env->isrgb24  = 0; // RGB15
  env->pad[0]   = 0;
  env->pad[1]   = 0;

  ctx->r[V0] = static_cast<uint32_t>(env);
}

// PutDispEnv
//
// PsyQ PutDispEnv(env):
//   Applies a DispEnv to the GPU by sending GP1 commands:
//     GP1(0x05) -- set display start (VRAM X/Y)
//     GP1(0x06) -- set horizontal display range
//     GP1(0x07) -- set vertical   display range
//     GP1(0x08) -- set display mode (width, height, interlace)
//
// Audited 2026-08-06 against the psyz decomp reference (workspace clone,
// PS1Recomp-workspace/psyz/decomp/src/libgpu/sys.c:399-461). PutDispEnv
// branches on info.version (GPU hardware type); the same retail/1MB-VRAM
// branch this project's PutDrawEnv audit established applies here
// (info.version 0/3) is used below:
//
//   GP1(0x05) -- sys.c:409-412: (disp.y & 0x3FF) << 10 | (disp.x & 0x3FF).
//     The pre-audit code masked vx but not vy; an unmasked negative vy can
//     shift into bits 24-26 and corrupt the 0x05 command nibble itself.
//   GP1(0x06)/(0x07) -- sys.c:413-427: computed from DISPENV.screen (NOT
//     .disp), with defaults when screen.w/h == 0 and hard clamps. This
//     project only targets NTSC (GetVideoMode() default 0 / pad0 == false),
//     so the PAL branch (0x13/310/312 constants) is not implemented; the
//     pre-audit code used a fabricated disp.w/h-based formula that matched
//     neither branch.
//   GP1(0x08) -- sys.c:404, 428-457: bit layout matches psx-spx's documented
//     GP1(08h) fields (docs/graphicsprocessingunitgpu.md) exactly, the same
//     layout gpu.cpp's case 0x08 consumer already assumes. The pre-audit
//     code put isrgb24 at bit5 and isinter at bit6 (both wrong) and never
//     set the Hres2/368-mode bit, so producer and consumer disagreed.
//     info.reverse (SetGraphReverse) is not tracked by this runtime, so
//     that bit (0x80) is never set -- no game exercises it here.
//
void hle_PutDispEnv(recomp_context *ctx) {
  if (!g_cfg.writeGP1) {
    return;
  }
  uint32_t envPtr = ctx->r[A0];
  int16_t vx   = static_cast<int16_t>(ctx->mem->read16(envPtr + 0));
  int16_t vy   = static_cast<int16_t>(ctx->mem->read16(envPtr + 2));
  int16_t vw   = static_cast<int16_t>(ctx->mem->read16(envPtr + 4));
  int16_t vh   = static_cast<int16_t>(ctx->mem->read16(envPtr + 6));
  int16_t sx   = static_cast<int16_t>(ctx->mem->read16(envPtr + 8));
  int16_t sy   = static_cast<int16_t>(ctx->mem->read16(envPtr + 10));
  int16_t sw   = static_cast<int16_t>(ctx->mem->read16(envPtr + 12));
  int16_t sh   = static_cast<int16_t>(ctx->mem->read16(envPtr + 14));
  uint8_t isinter = ctx->mem->read8(envPtr + 16);
  uint8_t isrgb24 = ctx->mem->read8(envPtr + 17);

  // GP1(0x05): display start address
  g_cfg.writeGP1(0x05000000u | ((static_cast<uint32_t>(vy) & 0x3FFu) << 10) |
                                 (static_cast<uint32_t>(vx) & 0x3FFu));

  // GP1(0x06)/(0x07): horizontal/vertical display range from DISPENV.screen
  // (NTSC constants: v_start offset 0x10, v clamp bounds 256/258).
  int32_t hStart = static_cast<int32_t>(sx) * 10 + 0x260;
  int32_t hEnd   = hStart + (sw != 0 ? static_cast<int32_t>(sw) * 10 : 2560);
  int32_t vStart = static_cast<int32_t>(sy) + 0x10;
  int32_t vEnd   = vStart + (sh != 0 ? static_cast<int32_t>(sh) : 240);
  hStart = std::clamp(hStart, 500, 3290);
  hEnd   = std::clamp(hEnd, hStart + 0x50, 3290);
  vStart = std::clamp(vStart, 0x10, 256);
  vEnd   = std::clamp(vEnd, vStart + 2, 258);
  g_cfg.writeGP1(0x06000000u | ((static_cast<uint32_t>(hEnd) & 0xFFFu) << 12) |
                                 (static_cast<uint32_t>(hStart) & 0xFFFu));
  g_cfg.writeGP1(0x07000000u | ((static_cast<uint32_t>(vEnd) & 0x3FFu) << 10) |
                                 (static_cast<uint32_t>(vStart) & 0x3FFu));

  // GP1(0x08): display mode
  uint32_t mode = 0;
  if (isrgb24) mode |= 0x10u;
  if (isinter) mode |= 0x20u;
  if (vw <= 280) {
    // Hres1 = 0 (256)
  } else if (vw <= 352) {
    mode |= 0x01u; // Hres1 = 1 (320)
  } else if (vw <= 400) {
    mode |= 0x40u; // Hres2 = 1 (368)
  } else if (vw <= 560) {
    mode |= 0x02u; // Hres1 = 2 (512)
  } else {
    mode |= 0x03u; // Hres1 = 3 (640)
  }
  if (vh > 256) {
    mode |= 0x24u; // Vres (bit2) + interlace (bit5) combo for >256-line modes
  }
  g_cfg.writeGP1(0x08000000u | mode);
}

// SetDefDrawEnv
//
// PsyQ SetDefDrawEnv(env, x, y, w, h):
//   Initialises a DrawEnv struct in PS1 RAM.
//
//   PS1 DrawEnv layout (from PsyQ SDK headers):
//     +0  clip.x   (int16)  clipping area X
//     +2  clip.y   (int16)  clipping area Y
//     +4  clip.w   (int16)  clipping width
//     +6  clip.h   (int16)  clipping height
//     +8  ofs[0]   (int16)  drawing X offset
//     +10 ofs[1]   (int16)  drawing Y offset
//     +12 tw ...   (8 bytes, texture window)
//     +20 tpage    (uint16) texture page
//     +22 dtd      (uint8)  dithering
//     +23 dfe      (uint8)  draw-to-display enable
//     ... (DR_TPAGE follows)
//
// Audited 2026-08-06 against the psyz decomp reference (workspace clone,
// PS1Recomp-workspace/psyz/decomp/src/libgpu/ext.c:46-69):
//   env->tpage = getTPage(0, 0, 640, 0)       -- always this fixed value,
//     independent of the x/y/w/h args, evaluating (via this project's own
//     already-audited GetTPage formula, psyq_libgpu.cpp) to
//     (640 & 0x3FF) >> 6 = 0x0A.
//   env->dfe = video_mode ? h <= 288 : h <= 256 -- this project has no PAL
//     path (GetVideoMode() defaults to and stays NTSC/0), so the h <= 256
//     branch is used unconditionally.
// The pre-audit implementation hardcoded tpage=0 and dfe=0, ignoring both
// GetTPage and the height threshold.
void hle_SetDefDrawEnv(recomp_context *ctx) {
  uint32_t envPtr = ctx->r[A0];
  int16_t  x  = static_cast<int16_t>(ctx->r[A1]);
  int16_t  y  = static_cast<int16_t>(ctx->r[A2]);
  int16_t  w  = static_cast<int16_t>(ctx->r[A3]);
  int16_t  h  = static_cast<int16_t>(ctx->mem->read32(ctx->r[SP] + 16));

  // clip rect
  ctx->mem->write16(envPtr + 0, static_cast<uint16_t>(x));
  ctx->mem->write16(envPtr + 2, static_cast<uint16_t>(y));
  ctx->mem->write16(envPtr + 4, static_cast<uint16_t>(w));
  ctx->mem->write16(envPtr + 6, static_cast<uint16_t>(h));
  // drawing offset = clip origin
  ctx->mem->write16(envPtr + 8,  static_cast<uint16_t>(x));
  ctx->mem->write16(envPtr + 10, static_cast<uint16_t>(y));
  // texture window: no restriction (all zeros)
  ctx->mem->write16(envPtr + 12, 0);
  ctx->mem->write16(envPtr + 14, 0);
  ctx->mem->write16(envPtr + 16, 0);
  ctx->mem->write16(envPtr + 18, 0);
  // tpage = getTPage(0, 0, 640, 0) = 0x0A (ext.c:67), dtd=1 (dithering)
  ctx->mem->write16(envPtr + 20, 0x000Au);
  ctx->mem->write8(envPtr + 22, 1); // dtd
  // dfe = h <= 256 (NTSC branch, ext.c:60-64)
  ctx->mem->write8(envPtr + 23, h <= 256 ? 1 : 0);

  ctx->r[V0] = envPtr;
}

// PutDrawEnv
//
// PsyQ PutDrawEnv(env):
//   Applies a DrawEnv to the GPU by sending GP0 commands:
//     GP0(0xE1) -- texture page (tpage)
//     GP0(0xE2) -- texture window
//     GP0(0xE3) -- drawing area top-left
//     GP0(0xE4) -- drawing area bottom-right
//     GP0(0xE5) -- drawing offset
//
// Audited 2026-07-28 against the psyz decomp reference (workspace clone,
// PS1Recomp-workspace/psyz/decomp/src/libgpu/sys.c): PutDrawEnv builds its
// GP0(0xE1) word via SetDrawEnv2 -> get_mode(env->dfe, env->dtd, env->tpage)
// (sys.c:362-373, 561-573), the same get_mode() this project's SetDrawMode
// audit already confirmed (sys.c:624-632, retail/1MB-VRAM branch --
// info.version 0/3, applicable to this target per that audit):
//   (dtd ? 0xE1000200 : 0xE1000000) | (dfe ? 0x400 : 0) | (tpage & 0x9FF)
// The pre-audit implementation read only dtd and masked tpage with 0x7FF
// (bits 0-10) instead of 0x9FF (bits 0-8 + bit 11). Per psx-spx (GP0(E1h)),
// bits 9-10 are exclusively Dither/Draw-to-display-area, supplied by
// dtd/dfe -- never by the raw tpage value -- so 0x7FF let stray high bits of
// env->tpage leak into those flags, and env->dfe (DRAWENV +0x17, offset 23;
// psyz/include/libgpu.h:565-575) was never read at all.
//
// Known gap (not fixed in this pass): SetDrawEnv2 always also emits
// GP0(0xE2) Texture Window and GP0(0xE6) Mask Bit Setting (sys.c:574-575,
// 601), which this implementation still does not. Fixing that would change
// this function's GP0 word count, which would break test_psyq_hle.cpp's
// exact-size assertions -- a file outside this task's edit scope. Left
// documented for a follow-up that can touch that file.
void hle_PutDrawEnv(recomp_context *ctx) {
  if (!g_cfg.writeGP0) {
    return;
  }
  uint32_t envPtr = ctx->r[A0];
  int16_t cx  = static_cast<int16_t>(ctx->mem->read16(envPtr + 0));
  int16_t cy  = static_cast<int16_t>(ctx->mem->read16(envPtr + 2));
  int16_t cw  = static_cast<int16_t>(ctx->mem->read16(envPtr + 4));
  int16_t ch  = static_cast<int16_t>(ctx->mem->read16(envPtr + 6));
  int16_t ox  = static_cast<int16_t>(ctx->mem->read16(envPtr + 8));
  int16_t oy  = static_cast<int16_t>(ctx->mem->read16(envPtr + 10));
  uint16_t tpage = ctx->mem->read16(envPtr + 20);
  uint8_t dtd = ctx->mem->read8(envPtr + 22);
  uint8_t dfe = ctx->mem->read8(envPtr + 23);

  // GP0(0xE1): texture page + dithering + draw-to-display-area enable
  uint32_t e1 = (dtd ? 0xE1000200u : 0xE1000000u) |
                (dfe ? 0x400u : 0u) | (tpage & 0x9FFu);
  g_cfg.writeGP0(e1);

  // GP0(0xE3): drawing area top-left
  g_cfg.writeGP0(0xE3000000u | (static_cast<uint32_t>(cy & 0x1FF) << 10) |
                                 static_cast<uint32_t>(cx & 0x3FF));

  // GP0(0xE4): drawing area bottom-right (inclusive)
  int16_t x2 = static_cast<int16_t>(cx + cw - 1);
  int16_t y2 = static_cast<int16_t>(cy + ch - 1);
  g_cfg.writeGP0(0xE4000000u | (static_cast<uint32_t>(y2 & 0x1FF) << 10) |
                                 static_cast<uint32_t>(x2 & 0x3FF));

  // GP0(0xE5): drawing offset
  g_cfg.writeGP0(0xE5000000u |
                 (static_cast<uint32_t>(static_cast<int32_t>(oy) & 0x7FF) << 11) |
                 static_cast<uint32_t>(static_cast<int32_t>(ox) & 0x7FF));
}

} // namespace ps1::psyq
