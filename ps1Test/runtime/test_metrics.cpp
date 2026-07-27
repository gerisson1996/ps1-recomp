#include "runtime/metrics.h"

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>

namespace {

class MetricsTest : public ::testing::Test {
protected:
  void SetUp() override {
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(true);
  }
  void TearDown() override {
    ps1::metrics::reset();
    ps1::metrics::setEnabledForTesting(false);
  }
};

TEST_F(MetricsTest, CountAccumulatesAcrossCalls) {
  ps1::metrics::count("gp0.op.7C");
  ps1::metrics::count("gp0.op.7C", 5);
  EXPECT_EQ(ps1::metrics::get("gp0.op.7C"), 6u);
}

TEST_F(MetricsTest, CountIsUncappedPastLegacyLogLimits) {
  // The bug this whole effort exists for: the old logs stopped at 10 or 20.
  for (int i = 0; i < 5000; ++i)
    ps1::metrics::count("fill_rect");
  EXPECT_EQ(ps1::metrics::get("fill_rect"), 5000u);
}

TEST_F(MetricsTest, UnknownCounterReadsZero) {
  EXPECT_EQ(ps1::metrics::get("never.touched"), 0u);
}

TEST_F(MetricsTest, SetStateKeepsMostRecentValue) {
  ps1::metrics::setState("display.x", 0);
  ps1::metrics::setState("display.x", 512);
  EXPECT_EQ(ps1::metrics::getState("display.x"), 512);
}

TEST_F(MetricsTest, DisabledCollectorRecordsNothing) {
  ps1::metrics::setEnabledForTesting(false);
  ps1::metrics::count("ignored");
  EXPECT_EQ(ps1::metrics::get("ignored"), 0u);
}

TEST_F(MetricsTest, DumpJsonWritesCountersAndState) {
  ps1::metrics::count("frames", 42);
  ps1::metrics::setState("display.y", -7);

  const char *path = "/tmp/ps1_metrics_dump_test.json";
  std::remove(path);
  ASSERT_TRUE(ps1::metrics::dumpJsonTo(path));

  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string body = buf.str();

  EXPECT_NE(body.find("\"frames\": 42"), std::string::npos) << body;
  EXPECT_NE(body.find("\"display.y\": -7"), std::string::npos) << body;
  std::remove(path);
}

} // namespace
