// Tests for the generated `recomp_dispatch` body.
//
// The behaviour under test is a policy decision, not a formatting detail: an
// address that reaches the end of the resolver is code the recompiler never
// emitted. The original generator logged it five times and kept running, which
// made every miss a silent no-op -- execution continued with corrupted state
// and the symptom surfaced far from its cause. Measured consequence on Crash
// Bandicoot: months spent chasing render symptoms whose real cause was a
// function split in two by the analyzer, whose intra-function branch became a
// dispatch to an address that was never emitted, and was then swallowed.

#include "ps1recomp/dispatch_emitter.h"
#include <gtest/gtest.h>
#include <string>

using ps1recomp::emitDispatchBody;

namespace {

// Position of `needle` in the emitted body, or std::string::npos.
size_t at(const std::string &body, const char *needle) {
  return body.find(needle);
}

TEST(DispatchEmitter, AbortsOnUnmappedTarget) {
  const std::string body = emitDispatchBody();
  EXPECT_NE(at(body, "std::abort();"), std::string::npos)
      << "an unmapped target must stop the run, not be logged and skipped";
}

TEST(DispatchEmitter, FatalMessageNamesAddressCallerAndCause) {
  const std::string body = emitDispatchBody();
  // The whole point of failing here is that the message is actionable: the
  // address to emit, and the caller that reached it.
  EXPECT_NE(at(body, "unmapped call to 0x%08X"), std::string::npos);
  EXPECT_NE(at(body, "RA=0x%08X"), std::string::npos);
  EXPECT_NE(at(body, "never emitted by the recompiler"), std::string::npos);
}

TEST(DispatchEmitter, AbortIsReachedBeforeTheRateLimitedLogging) {
  const std::string body = emitDispatchBody();
  const size_t abortAt = at(body, "std::abort();");
  const size_t logAt = at(body, "s_unknownHitCount");
  ASSERT_NE(abortAt, std::string::npos);
  ASSERT_NE(logAt, std::string::npos);
  EXPECT_LT(abortAt, logAt)
      << "the permissive log must sit after the abort, so it only runs when "
         "the escape hatch was taken";
}

TEST(DispatchEmitter, PermissiveEscapeHatchIsEnvGated) {
  const std::string body = emitDispatchBody();
  EXPECT_NE(at(body, "PS1_DISPATCH_PERMISSIVE"), std::string::npos)
      << "there must be a documented way to run a knowingly-incomplete build";
  // Guard against the hatch being open by default: the abort is skipped only
  // when the variable is set to something other than empty or "0".
  EXPECT_NE(at(body, "std::getenv(\"PS1_DISPATCH_PERMISSIVE\")"),
            std::string::npos);
  EXPECT_NE(at(body, "if (!s_permissive)"), std::string::npos);
}

// The resolver has six legitimate paths before the failure. Losing any of them
// would turn a resolvable call into a spurious abort, which is worse than the
// silent no-op it replaced -- so pin the order.
TEST(DispatchEmitter, ResolutionPathsPrecedeTheFailure) {
  const std::string body = emitDispatchBody();
  const size_t nullGuard = at(body, "1. NULL pointer guard");
  const size_t direct = at(body, "2. Direct lookup");
  const size_t normalize = at(body, "3. Address normalization");
  const size_t biosEntry = at(body, "4. BIOS entry points");
  const size_t sentinel = at(body, "5. BIOS table sentinel");
  const size_t trampoline = at(body, "6. JR RA trampoline");
  const size_t fatal = at(body, "7. Unmapped target");

  for (size_t pos : {nullGuard, direct, normalize, biosEntry, sentinel,
                     trampoline, fatal})
    ASSERT_NE(pos, std::string::npos) << "a resolution path went missing";

  EXPECT_LT(nullGuard, direct);
  EXPECT_LT(direct, normalize);
  EXPECT_LT(normalize, biosEntry);
  EXPECT_LT(biosEntry, sentinel);
  EXPECT_LT(sentinel, trampoline);
  EXPECT_LT(trampoline, fatal);
}

TEST(DispatchEmitter, NullDispatchStillReturnsQuietly) {
  // Null targets are a known benign startup transient (uninitialised callback
  // pointers), so they must keep returning rather than abort.
  const std::string body = emitDispatchBody();
  const size_t guard = at(body, "if (addr == 0)");
  const size_t fatal = at(body, "std::abort();");
  ASSERT_NE(guard, std::string::npos);
  ASSERT_NE(fatal, std::string::npos);
  EXPECT_LT(guard, fatal);
}

} // namespace

// The site address is the only clue that survives every path.  `RA` holds the
// return address of the last direct call, so it names an unrelated function
// whenever the bad jump came through one, and the host stack names the
// emitted function but not which of its indirect sites fired -- Crash's GOOL
// interpreter alone has seven.
TEST(DispatchEmitter, FatalMessageNamesTheIndirectSite) {
  const std::string body = emitDispatchBody();
  EXPECT_NE(at(body, "ps1LastIndirectSite()"), std::string::npos);
  EXPECT_NE(at(body, "issued from guest site 0x%08X"), std::string::npos);
}

TEST(DispatchEmitter, FatalPathPrintsAHostStack) {
  const std::string body = emitDispatchBody();
  EXPECT_NE(at(body, "backtrace_symbols_fd"), std::string::npos);
  const size_t stackAt = at(body, "backtrace_symbols_fd");
  const size_t abortAt = at(body, "std::abort();");
  ASSERT_NE(abortAt, std::string::npos);
  EXPECT_LT(stackAt, abortAt) << "the stack has to be printed before aborting";
}
