// ps1Analyzer -- Function Finder Implementation
// Multi-pass function detection for PS1 MIPS binaries

#include "ps1recomp/function_finder.h"

#include <algorithm>
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
    }

    // Pass 6: Compute sizes from sorted addresses
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

    for (uint32_t i = 0; i < numInstructions; ++i) {
        uint32_t instr = readInstruction(text, i * 4);
        uint32_t pc = text.vaddr + (i * 4);

        if (mips::isJAL(instr)) {
            uint32_t target = mips::jalTarget(pc, instr);

            // Only accept targets within the text section
            if (text.containsAddress(target)) {
                m_jalTargets.insert(target);

                if (!hasFunction(target)) {
                    // Generate name based on address
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
            !text.containsAddress(base) || !returnsWithin(base, slot)) {
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

// Pass 6: Compute Boundaries

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
