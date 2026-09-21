#pragma once
// ps1Analyzer -- names the runtime's PsyQ HLE registry actually implements.
//
// Hash detection tells us a function *is* a known PsyQ routine. That is not
// the same as deciding to replace it: replacing means our hand-written C++
// stands in for the real semantics, and every such substitution is a place the
// two can silently diverge. So detection is broad and replacement is narrow --
// a function is only marked `hle = true` in the generated config when the name
// below exists, and everything else is recompiled from its own MIPS.
//
// Kept in sync with `psyq_register()` in ps1Runtime/src/psyq/. The e2e test
// `PsyqHleAllowList.MatchesRuntimeRegistry` fails if the two drift.

#include <string>

namespace ps1recomp {

/// True when the PsyQ HLE registry implements `name` (`<library>_<basename>`).
bool psyqHleIsImplemented(const std::string& name);

} // namespace ps1recomp
