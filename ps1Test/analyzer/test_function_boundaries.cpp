// Tests for function-end refinement.
//
// The rule this replaces was "a function ends where the next detected one
// begins". That is wrong whenever detection produces a false entry point
// inside a real function: the body gets cut, and the branch spanning the cut
// is emitted as a call to an address the recompiler never emits. Measured on
// Crash Bandicoot (SCUS-94900): 0x8003A264 is a label inside func_8003A144,
// reached by two forward branches; we cut there and dispatched to nothing.

#include "ps1recomp/function_finder.h"
#include <gtest/gtest.h>
#include <vector>

using namespace ps1recomp;

namespace {

// Instruction builders -- just enough to express the shapes under test.
constexpr uint32_t kNop = 0x00000000u;
constexpr uint32_t kJrRa = 0x03E00008u; // jr $ra

// jr $reg
uint32_t jr(uint32_t reg) { return (reg << 21) | 0x08u; }

// lw $rt, imm($rs) -- opcode 0x23
uint32_t lw(uint32_t rt, uint32_t rs, int16_t imm) {
  return (0x23u << 26) | (rs << 21) | (rt << 16) |
         static_cast<uint16_t>(imm);
}

// addu $rd, $rs, $rt -- SPECIAL fn 0x21
uint32_t addu(uint32_t rd, uint32_t rs, uint32_t rt) {
  return (rs << 21) | (rt << 16) | (rd << 11) | 0x21u;
}

// move $rd, $ra  ==  addu $rd, $ra, $zero
uint32_t moveFromRa(uint32_t rd) { return addu(rd, 31, 0); }

// beq $rs, $rt, offset(instructions forward from the delay slot)
uint32_t beq(uint32_t rs, uint32_t rt, int16_t words) {
  return (0x04u << 26) | (rs << 21) | (rt << 16) |
         static_cast<uint16_t>(words);
}

constexpr uint32_t kBase = 0x80010000u;

TEST(RefineFunctionEnd, EndsAtPlainReturn) {
  // index 0: nop, 1: jr $ra, 2: nop(delay) -> body is [base, base+12)
  const std::vector<uint32_t> w{kNop, kJrRa, kNop, kNop, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 20), kBase + 12);
}

TEST(RefineFunctionEnd, ForwardBranchOverTheReturnKeepsTheBodyTogether) {
  // This is the shape that broke Crash. A branch jumps past an early `jr $ra`;
  // the function continues to the *second* return, and everything in between
  // belongs to one body.
  //
  //  0: beq $0,$0,+4   -> targets index 6
  //  1: nop (delay)
  //  2: jr $ra          <- must NOT end the function: index 6 is still ahead
  //  3: nop
  //  4: nop
  //  5: nop
  //  6: jr $ra          <- the real end
  //  7: nop (delay)
  const std::vector<uint32_t> w{beq(0, 0, 4), kNop, kJrRa, kNop,
                                kNop,         kNop, kJrRa, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 32), kBase + 32)
      << "an early return before a live forward branch must not cut the body";
}

TEST(RefineFunctionEnd, ReturnAtOrPastReachDoesEndTheFunction) {
  // Same shape, but the branch target lands exactly on the terminator, so the
  // terminator is legitimately the end.
  //
  //  0: beq $0,$0,+2 -> index 4
  //  1: nop (delay)
  //  2: nop
  //  3: nop
  //  4: jr $ra
  //  5: nop (delay)
  //  6..7: another function's padding
  const std::vector<uint32_t> w{beq(0, 0, 2), kNop, kNop, kNop,
                                kJrRa,        kNop, kNop, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 32), kBase + 24);
}

TEST(RefineFunctionEnd, JumpTableJrDoesNotEndTheFunction) {
  // `jr $t0` where $t0 came out of memory is a jump table or a call through a
  // pointer -- the targets are inside this function, so it continues.
  //
  //  0: lw $t0, 0($t1)
  //  1: jr $t0        <- not a return
  //  2: nop (delay)
  //  3: nop
  //  4: jr $ra        <- the real end
  //  5: nop (delay)
  const std::vector<uint32_t> w{lw(8, 9, 0), jr(8), kNop, kNop, kJrRa, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 24), kBase + 24);
}

TEST(RefineFunctionEnd, JrThroughAnRaAliasIsAReturn) {
  // `move $t0, $ra; ...; jr $t0` is a return with the address parked in another
  // register -- it must end the function, unlike the jump-table case above.
  const std::vector<uint32_t> w{moveFromRa(8), kNop, jr(8), kNop, kNop, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 24), kBase + 16);
}

TEST(RefineFunctionEnd, NeverRunsPastTheHardBound) {
  // No terminator at all: the caller's bound wins, and nothing beyond it is
  // ever claimed.
  const std::vector<uint32_t> w{kNop, kNop, kNop, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 8), kBase + 8);
}

TEST(RefineFunctionEnd, TerminatorNearTheBoundIsClampedNotOverrun) {
  // `jr $ra` in the last slot would put end at +8 past it; the bound clamps it.
  const std::vector<uint32_t> w{kNop, kJrRa};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 8), kBase + 8);
}

TEST(RefineFunctionEnd, BackwardBranchDoesNotExtendTheBody) {
  // A loop branching backwards must not push `reach` forward and swallow the
  // following function.
  //
  //  0: nop
  //  1: beq $0,$0,-2  -> back to index 0
  //  2: nop (delay)
  //  3: jr $ra
  //  4: nop (delay)
  const std::vector<uint32_t> w{kNop, beq(0, 0, -2), kNop, kJrRa, kNop, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 24), kBase + 20);
}

TEST(WritesRegister, ClassifiesTheFormsBoundaryDetectionRelieson) {
  EXPECT_TRUE(mips::writesRegister(lw(8, 9, 0), 8));
  EXPECT_FALSE(mips::writesRegister(lw(8, 9, 0), 9)) << "rs is the base, not a destination";
  EXPECT_TRUE(mips::writesRegister(addu(8, 9, 10), 8));
  EXPECT_FALSE(mips::writesRegister(jr(8), 8)) << "jr writes nothing";
  EXPECT_FALSE(mips::writesRegister(addu(0, 9, 10), 0)) << "$zero is never written";
}

} // namespace
