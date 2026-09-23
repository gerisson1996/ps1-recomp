#pragma once

// ps1Recomp -- Indirect jump analysis
//
// Recovers the target set of a `jr $rx` so the emitter can turn it into local
// gotos instead of a runtime dispatch.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps1recomp {

/// Entries to assume when no bounds check is found next to the index.
///
/// Measured 2026-08-30: raising this to 1024 does NOT help Crash's
/// 0x80037D50 (the missing target 0x80038030 sits *below* the detected base,
/// and enumeration is one-directional) and it regressed the boot in 1 of 3
/// runs.  The gap there is direction/base, not count -- do not raise this
/// without a measurement that shows the extra entries are the ones needed.
inline constexpr uint32_t kJumpTableFallbackEntries = 64;
/// Upper limit accepted from a bounds check, as a sanity clamp.
inline constexpr uint32_t kJumpTableMaxEntries = 4096;
/// Largest per-entry stride accepted from the scaling chain. Real ones are a
/// handful of instructions wide; anything past this is a misread pattern.
inline constexpr uint32_t kJumpTableMaxStride = 256;

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

/// Find the addresses a function loads into `$ra` as a constant pointing back
/// into itself -- a *hijacked* return address.
///
///   LUI   $ra, hi              ; $ra = address inside THIS function
///   ADDIU $ra, $ra, lo
///   ...
///   JR    $rx                  ; enters an out-of-line block
///
/// The block ends in `jr $ra`, so control resumes at that address rather than
/// returning to the caller.  Emitting the `jr $rx` as a plain dispatch-then-
/// return unwinds past the function's epilogue, so the registers it stashed on
/// entry are never restored and the caller sees the block's working values.
///
/// \param instrs    the function's instruction words
/// \param funcAddr  address of the function's first instruction
/// \returns the in-function `$ra` constants, in program order, deduplicated;
///          empty when the function never hijacks `$ra`.
std::vector<uint32_t>
detectInternalReturnTargets(const std::vector<uint32_t> &instrs,
                            uint32_t funcAddr);

} // namespace ps1recomp
