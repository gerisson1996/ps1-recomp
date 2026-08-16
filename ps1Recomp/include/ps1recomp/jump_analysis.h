#pragma once

// ps1Recomp -- Indirect jump analysis
//
// Recovers the target set of a `jr $rx` so the emitter can turn it into local
// gotos instead of a runtime dispatch.

#include <cstdint>
#include <vector>

namespace ps1recomp {

/// Entries to assume when no bounds check is found next to the index.
inline constexpr uint32_t kJumpTableFallbackEntries = 64;
/// Upper limit accepted from a bounds check, as a sanity clamp.
inline constexpr uint32_t kJumpTableMaxEntries = 4096;

/// Resolve a `jr` whose target is *computed* from a code address inside the
/// same function, rather than loaded from a table (Duff's device).
///
/// An unrolled copy loop is entered partway through: the caller scales the
/// remaining count and subtracts it from the address of the loop's tail, then
/// jumps there.  Nothing is read from memory, so the table detector never
/// matches and the `jr` would fall through to a dispatch on a mid-function
/// address -- which is not a known function, and aborts.
///
///   LUI   $rb, hi              ; $rb = address inside THIS function
///   ADDIU $rb, $rb, lo
///   SLL   $rs, $ridx, N        ; N=2 single instructions, N=3 8-byte slots
///   SUBU  $rt, $rb, $rs        ; ADDU when the loop is entered ascending
///   JR    $rt
///
/// Shift 3 covers the other in-function form: a table of `bgez $zero, ...`
/// branches, each with its delay slot, jumped into by index.
///
/// \param instrs    the function's instruction words
/// \param jr_idx    index of the `jr` in \p instrs
/// \param funcAddr  address of the function's first instruction
/// \returns target addresses `base -/+ i*stride`, clipped to the function;
///          empty when the pattern does not match.
std::vector<uint32_t>
detectComputedCodeJump(const std::vector<uint32_t> &instrs, size_t jr_idx,
                       uint32_t funcAddr);

} // namespace ps1recomp
