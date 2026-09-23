// ps1Analyzer -- Function Finder Implementation
// Multi-pass function detection for PS1 MIPS binaries

#include "ps1recomp/function_finder.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <fmt/format.h>

namespace ps1recomp {

namespace mips {

bool writesRegister(uint32_t instr, uint32_t reg) {
  if (reg == 0)
    return false; // $zero is never really written

  const uint32_t op = getOpcode(instr);

  if (op == OP_SPECIAL) {
    const uint32_t fn = getFunction(instr);
    // jr, jalr, mthi, mtlo, mult, multu, div, divu write no GPR rd we track
    const bool noRdWrite = (fn == 0x08 || fn == 0x09 || fn == 0x11 ||
                            fn == 0x13 || fn == 0x18 || fn == 0x19 ||
                            fn == 0x1A || fn == 0x1B);
    return !noRdWrite && getRd(instr) == reg;
  }
  if (isLoad(instr))
    return getRt(instr) == reg;
  // ADDI, ADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI -- all write rt
  if (op >= 0x08 && op <= 0x0F)
    return getRt(instr) == reg;
  return false;
}

bool isKnownInstruction(uint32_t instr) {
  const uint32_t op = getOpcode(instr);

  if (op == OP_SPECIAL) {
    switch (getFunction(instr)) {
    case 0x00: // sll
    case 0x02: // srl
    case 0x03: // sra
    case 0x04: // sllv
    case 0x06: // srlv
    case 0x07: // srav
    case 0x08: // jr
    case 0x09: // jalr
    case 0x0C: // syscall
    case 0x0D: // break
    case 0x10: // mfhi
    case 0x11: // mthi
    case 0x12: // mflo
    case 0x13: // mtlo
    case 0x18: // mult
    case 0x19: // multu
    case 0x1A: // div
    case 0x1B: // divu
    case 0x20: // add
    case 0x21: // addu
    case 0x22: // sub
    case 0x23: // subu
    case 0x24: // and
    case 0x25: // or
    case 0x26: // xor
    case 0x27: // nor
    case 0x2A: // slt
    case 0x2B: // sltu
      return true;
    default:
      return false;
    }
  }

  if (op == OP_REGIMM) {
    const uint32_t rt = getRt(instr);
    // bltz, bgez, bltzal, bgezal -- every other rt is reserved
    return rt == 0x00 || rt == 0x01 || rt == 0x10 || rt == 0x11;
  }

  // j, jal, beq, bne, blez, bgtz, addi, addiu, slti, sltiu, andi, ori,
  // xori, lui
  if (op >= 0x02 && op <= 0x0F)
    return true;

  if (op == 0x10) { // COP0
    const uint32_t rs = getRs(instr);
    return rs == 0 || rs == 4 || rs == 16; // mfc0, mtc0, CO (rfe)
  }

  if (op == 0x12) { // COP2 (GTE)
    if (((instr >> 25) & 1) != 0)
      return true; // GTE command
    const uint32_t rs = getRs(instr);
    return rs == 0 || rs == 2 || rs == 4 || rs == 6 || rs == 8;
  }

  switch (op) {
  case 0x20: // lb
  case 0x21: // lh
  case 0x22: // lwl
  case 0x23: // lw
  case 0x24: // lbu
  case 0x25: // lhu
  case 0x26: // lwr
  case 0x28: // sb
  case 0x29: // sh
  case 0x2A: // swl
  case 0x2B: // sw
  case 0x2E: // swr
  case 0x32: // lwc2
  case 0x3A: // swc2
    return true;
  default:
    return false;
  }
}

} // namespace mips

namespace {

/// Does the instruction at `idx` terminate the function?
///
/// `jr $ra` always does. For `jr $reg` we walk back to whoever wrote `$reg`:
/// a load means the target came out of memory -- a jump table or dispatch
/// through a pointer, both of which stay inside the function -- while anything
/// else (notably `move $reg, $ra`) means the register carries a return address.
bool isFunctionEnd(const std::vector<uint32_t> &words, size_t idx) {
  const uint32_t instr = words[idx];
  if (!mips::isJR(instr))
    return false;
  const uint32_t reg = mips::getRs(instr);
  if (reg == mips::REG_RA)
    return true;

  for (size_t k = idx; k-- > 0;) {
    if (!mips::writesRegister(words[k], reg))
      continue;
    return !mips::isLoad(words[k]);
  }
  // Nothing in this range wrote it, so it came in from the caller.
  return true;
}

/// Validate one candidate body without first copying the entire gap to the
/// next known entry point.
///
/// PS-X EXE exposes code + data as one synthetic .text section.  The old
/// linear sweep built a vector from every candidate address all the way to the
/// next known function before it could discover that the *first* word was
/// data.  In a large asset/table tail that is quadratic work.  This scanner
/// grows only as far as the first invalid encoding or the real return.
///
/// On success returns one-past-the-body, including the return delay slot.
std::optional<uint32_t> scanValidatedFunctionEnd(const Section &text,
                                                 uint32_t startAddr,
                                                 uint32_t maxEndAddr) {
  if (!text.data || startAddr < text.vaddr ||
      startAddr >= text.vaddr + text.size || maxEndAddr <= startAddr)
    return std::nullopt;

  const uint32_t textEnd = text.vaddr + text.size;
  maxEndAddr = std::min(maxEndAddr, textEnd);

  auto readWord = [&](uint32_t addr) {
    const uint32_t off = addr - text.vaddr;
    const uint8_t *p = text.data + off;
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
  };

  std::vector<uint32_t> words;
  words.reserve(std::min<uint32_t>((maxEndAddr - startAddr) / 4, 256u));
  uint32_t reach = startAddr;

  for (uint32_t addr = startAddr; addr + 4 <= maxEndAddr; addr += 4) {
    const uint32_t word = readWord(addr);
    if (!mips::isKnownInstruction(word))
      return std::nullopt;

    words.push_back(word);
    const size_t idx = words.size() - 1;

    if (mips::isBranch(word) || mips::isJ(word)) {
      const uint32_t target = mips::isJ(word)
                                  ? mips::jalTarget(addr, word)
                                  : mips::branchTarget(addr, word);
      if (target > reach && target > startAddr && target < maxEndAddr)
        reach = target;
    }

    if (!isFunctionEnd(words, idx) || addr < reach)
      continue;

    uint32_t end = std::min<uint32_t>(addr + 8, maxEndAddr);
    // A return's delay slot is part of the body.  Match the old
    // validate+refine path by refusing a candidate whose reachable delay word
    // is not an R3000A instruction.
    if (addr + 4 < end && !mips::isKnownInstruction(readWord(addr + 4)))
      return std::nullopt;
    return end;
  }

  return std::nullopt;
}

} // namespace

uint32_t refineFunctionEnd(const std::vector<uint32_t> &words,
                           uint32_t startAddr, uint32_t maxEndAddr) {
  if (words.empty() || maxEndAddr <= startAddr)
    return maxEndAddr;

  uint32_t reach = startAddr;

  for (size_t i = 0; i < words.size(); ++i) {
    const uint32_t addr = startAddr + static_cast<uint32_t>(i * 4);
    if (addr >= maxEndAddr)
      break;
    const uint32_t instr = words[i];

    // Track the furthest forward target: while any branch still jumps past
    // this point, the body continues regardless of terminators in between.
    if (mips::isBranch(instr) || mips::isJ(instr)) {
      const uint32_t target = mips::isJ(instr)
                                  ? mips::jalTarget(addr, instr)
                                  : mips::branchTarget(addr, instr);
      if (target > reach && target > startAddr && target < maxEndAddr)
        reach = target;
    }

    if (isFunctionEnd(words, i) && addr >= reach) {
      const uint32_t end = addr + 8; // terminator + its delay slot
      return end > maxEndAddr ? maxEndAddr : end;
    }
  }

  return maxEndAddr;
}

bool validatesAsFunction(const std::vector<uint32_t> &words, uint32_t startAddr,
                         uint32_t maxEndAddr) {
  if (words.empty() || maxEndAddr <= startAddr)
    return false;

  for (size_t i = 0; i < words.size(); ++i) {
    const uint32_t addr = startAddr + static_cast<uint32_t>(i * 4);
    if (addr >= maxEndAddr)
      break;
    if (!mips::isKnownInstruction(words[i]))
      return false;
    if (isFunctionEnd(words, i))
      return true;
  }
  return false;
}

void clampOverlappingSizes(std::vector<FunctionInfo> &funcs) {
  if (funcs.size() < 2)
    return;
  if (!std::is_sorted(funcs.begin(), funcs.end()))
    std::sort(funcs.begin(), funcs.end());

  for (size_t i = 0; i + 1 < funcs.size(); ++i) {
    if (funcs[i + 1].address <= funcs[i].address)
      continue; // duplicate address -- not this function's problem
    const uint32_t room = funcs[i + 1].address - funcs[i].address;
    if (funcs[i].size > room)
      funcs[i].size = room;
  }
}

// Main Entry Point

void FunctionFinder::findFunctions(const ElfParser& elf) {
    m_functions.clear();
    m_jalTargets.clear();
    m_jumpIslands.clear();

    // Pass 1: Entry point
    addEntryPoint(elf);

    // Pass 2: ELF symbol table
    addSymbolFunctions(elf);

    // Pass 3, 4 & 5: Heuristic scans on .text section
    const Section* text = elf.getTextSection();
    if (text != nullptr && text->data != nullptr && text->size >= 4) {
        scanJALTargets(*text);
        scanPrologues(*text);
        scanJumpArrays(*text);
        linearSweep(*text);
    }

    // Pass 7: Compute sizes from sorted addresses
    if (text != nullptr) {
        computeBoundaries(*text);
    }
}

// Pass 1: Entry Point

void FunctionFinder::addEntryPoint(const ElfParser& elf) {
    addFunction(elf.getEntryPoint(), "__start", FunctionSource::EntryPoint);
}

// Pass 2: ELF Symbols

void FunctionFinder::addSymbolFunctions(const ElfParser& elf) {
    for (const auto& sym : elf.getSymbols()) {
        if (sym.isFunction() && sym.address != 0) {
            addFunction(sym.address, sym.name, FunctionSource::Symbol);
        }
    }
}

// Pass 3: JAL Target Scan

void FunctionFinder::scanJALTargets(const Section& text) {
    const uint32_t numInstructions = text.size / 4;

    // PS-X EXE has no section table: its whole payload is exposed as .text,
    // even though real games freely mix code, lookup tables and assets in that
    // range. A data word whose top six bits happen to be 0x03 looks exactly
    // like JAL. Do not turn that coincidence into a function unless the target
    // itself decodes as a self-contained R3000A function.
    //
    // 64 KiB is deliberately only a validation horizon, not a function-size
    // limit in the generated config. It prevents a bogus target in a large
    // zero/data tail from making this pass walk the rest of a multi-megabyte
    // executable for every candidate.
    constexpr uint32_t kJalValidationBytes = 0x10000;
    std::unordered_map<uint32_t, bool> jalTargetValidationCache;

    auto targetLooksLikeFunction = [&](uint32_t target) {
        auto cached = jalTargetValidationCache.find(target);
        if (cached != jalTargetValidationCache.end())
            return cached->second;
        if (!text.containsAddress(target) || target < text.vaddr)
            return false;

        const uint32_t sectionEnd = text.vaddr + text.size;
        const uint32_t hardEnd =
            std::min(sectionEnd, target + kJalValidationBytes);
        const bool valid =
            hardEnd > target &&
            scanValidatedFunctionEnd(text, target, hardEnd).has_value();
        jalTargetValidationCache[target] = valid;
        return valid;
    };

    auto isDirectJump = [](uint32_t word) {
        const uint32_t op = mips::getOpcode(word);
        return op == mips::OP_J || op == mips::OP_JAL;
    };

    for (uint32_t i = 0; i < numInstructions; ++i) {
        uint32_t instr = readInstruction(text, i * 4);
        uint32_t pc = text.vaddr + (i * 4);

        if (mips::isJAL(instr)) {
            // A direct jump/call cannot legally occupy another direct
            // jump/call's delay slot. Long runs of words with opcodes 0x02/0x03
            // are therefore data tables, not executable MIPS. Real KERNEL.BIN
            // contains exactly this shape and otherwise produces hundreds of
            // fake JAL targets.
            const bool prevDirect =
                i > 0 && isDirectJump(readInstruction(text, (i - 1) * 4));
            const bool nextDirect =
                i + 1 < numInstructions &&
                isDirectJump(readInstruction(text, (i + 1) * 4));
            if (prevDirect || nextDirect)
                continue;

            uint32_t target = mips::jalTarget(pc, instr);

            if (text.containsAddress(target) && targetLooksLikeFunction(target)) {
                m_jalTargets.insert(target);

                if (!hasFunction(target)) {
                    std::string name = fmt::format("func_{:08X}", target);
                    addFunction(target, name, FunctionSource::JALTarget);
                }
            }
        }
    }
}

// Pass 4: Prologue Pattern Scan

void FunctionFinder::scanPrologues(const Section& text) {
    const uint32_t numInstructions = text.size / 4;

    for (uint32_t i = 0; i < numInstructions; ++i) {
        uint32_t instr = readInstruction(text, i * 4);
        uint32_t addr = text.vaddr + (i * 4);

        if (mips::isStackPrologue(instr) && !hasFunction(addr)) {
            // Additional validation: check if preceded by JR $ra + delay slot
            // or by NOP padding (common between functions)
            bool likelyStart = false;

            if (i == 0) {
                // First instruction in section -- likely function start
                likelyStart = true;
            } else if (i >= 2) {
                // Check if the instruction 2 slots back is JR $ra
                // (the instruction right before is the delay slot)
                uint32_t prevInstr = readInstruction(text, (i - 2) * 4);
                if (mips::isJR_RA(prevInstr)) {
                    likelyStart = true;
                }
            }

            if (i >= 1) {
                // Check if previous instruction is a NOP (padding)
                uint32_t prevInstr = readInstruction(text, (i - 1) * 4);
                if (mips::isNOP(prevInstr)) {
                    likelyStart = true;
                }
            }

            if (likelyStart) {
                std::string name = fmt::format("func_{:08X}", addr);
                addFunction(addr, name, FunctionSource::Prologue);
            }
        }
    }
}

// Pass 5: Computed Jump Arrays

/// How far back the operands of the computed jump are traced.
static constexpr uint32_t kJumpArrayTraceWindow = 16;
/// A slot has to hold at least `jr $ra` and its delay slot to be a function.
static constexpr uint32_t kJumpArrayMinSlot = 8;
/// Above this a "slot size" is far likelier to be a mis-traced shift.
static constexpr uint32_t kJumpArrayMaxSlot = 1024;

/// Register the slots of an array of bodies reached by a computed jump.
///
/// A switch does not always dispatch through a table of pointers. The other
/// shape -- the one Crash's memcpy and its decompressor use -- computes the
/// target address arithmetically:
///
///     lui   $t, hi(base)
///     addiu $t, $t, lo(base)    ; base is a .text address, not a table
///     sll   $i, $idx, k         ; scale the index by the slot size
///     addu  $t, $t, $i
///     jr    $t
///
/// There is no table anywhere to read: the target is `base + idx * 2^k`, and
/// each slot is a body of its own. Nothing else in the binary points at those
/// bodies -- no `jal`, no stack prologue -- so without this pass the function
/// ahead of the array simply extends over all of them, and every branch that
/// lands in one becomes a dispatch to an address nobody emitted.
///
/// What separates a slot that is a *function* from a slot that is a *label* is
/// whether it returns on its own. The same computed jump also builds jump
/// islands (`bgez $zero, far_label` + delay slot in each slot), and those
/// belong to the function around them. Requiring `jr $ra` inside the slot
/// keeps the islands out.
void FunctionFinder::scanJumpArrays(const Section& text) {
    const uint32_t numInstructions = text.size / 4;
    if (numInstructions < 4) {
        return;
    }

    auto wordAt = [&](uint32_t addr) {
        return readInstruction(text, addr - text.vaddr);
    };

    // `reg` holds a LUI (+ ADDIU) constant at instruction `from`, walking back.
    // Any other write to it first means the value is not a link-time constant.
    auto traceConstant = [&](uint32_t reg, uint32_t from, uint32_t& out) {
        int32_t addend = 0;
        const uint32_t stop =
            (from >= kJumpArrayTraceWindow) ? from - kJumpArrayTraceWindow : 0;
        for (uint32_t j = from + 1; j-- > stop;) {
            const uint32_t instr = readInstruction(text, j * 4);
            if (!mips::writesRegister(instr, reg)) {
                continue;
            }
            const uint32_t op = mips::getOpcode(instr);
            if (op == mips::OP_LUI) {
                out = ((instr & 0xFFFFu) << 16) + static_cast<uint32_t>(addend);
                return true;
            }
            if ((op == mips::OP_ADDIU || op == mips::OP_ADDI) &&
                mips::getRs(instr) == reg) {
                addend = mips::getImm16(instr);
                continue;
            }
            return false;
        }
        return false;
    };

    // `reg` is an index scaled by a left shift; the shift amount is the slot
    // size, which is the whole point of the pattern.
    auto traceShift = [&](uint32_t reg, uint32_t from, uint32_t& shamt) {
        const uint32_t stop =
            (from >= kJumpArrayTraceWindow) ? from - kJumpArrayTraceWindow : 0;
        for (uint32_t j = from + 1; j-- > stop;) {
            const uint32_t instr = readInstruction(text, j * 4);
            if (!mips::writesRegister(instr, reg)) {
                continue;
            }
            if (mips::getOpcode(instr) != mips::OP_SPECIAL ||
                mips::getFunction(instr) != mips::FUNC_SLL) {
                return false;
            }
            shamt = mips::getShamt(instr);
            return shamt > 0;
        }
        return false;
    };

    auto returnsWithin = [&](uint32_t addr, uint32_t slot) {
        for (uint32_t off = 0; off + 4 <= slot; off += 4) {
            if (!text.containsAddress(addr + off)) {
                return false;
            }
            if (mips::isJR_RA(wordAt(addr + off))) {
                return true;
            }
        }
        return false;
    };

    const uint32_t textEnd = text.vaddr + text.size;

    for (uint32_t i = 1; i < numInstructions; ++i) {
        const uint32_t jr = readInstruction(text, i * 4);
        if (!mips::isJR(jr) || mips::getRs(jr) == mips::REG_RA) {
            continue;
        }

        // ADDU $target, $base, $scaled -- either operand may be the base.
        const uint32_t targetReg = mips::getRs(jr);
        const uint32_t stop =
            (i >= kJumpArrayTraceWindow) ? i - kJumpArrayTraceWindow : 0;
        uint32_t base = 0;
        uint32_t shamt = 0;
        bool matched = false;
        for (uint32_t j = i; j-- > stop;) {
            const uint32_t instr = readInstruction(text, j * 4);
            if (!mips::writesRegister(instr, targetReg)) {
                continue;
            }
            if (j == 0 || mips::getOpcode(instr) != mips::OP_SPECIAL ||
                mips::getFunction(instr) != mips::FUNC_ADDU) {
                break;
            }
            const uint32_t lhs = mips::getRs(instr);
            const uint32_t rhs = mips::getRt(instr);
            matched =
                (traceConstant(lhs, j - 1, base) && traceShift(rhs, j - 1, shamt)) ||
                (traceConstant(rhs, j - 1, base) && traceShift(lhs, j - 1, shamt));
            break;
        }
        if (!matched) {
            continue;
        }

        const uint32_t slot = 1u << shamt;
        if (slot < kJumpArrayMinSlot || slot > kJumpArrayMaxSlot ||
            !text.containsAddress(base)) {
            continue;
        }
        // Never run past an entry point another pass already found: that one
        // is better evidence than this arithmetic.
        uint32_t limit = textEnd;
        for (const auto& f : m_functions) {
            if (f.address > base && f.address < limit) {
                limit = f.address;
            }
        }

        if (!returnsWithin(base, slot)) {
            // The array is real -- the arithmetic traced -- but its slots do
            // not return, so they are jump islands rather than functions.
            // Remember that verdict with the extent it actually covers: the
            // linear sweep would otherwise reach the same slots with far
            // weaker evidence and claim them.
            uint32_t end = base;
            while (end < limit && text.containsAddress(end) &&
                   mips::hasDelaySlot(wordAt(end)) && !returnsWithin(end, slot)) {
                end += slot;
            }
            if (end > base) {
                m_jumpIslands.push_back({base, end, slot});
            }
            continue;
        }

        for (uint32_t n = 0;; ++n) {
            const uint32_t addr = base + n * slot;
            if (addr >= limit || !text.containsAddress(addr)) {
                break;
            }
            if (n > 0) {
                // The array ends where a slot stops returning, and a slot that
                // begins on a delay slot is the tail of the one before it.
                if (!returnsWithin(addr - slot, slot) ||
                    mips::hasDelaySlot(wordAt(addr - 4))) {
                    break;
                }
            }
            addFunction(addr, fmt::format("func_{:08X}", addr),
                        FunctionSource::JumpArray);
        }
    }
}

// Pass 6: Linear Sweep

/// Is `addr` a slot of a jump-island array `scanJumpArrays` traced and refused?
///
/// Both conditions have to hold: the address sits exactly on the stride of a
/// traced dispatch base, and it opens with a control transfer, which is what an
/// island is made of. Real code that merely happens to land on the stride keeps
/// its chance at being a function.
bool FunctionFinder::isJumpIslandSlot(uint32_t addr, uint32_t word) const {
    if (!mips::hasDelaySlot(word)) {
        return false;
    }
    for (const auto& island : m_jumpIslands) {
        if (addr >= island.base && addr < island.end &&
            ((addr - island.base) % island.slot) == 0) {
            return true;
        }
    }
    return false;
}

/// Walk the bytes no other pass claimed and recover the functions hiding there.
///
/// Every pass before this one needs a *reference* to the function: a symbol, a
/// `jal`, a stack prologue, an address in the config. A function that is only
/// ever reached by `jalr` through a pointer -- a table of handlers, a callback
/// installed at runtime, the tail of a dispatch family -- has none of those, so
/// nothing points at it and it disappears: the function before it simply
/// extends over it, and every branch that lands inside becomes a dispatch to an
/// address nobody emitted.
///
/// The sweep finds them by asking a different question. Instead of "does
/// something point here?", it asks "does the code here stand on its own?".
/// Where the previous function has genuinely ended (`refineFunctionEnd`, not
/// the gap to the next entry point) and the bytes that follow validate as a
/// body, that is a function.
///
/// This is the one pass that can invent functions, because it is the one pass
/// with no external evidence -- so `validatesAsFunction` does the whole job:
/// unknown encoding anywhere, or no terminator before the next known entry, and
/// the candidate is dropped and the sweep steps one word forward. Both guards
/// have to hold; either one alone lets a jump table through, since jump table
/// words decode as plausible loads.
void FunctionFinder::linearSweep(const Section& text) {
    const uint32_t textStart = text.vaddr;
    const uint32_t textEnd = text.vaddr + text.size;
    if (text.size < 8) {
        return;
    }

    auto wordAt = [&](uint32_t addr) {
        return readInstruction(text, addr - textStart);
    };
    auto slice = [&](uint32_t from, uint32_t to) {
        std::vector<uint32_t> words;
        words.reserve((to - from) / 4);
        for (uint32_t a = from; a < to; a += 4) {
            words.push_back(wordAt(a));
        }
        return words;
    };

    std::set<uint32_t> starts;
    for (const auto& f : m_functions) {
        if (f.address >= textStart && f.address < textEnd) {
            starts.insert(f.address);
        }
    }

    // Where the already-detected functions really end. Their *sizes* are still
    // the gap to the next entry point, which is exactly the over-extension the
    // sweep is here to undo, so the extents have to be refined first.
    std::vector<std::pair<uint32_t, uint32_t>> claimed;
    claimed.reserve(starts.size());
    for (auto it = starts.begin(); it != starts.end(); ++it) {
        auto next = std::next(it);
        const uint32_t bound = (next == starts.end()) ? textEnd : *next;
        claimed.emplace_back(*it, refineFunctionEnd(slice(*it, bound), *it, bound));
    }

    size_t ci = 0;
    uint32_t addr = textStart;
    while (addr + 8 <= textEnd) {
        while (ci < claimed.size() && claimed[ci].second <= addr) {
            ++ci;
        }
        if (ci < claimed.size() && claimed[ci].first <= addr) {
            addr = std::max(addr + 4, claimed[ci].second);
            continue;
        }

        const uint32_t first = wordAt(addr);

        // Inter-function padding, and never a body's first instruction.
        if (mips::isNOP(first)) {
            addr += 4;
            continue;
        }

        // A slot of a computed-jump array that `scanJumpArrays` already looked
        // at and refused, because it never returns: a jump island, which
        // belongs to the function around it. `validatesAsFunction` cannot tell
        // the two apart on its own -- it follows the island's branch straight
        // into that function's `jr $ra` and accepts. The earlier pass traced
        // the dispatch arithmetic and is the better evidence.
        if (isJumpIslandSlot(addr, first)) {
            addr += 4;
            continue;
        }

        uint32_t bound = textEnd;
        const auto next = starts.upper_bound(addr);
        if (next != starts.end()) {
            bound = *next;
        }

        // A real PS1 function is never expected to need an unbounded scan
        // through the rest of a mixed code/data PS-X EXE. Keep the probe
        // finite so a long table made entirely of valid-looking opcodes cannot
        // turn the linear sweep quadratic.
        constexpr uint32_t kLinearSweepValidationBytes = 0x10000;
        const uint32_t probeBound =
            std::min(bound, addr + kLinearSweepValidationBytes);
        const auto endOpt = scanValidatedFunctionEnd(text, addr, probeBound);
        if (!endOpt.has_value()) {
            addr += 4;
            continue;
        }
        const uint32_t end = *endOpt;

        addFunction(addr, fmt::format("func_{:08X}", addr),
                    FunctionSource::LinearSweep);

        // The sweep just proved the exact extent of this body. Keep it.
        // PS-X EXE exposes the entire payload as one .text section, so using
        // "distance to the next detected entry" later can append kilobytes of
        // lookup tables, strings or assets to a valid function. That made the
        // recompiler emit INVALID pseudo-instructions from game data.
        for (auto &f : m_functions) {
            if (f.address == addr && f.source == FunctionSource::LinearSweep &&
                f.size == 0) {
                f.size = end - addr;
                break;
            }
        }

        starts.insert(addr);
        claimed.insert(claimed.begin() + static_cast<long>(ci), {addr, end});
        addr = end;
    }
}

// Pass 7: Compute Boundaries

void FunctionFinder::computeBoundaries(const Section& text) {
    // Sort functions by address
    std::sort(m_functions.begin(), m_functions.end());

    // Compute sizes: each function extends to the start of the next
    for (size_t i = 0; i < m_functions.size(); ++i) {
        if (m_functions[i].size != 0) {
            continue; // Already has size from symbol table
        }

        if (i + 1 < m_functions.size()) {
            m_functions[i].size = m_functions[i + 1].address - m_functions[i].address;
        } else {
            // Last function: extends to end of text section
            uint32_t textEnd = text.vaddr + text.size;
            if (m_functions[i].address < textEnd) {
                m_functions[i].size = textEnd - m_functions[i].address;
            }
        }
    }

    // No function may reach into the next one. Sizes set by earlier passes
    // (ELF symbols) and by `--add-func` after the fact never revisit each
    // other, so a containing function keeps its old extent and the same
    // bytes get emitted twice, under two names.
    clampOverlappingSizes(m_functions);

    // Determine leaf functions: scan each function for JAL/JALR instructions
    for (auto& func : m_functions) {
        if (func.size == 0 || !text.containsAddress(func.address)) {
            continue;
        }

        func.isLeaf = true;
        uint32_t offset = func.address - text.vaddr;
        uint32_t numInstr = func.size / 4;

        for (uint32_t i = 0; i < numInstr; ++i) {
            uint32_t instr = readInstruction(text, offset + i * 4);
            uint32_t opcode = mips::getOpcode(instr);

            if (opcode == mips::OP_JAL) {
                func.isLeaf = false;
                break;
            }
            if (opcode == mips::OP_SPECIAL && mips::getFunction(instr) == mips::FUNC_JALR) {
                func.isLeaf = false;
                break;
            }
        }
    }
}

// Queries

const FunctionInfo* FunctionFinder::findByAddress(uint32_t addr) const {
    // Binary search (m_functions is sorted)
    auto it = std::lower_bound(m_functions.begin(), m_functions.end(), addr,
        [](const FunctionInfo& f, uint32_t a) { return f.address < a; });

    if (it != m_functions.end() && it->address == addr) {
        return &(*it);
    }
    return nullptr;
}

const FunctionInfo* FunctionFinder::findContaining(uint32_t addr) const {
    if (m_functions.empty()) return nullptr;

    // Find last function with address <= addr
    auto it = std::upper_bound(m_functions.begin(), m_functions.end(), addr,
        [](uint32_t a, const FunctionInfo& f) { return a < f.address; });

    if (it == m_functions.begin()) return nullptr;
    --it;

    // Check if addr is within this function's range
    if (it->size > 0 && addr < it->address + it->size) {
        return &(*it);
    }
    // If size unknown, assume it could contain it
    if (it->size == 0) {
        return &(*it);
    }
    return nullptr;
}

// Helpers

void FunctionFinder::addFunction(uint32_t addr, const std::string& name, FunctionSource source) {
    // Don't add if already exists (prefer earlier source -- higher priority)
    if (hasFunction(addr)) {
        return;
    }

    FunctionInfo info;
    info.address = addr;
    info.size = 0;      // Computed later in computeBoundaries
    info.name = name;
    info.source = source;
    info.isLeaf = false; // Determined later

    m_functions.push_back(std::move(info));
}

void FunctionFinder::recomputeBoundaries(const ElfParser& elf) {
    const Section* text = elf.getTextSection();
    if (text != nullptr) {
        computeBoundaries(*text);
    }
}

bool FunctionFinder::hasFunction(uint32_t addr) const {
    return std::any_of(m_functions.begin(), m_functions.end(),
        [addr](const FunctionInfo& f) { return f.address == addr; });
}

uint32_t FunctionFinder::readInstruction(const Section& sec, uint32_t offset) {
    if (offset + 4 > sec.size || sec.data == nullptr) {
        return 0;
    }
    // Little-endian read (PS1 is MIPS LE)
    const uint8_t* p = sec.data + offset;
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace ps1recomp
