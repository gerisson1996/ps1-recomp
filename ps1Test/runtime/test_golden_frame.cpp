#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

// Tests run from build/ps1Test/, so the repository root is two levels up.
// The pre-existing pipeline tests use "../" and consequently skip forever --
// do not copy that.
constexpr const char *kRepoRoot = "../../";

std::string atRoot(const char *relative) {
  return std::string(kRepoRoot) + relative;
}

// Counts non-zero 16-bit pixels in a P6 PPM produced by dumpVramPpm.
// Returns -1 when the file is unreadable or malformed.
long countNonZeroPixels(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good())
    return -1;
  std::string magic;
  int w = 0, h = 0, maxval = 0;
  in >> magic >> w >> h >> maxval;
  if (magic != "P6" || w <= 0 || h <= 0)
    return -1;
  in.get(); // single whitespace byte before the pixel payload
  std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3);
  in.read(reinterpret_cast<char *>(px.data()),
          static_cast<std::streamsize>(px.size()));
  if (in.gcount() != static_cast<std::streamsize>(px.size()))
    return -1;
  long n = 0;
  for (size_t i = 0; i + 2 < px.size(); i += 3)
    if (px[i] || px[i + 1] || px[i + 2])
      ++n;
  return n;
}

bool exists(const std::string &path) {
  if (FILE *f = std::fopen(path.c_str(), "r")) {
    std::fclose(f);
    return true;
  }
  return false;
}

// The golden frame contract for Crash bring-up.  Skipped when the game config
// or the runtime binary is absent, matching the skip-if-no-rom pattern in
// ps1Test/pipeline/.
TEST(GoldenFrame, CrashRendersContentAtFrame200) {
  const std::string config = atRoot("configs/crash.toml");
  const std::string runtime = atRoot("build/ps1Runtime/ps1Runtime");
  if (!exists(config))
    GTEST_SKIP() << "configs/crash.toml not found. Skipping.";
  if (!exists(runtime))
    GTEST_SKIP() << "ps1Runtime binary not found. Skipping.";

  const std::string ppm = "/tmp/ps1_golden_frame_test.ppm";
  std::remove(ppm.c_str());

  // Needs a real display.  `dummy` has no OpenGL at all -- the runtime exits
  // before the first frame.  `offscreen` has OpenGL but no vsync, so the main
  // loop free-runs at ~880fps and frame 200 lands 0.2s into the boot, before
  // anything is drawn; pushing the capture later there is unstable because
  // content presence then races the per-frame clear.  Under a vsynced display
  // frame 200 is 3.3s in and the capture is repeatable.
  if (!std::getenv("WAYLAND_DISPLAY") && !std::getenv("DISPLAY"))
    GTEST_SKIP() << "No display available; golden frame needs vsynced "
                    "rendering. Skipping.";

  // Run from the repository root: the disc paths inside crash.toml are
  // relative to it.
  const std::string cmd =
      "cd " + std::string(kRepoRoot) +
      " && SDL_AUDIODRIVER=dummy PS1_VRAM_DUMP_FRAME=200 PS1_VRAM_DUMP_PATH=" +
      ppm +
      " timeout --kill-after=5 25 ./build/ps1Runtime/ps1Runtime "
      "--config configs/crash.toml > /dev/null 2>&1";
  std::system(cmd.c_str());

  const long n = countNonZeroPixels(ppm);
  ASSERT_GE(n, 0) << "VRAM capture missing or malformed at " << ppm;

  // Lower bound only.  The exact figure is recorded when Phase 1 lands a
  // visible title screen; until then this asserts the pipeline produces
  // pixels at all, which is what regressed silently before.
  EXPECT_GT(n, 0) << "VRAM is entirely black at frame 200";

  std::remove(ppm.c_str());
}

} // namespace
