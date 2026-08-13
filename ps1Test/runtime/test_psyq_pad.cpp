// Tests for the libetc Pad/Controller HLEs (Group 1.E).
//
// Strategy:
//   - Build a Bios + InputController pair and attach the controller via
//     setInputController so the HLEs can resolve `bios->inputController()`.
//   - Drive the InputController's `press`/`release`/`setPadType` directly,
//     then assert PadRead packs the active-low button words into $v0.
//   - PadInitDirect's status-buffer refresh is checked by reading PS1 RAM
//     after the call (status / type / button-low / button-high bytes).
//   - Two extra tests cover the no-input fallback (returns 0xFFFFFFFF) and
//     PadGetState's pad-type -> state mapping.
//   - One end-to-end registry test confirms each name dispatches after
//     `psyq_register_libetc_pad()`.
//
// We do NOT use a Bios fixture for the no-input fallback path so we can
// observe the "ctx->bios == nullptr" branch as well.

#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/input/input.h"
#include "runtime/memory.h"
#include "runtime/psyq/psyq_pad.h"
#include "runtime/psyq/psyq_registry.h"

#include <gtest/gtest.h>
#include <memory>

using namespace ps1;
using namespace ps1::psyq;

namespace {

class PsyqPadTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  cdrom::VirtualFs fs;
  input::InputController input;
  std::unique_ptr<bios::Bios> bios;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    bios = std::make_unique<bios::Bios>(ctx, fs, mem);
    bios->setInputController(&input);
    ctx.bios = bios.get();

    // Both ports default to Digital with all buttons released (0xFFFF).
    input.reset();
    input.setPadType(0, input::PadType::Digital);
    input.setPadType(1, input::PadType::Digital);

    psyq_pad_reset_for_tests();
  }

  void TearDown() override { psyq_pad_reset_for_tests(); }
};

} // namespace

// PadInit / PadStartCom / PadStopCom -- bookkeeping NOPs

TEST_F(PsyqPadTest, PadInitReturnsZero) {
  ctx.r[A0] = 0;
  hle_libetc_PadInit(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqPadTest, PadStartComStopComReturnZero) {
  hle_libetc_PadStartCom(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
  hle_libetc_PadStopCom(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);
}

// PadRead -- packed (port2 << 16) | port1, active-low

TEST_F(PsyqPadTest, PadReadIdleReturnsAllOnes) {
  // No buttons pressed on either port -> 0xFFFFFFFF.
  hle_libetc_PadRead(&ctx);
  EXPECT_EQ(ctx.r[V0], 0xFFFFFFFFu);
}

TEST_F(PsyqPadTest, PadReadPressedBitsAreCleared) {
  // Press CROSS on port 0 -> bit 14 cleared in low half.
  input.press(input::BTN_CROSS, 0);
  hle_libetc_PadRead(&ctx);
  uint32_t expected = 0xFFFFFFFFu & ~static_cast<uint32_t>(input::BTN_CROSS);
  EXPECT_EQ(ctx.r[V0], expected);

  // Add START on port 1 -> bit 3 of high half cleared.
  input.press(input::BTN_START, 1);
  hle_libetc_PadRead(&ctx);
  expected = (~static_cast<uint32_t>(input::BTN_CROSS) & 0xFFFFu) |
             ((~static_cast<uint32_t>(input::BTN_START) & 0xFFFFu) << 16);
  EXPECT_EQ(ctx.r[V0], expected);
}

// Ground truth verified directly against the Crash Bandicoot (SCUS-94900)
// retail binary: `PadUpdate` at VA 0x800167A4 loops the port index in $s1 and
// picks the half with `bnez $s1, 0x80016818` before a delay-slot `srl
// $v1,$v0,0x10` -- port 0 (s1==0) falls through to `andi $v1,$v0,0xffff`
// (low half), port 1 (s1!=0) keeps the `srl` result (high half). This
// matches CRASH_BANDICOOT_RECOMP.md Sec 2.1's account of the reference
// implementation's "wrong halfword" trap (port 0 must be the low 16 bits),
// and confirms our packing already has it right -- this test pins that so a
// future change to hle_libetc_PadRead can't silently swap the halves.
TEST_F(PsyqPadTest, PadReadPort0IsLowHalfPerRetailPadUpdateDisassembly) {
  input.press(input::BTN_CROSS, 0);  // port 0 (low half)
  input.press(input::BTN_SQUARE, 1); // port 1 (high half)
  hle_libetc_PadRead(&ctx);

  uint16_t lowHalf = static_cast<uint16_t>(ctx.r[V0] & 0xFFFFu);
  uint16_t highHalf = static_cast<uint16_t>((ctx.r[V0] >> 16) & 0xFFFFu);

  // CROSS (port 0) must land in the low half, not the high half.
  EXPECT_EQ(lowHalf, static_cast<uint16_t>(0xFFFFu & ~input::BTN_CROSS));
  EXPECT_NE(highHalf, static_cast<uint16_t>(0xFFFFu & ~input::BTN_CROSS));

  // SQUARE (port 1) must land in the high half, not the low half.
  EXPECT_EQ(highHalf, static_cast<uint16_t>(0xFFFFu & ~input::BTN_SQUARE));
  EXPECT_NE(lowHalf, static_cast<uint16_t>(0xFFFFu & ~input::BTN_SQUARE));
}

TEST_F(PsyqPadTest, PadReadReflectsRelease) {
  input.press(input::BTN_CIRCLE, 0);
  hle_libetc_PadRead(&ctx);
  EXPECT_EQ(ctx.r[V0] & 0xFFFFu,
            static_cast<uint32_t>(0xFFFFu & ~input::BTN_CIRCLE));

  input.release(input::BTN_CIRCLE, 0);
  hle_libetc_PadRead(&ctx);
  EXPECT_EQ(ctx.r[V0], 0xFFFFFFFFu);
}

TEST_F(PsyqPadTest, PadReadFallsBackWhenNoBackend) {
  // Detach the input controller -- bios accessor returns nullptr.
  bios->setInputController(nullptr);
  hle_libetc_PadRead(&ctx);
  EXPECT_EQ(ctx.r[V0], 0xFFFFFFFFu);
}

// PadInitDirect -- 34-byte status buffer refresh

TEST_F(PsyqPadTest, PadInitDirectSeedsBufferHeader) {
  uint32_t buf1 = 0x80120000u;
  uint32_t buf2 = 0x80120100u;

  ctx.r[A0] = buf1;
  ctx.r[A1] = buf2;
  hle_libetc_PadInitDirect(&ctx);
  EXPECT_EQ(ctx.r[V0], 0u);

  EXPECT_EQ(mem.read8(buf1 + 0), 0x00); // status OK
  EXPECT_EQ(mem.read8(buf1 + 1), static_cast<uint8_t>(input::PadType::Digital));
  EXPECT_EQ(mem.read8(buf1 + 2), 0xFF); // no buttons
  EXPECT_EQ(mem.read8(buf1 + 3), 0xFF);

  EXPECT_EQ(mem.read8(buf2 + 0), 0x00);
  EXPECT_EQ(mem.read8(buf2 + 1), static_cast<uint8_t>(input::PadType::Digital));
}

TEST_F(PsyqPadTest, PadInitDirectAcceptsNullSecondBuffer) {
  // Single-port wiring: buf2 = 0 must be tolerated without writing anywhere.
  uint32_t buf1 = 0x80120000u;
  ctx.r[A0] = buf1;
  ctx.r[A1] = 0;
  hle_libetc_PadInitDirect(&ctx);
  EXPECT_EQ(mem.read8(buf1 + 0), 0x00);
}

TEST_F(PsyqPadTest, PadReadRefreshesDirectBufferButtons) {
  uint32_t buf1 = 0x80120000u;
  ctx.r[A0] = buf1;
  ctx.r[A1] = 0;
  hle_libetc_PadInitDirect(&ctx);

  // Press SQUARE (bit 15) on port 0 -- high byte should drop bit 7.
  input.press(input::BTN_SQUARE, 0);
  hle_libetc_PadRead(&ctx);

  uint16_t expected = 0xFFFFu & ~input::BTN_SQUARE;
  EXPECT_EQ(mem.read8(buf1 + 2), static_cast<uint8_t>(expected & 0xFF));
  EXPECT_EQ(mem.read8(buf1 + 3),
            static_cast<uint8_t>((expected >> 8) & 0xFF));
}

TEST_F(PsyqPadTest, PadInitDirectMarksMissingControllerInBuffer) {
  // Detach port 1 by switching it to PadType::None -- header should flip to
  // 0xFF/0xFF so the game's "no controller" branch fires.
  input.setPadType(1, input::PadType::None);

  uint32_t buf1 = 0x80120000u;
  uint32_t buf2 = 0x80120100u;
  ctx.r[A0] = buf1;
  ctx.r[A1] = buf2;
  hle_libetc_PadInitDirect(&ctx);

  EXPECT_EQ(mem.read8(buf2 + 0), 0xFF);
  EXPECT_EQ(mem.read8(buf2 + 1), 0xFF);
  // Port 0 still healthy.
  EXPECT_EQ(mem.read8(buf1 + 1), static_cast<uint8_t>(input::PadType::Digital));
}

// PadGetState -- collapsed two-state mapping

TEST_F(PsyqPadTest, PadGetStateStableWhenPadAttached) {
  ctx.r[A0] = 0;
  hle_libetc_PadGetState(&ctx);
  EXPECT_EQ(ctx.r[V0], static_cast<uint32_t>(PAD_STATE_STABLE));
}

TEST_F(PsyqPadTest, PadGetStateDiscoveryWhenPadAbsent) {
  input.setPadType(0, input::PadType::None);
  ctx.r[A0] = 0;
  hle_libetc_PadGetState(&ctx);
  EXPECT_EQ(ctx.r[V0], static_cast<uint32_t>(PAD_STATE_DISCOVERY));
}

TEST_F(PsyqPadTest, PadGetStateOutOfRangePort) {
  ctx.r[A0] = 5; // bogus port id
  hle_libetc_PadGetState(&ctx);
  EXPECT_EQ(ctx.r[V0], static_cast<uint32_t>(PAD_STATE_DISCOVERY));
}

// Registry coverage

TEST_F(PsyqPadTest, RegistryDispatchesAllPadNames) {
  psyq_register_libetc_pad();

  ctx.r[A0] = 0;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libetc_PadInit", &ctx));
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libetc_PadStartCom", &ctx));
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libetc_PadStopCom", &ctx));

  ctx.r[A0] = 0x80120000u;
  ctx.r[A1] = 0x80120100u;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libetc_PadInitDirect", &ctx));

  ctx.r[A0] = 0;
  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libetc_PadGetState", &ctx));

  EXPECT_NO_FATAL_FAILURE(psyq_dispatch("libetc_PadRead", &ctx));
}
