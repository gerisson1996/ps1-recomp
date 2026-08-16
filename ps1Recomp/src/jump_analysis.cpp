#include <ps1recomp/jump_analysis.h>

#include <ps1recomp/mips_decoder.h>

namespace ps1recomp {

// The pattern is matched backwards from the `jr`, in the order the compiler
// emits it.  Steps 3 and 4 mirror the memory-table detector in main.cpp: trace
// the base to its LUI, and take the entry count from the SLTIU/SLTI that
// guards the index.
std::vector<uint32_t>
detectComputedCodeJump(const std::vector<uint32_t> &instrs, size_t jr_idx,
                       uint32_t funcAddr) {
  if (jr_idx == 0 || jr_idx >= instrs.size())
    return {};

  const uint32_t funcEnd = funcAddr + static_cast<uint32_t>(instrs.size() * 4);

  Instruction jr_inst = MipsDecoder::decode(instrs[jr_idx]);
  uint8_t target_reg = jr_inst.rs;

  // Step 1: the target register is produced by ADDU/SUBU, not loaded.
  int op_idx = -1;
  bool descending = false;
  uint8_t op_rs = 0, op_rt = 0;
  for (int j = (int)jr_idx - 1; j >= 0 && j >= (int)jr_idx - 8; j--) {
    Instruction inst = MipsDecoder::decode(instrs[j]);
    if ((inst.id == InstrId::ADDU || inst.id == InstrId::SUBU) &&
        inst.rd == target_reg) {
      op_idx = j;
      descending = (inst.id == InstrId::SUBU);
      op_rs = inst.rs;
      op_rt = inst.rt;
      break;
    }
  }
  if (op_idx < 0)
    return {};

  // Step 2: trace a candidate register back to its LUI (+ optional ADDIU).
  uint32_t lui_val = 0;
  int16_t addiu_imm = 0;
  auto traceToLui = [&](uint8_t reg) {
    lui_val = 0;
    addiu_imm = 0;
    for (int j = op_idx - 1; j >= 0 && j >= (int)jr_idx - 20; j--) {
      Instruction inst = MipsDecoder::decode(instrs[j]);
      if (inst.id == InstrId::LUI && inst.rt == reg) {
        lui_val = static_cast<uint32_t>(static_cast<uint16_t>(inst.imm16)) << 16;
        return true;
      }
      if ((inst.id == InstrId::ADDIU || inst.id == InstrId::ADDI) &&
          inst.rt == reg && inst.rs == reg) {
        addiu_imm = inst.imm16;
      }
    }
    return false;
  };

  // SUBU only makes sense as base - scaled, so the base must be the minuend.
  uint8_t scaled_reg = op_rt;
  if (!traceToLui(op_rs)) {
    if (descending)
      return {};
    scaled_reg = op_rs;
    if (!traceToLui(op_rt))
      return {};
  }

  uint32_t base = lui_val + static_cast<uint32_t>(addiu_imm);

  // Step 3: the base has to land inside this function -- that is what makes
  // the targets local labels rather than calls into another function.
  if (base < funcAddr || base >= funcEnd || (base & 3u) != 0)
    return {};

  // Step 4: the scaled index comes from SLL $rs, $ridx, 2; the SLTIU/SLTI
  // guarding that index carries the entry count.
  int sll_idx = -1;
  uint8_t index_reg = 0;
  for (int j = op_idx - 1; j >= 0 && j >= (int)jr_idx - 24; j--) {
    Instruction inst = MipsDecoder::decode(instrs[j]);
    if (inst.id == InstrId::SLL && inst.rd == scaled_reg && inst.shamt == 2) {
      sll_idx = j;
      index_reg = inst.rt;
      break;
    }
  }
  if (sll_idx < 0)
    return {};

  uint32_t max_entries = kJumpTableFallbackEntries;
  for (int j = sll_idx - 1; j >= 0 && j >= (int)jr_idx - 28; j--) {
    Instruction inst = MipsDecoder::decode(instrs[j]);
    if ((inst.id == InstrId::SLTIU || inst.id == InstrId::SLTI) &&
        inst.rs == index_reg) {
      uint32_t bound = static_cast<uint32_t>(static_cast<uint16_t>(inst.imm16));
      if (bound > 0 && bound <= kJumpTableMaxEntries)
        max_entries = bound;
      break;
    }
  }

  // Step 5: enumerate base -/+ i*4, clipped to the function.  Extra entries
  // are inert -- they only become reachable if the computed value matches.
  std::vector<uint32_t> targets;
  for (uint32_t i = 0; i < max_entries; i++) {
    uint32_t off = i * 4;
    if (descending && off > base - funcAddr)
      break;
    uint32_t target = descending ? base - off : base + off;
    if (target < funcAddr || target >= funcEnd)
      break;
    targets.push_back(target);
  }
  return targets;
}

} // namespace ps1recomp
