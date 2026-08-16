// Computed in-function jump (Duff's device) detection.
//
// Ground truth is func_80033FBC in Crash Bandicoot (SCUS-94900), the PsyQ
// memcpy: an unrolled copy loop entered partway through.  Before this
// detector the `jr` emitted a runtime dispatch on a mid-function address and
// the game aborted with `unmapped call to 0x80034048` the moment the title
// screen accepted START.

#include <gtest/gtest.h>
#include <ps1recomp/jump_analysis.h>

#include <algorithm>

using namespace ps1recomp;

namespace {

// Hand-assembled MIPS words, so the test pins the encoding the detector must
// match rather than trusting a second copy of the pattern matcher.
constexpr uint32_t lui(uint8_t rt, uint16_t imm) {
  return (0x0Fu << 26) | (static_cast<uint32_t>(rt) << 16) | imm;
}
constexpr uint32_t addiu(uint8_t rt, uint8_t rs, uint16_t imm) {
  return (0x09u << 26) | (static_cast<uint32_t>(rs) << 21) |
         (static_cast<uint32_t>(rt) << 16) | imm;
}
constexpr uint32_t sltiu(uint8_t rt, uint8_t rs, uint16_t imm) {
  return (0x0Bu << 26) | (static_cast<uint32_t>(rs) << 21) |
         (static_cast<uint32_t>(rt) << 16) | imm;
}
constexpr uint32_t sll(uint8_t rd, uint8_t rt, uint8_t sa) {
  return (static_cast<uint32_t>(rt) << 16) | (static_cast<uint32_t>(rd) << 11) |
         (static_cast<uint32_t>(sa) << 6);
}
constexpr uint32_t subu(uint8_t rd, uint8_t rs, uint8_t rt) {
  return (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) |
         (static_cast<uint32_t>(rd) << 11) | 0x23u;
}
constexpr uint32_t addu(uint8_t rd, uint8_t rs, uint8_t rt) {
  return (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) |
         (static_cast<uint32_t>(rd) << 11) | 0x21u;
}
constexpr uint32_t jr(uint8_t rs) {
  return (static_cast<uint32_t>(rs) << 21) | 0x08u;
}
constexpr uint32_t kNop = 0u;

constexpr uint32_t kFuncAddr = 0x80033FBC;

// The shape at func_80033FBC+108: base 0x80034058 is the loop tail, $a2 the
// remaining count, guarded by `sltiu $at, $a2, 9`.
std::vector<uint32_t> duffFunction(uint32_t base, bool descending) {
  std::vector<uint32_t> f((0x80034210u - kFuncAddr) / 4, kNop);
  const size_t jrIdx = (0x80034028u - kFuncAddr) / 4;
  const uint16_t lo = static_cast<uint16_t>(base & 0xFFFFu);
  f[jrIdx - 5] = sltiu(1, 6, 9);            // sltiu $at, $a2, 9
  f[jrIdx - 4] = lui(8, base >> 16);        // lui   $t0, 0x8003
  f[jrIdx - 3] = addiu(8, 8, lo);           // addiu $t0, $t0, lo
  f[jrIdx - 2] = sll(1, 6, 2);              // sll   $at, $a2, 2
  f[jrIdx - 1] = descending ? subu(8, 8, 1) // subu  $t0, $t0, $at
                            : addu(8, 8, 1);
  f[jrIdx] = jr(8);
  return f;
}

} // namespace

TEST(ComputedCodeJump, DescendingDuffDeviceResolvesToInFunctionTargets) {
  auto f = duffFunction(0x80034058u, /*descending=*/true);
  auto targets =
      detectComputedCodeJump(f, (0x80034028u - kFuncAddr) / 4, kFuncAddr);

  // sltiu bound 9 -> base - 0..8 words.
  ASSERT_EQ(targets.size(), 9u);
  EXPECT_EQ(targets.front(), 0x80034058u);
  EXPECT_EQ(targets.back(), 0x80034038u);

  // The address the game actually reached with count == 4, and the one that
  // used to abort the run.
  EXPECT_NE(std::find(targets.begin(), targets.end(), 0x80034048u),
            targets.end());
}

TEST(ComputedCodeJump, AscendingFormWalksForwardFromTheBase) {
  auto f = duffFunction(0x80034058u, /*descending=*/false);
  auto targets =
      detectComputedCodeJump(f, (0x80034028u - kFuncAddr) / 4, kFuncAddr);

  ASSERT_EQ(targets.size(), 9u);
  EXPECT_EQ(targets.front(), 0x80034058u);
  EXPECT_EQ(targets.back(), 0x80034078u);
}

// The other in-function form, at func_80037D50 in the same binary: the base
// addresses a table of `bgez $zero, ...` branches, each followed by its delay
// slot, so the index is scaled by 8 rather than 4.
TEST(ComputedCodeJump, EightByteSlotTableUsesTheShiftAsStride) {
  constexpr uint32_t kFn = 0x80037D50;
  constexpr uint32_t kBase = 0x80037ED4;
  constexpr size_t kJrIdx = (0x80037E9Cu - kFn) / 4;

  std::vector<uint32_t> f((0x8003864Cu - kFn) / 4, kNop);
  f[kJrIdx - 4] = lui(1, kBase >> 16);
  f[kJrIdx - 3] = addiu(1, 1, static_cast<uint16_t>(kBase & 0xFFFFu));
  f[kJrIdx - 2] = sll(2, 2, 3); // sll $v0, $v0, 3
  f[kJrIdx - 1] = addu(1, 1, 2);
  f[kJrIdx] = jr(1);

  auto targets = detectComputedCodeJump(f, kJrIdx, kFn);

  ASSERT_FALSE(targets.empty());
  EXPECT_EQ(targets[0], kBase);
  EXPECT_EQ(targets[1], kBase + 8); // stride 8, not 4
  EXPECT_LT(targets.back(), 0x8003864Cu);
}

TEST(ComputedCodeJump, BaseOutsideTheFunctionIsRejected) {
  // A computed address pointing at another function is a call, not a local
  // branch -- resolving it to a goto would jump across function bodies.
  auto f = duffFunction(0x80034058u, /*descending=*/true);
  auto targets = detectComputedCodeJump(f, (0x80034028u - kFuncAddr) / 4,
                                        /*funcAddr=*/0x80040000u);
  EXPECT_TRUE(targets.empty());
}

TEST(ComputedCodeJump, MissingScaledIndexIsRejected) {
  // Without `sll $rs, $ridx, 2` the value is not an unrolled-loop entry and
  // the target set cannot be enumerated.
  auto f = duffFunction(0x80034058u, /*descending=*/true);
  f[(0x80034028u - kFuncAddr) / 4 - 2] = kNop; // drop the SLL
  auto targets =
      detectComputedCodeJump(f, (0x80034028u - kFuncAddr) / 4, kFuncAddr);
  EXPECT_TRUE(targets.empty());
}

TEST(ComputedCodeJump, TableShapedJumpIsLeftToTheTableDetector) {
  // `jr` whose target came out of memory has no ADDU/SUBU producing it.
  std::vector<uint32_t> f(64, kNop);
  f[32] = jr(8);
  EXPECT_TRUE(detectComputedCodeJump(f, 32, kFuncAddr).empty());
}
