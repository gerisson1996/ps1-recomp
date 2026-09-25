#include "runtime/cdrom/cdrom_controller.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>

using namespace ps1::cdrom;

class CdromControllerTest : public ::testing::Test {
protected:
  CdromController cdrom;
  void SetUp() override { cdrom.reset(); }
};

TEST_F(CdromControllerTest, InitialState) {
  EXPECT_EQ(cdrom.getState(), CdromState::Idle);
  EXPECT_FALSE(cdrom.hasInterrupt());
}

TEST_F(CdromControllerTest, GetStatCommand) {
  // Write index 0 to select command registers
  cdrom.writeRegister(0x1F801800, 0x00);
  // Send GetStat (0x01)
  cdrom.writeRegister(0x1F801801, 0x01);
  // Tick to process command
  cdrom.tick(100000);
  // Should have interrupt response
  EXPECT_TRUE(cdrom.hasInterrupt());
  // Read response - should be status byte
  uint8_t stat = cdrom.readRegister(0x1F801801);
  EXPECT_NE(stat, 0); // Motor should be on after GetStat
}

TEST_F(CdromControllerTest, SetLocCommand) {
  cdrom.writeRegister(0x1F801800, 0x00);
  // Set parameters: minute=0, second=2, sector=0 (LBA 0)
  cdrom.writeRegister(0x1F801802, 0x00); // minute BCD
  cdrom.writeRegister(0x1F801802, 0x02); // second BCD
  cdrom.writeRegister(0x1F801802, 0x00); // sector BCD
  // Send SetLoc (0x02)
  cdrom.writeRegister(0x1F801801, 0x02);
  cdrom.tick(100000);
  EXPECT_TRUE(cdrom.hasInterrupt());
}

TEST_F(CdromControllerTest, InitCommand) {
  cdrom.writeRegister(0x1F801800, 0x00);
  cdrom.writeRegister(0x1F801801, 0x0A); // Init
  cdrom.tick(100000);
  EXPECT_EQ(cdrom.getState(), CdromState::Idle);
  EXPECT_TRUE(cdrom.hasInterrupt());
}

TEST_F(CdromControllerTest, ReadTOCSeparatesAcknowledgeAndComplete) {
  cdrom.writeRegister(0x1F801800, 0x00);
  cdrom.writeRegister(0x1F801801, 0x1E); // ReadTOC

  // First response must remain visible until the guest acknowledges it.
  EXPECT_EQ(cdrom.interruptFlag(), INT_ACKNOWLEDGE);

  // ACK through the real register path (index 1, port 3). This should expose
  // the queued second response rather than losing it.
  cdrom.writeRegister(0x1F801800, 0x01);
  cdrom.writeRegister(0x1F801803, 0x1F);
  EXPECT_EQ(cdrom.interruptFlag(), INT_COMPLETE);
}

TEST_F(CdromControllerTest, StatusRegisterReflectsState) {
  // Read status register (port 0)
  uint8_t status = cdrom.readRegister(0x1F801800);
  // Index bits should be 0
  EXPECT_EQ(status & 3, 0);
}

TEST_F(CdromControllerTest, InterruptAcknowledge) {
  cdrom.writeRegister(0x1F801800, 0x00);
  cdrom.writeRegister(0x1F801801, 0x01); // GetStat
  cdrom.tick(100000);
  EXPECT_TRUE(cdrom.hasInterrupt());
  // Acknowledge interrupt
  cdrom.ackInterrupt(0x1F);
  EXPECT_FALSE(cdrom.hasInterrupt());
}

TEST_F(CdromControllerTest, MsfLbaConversion) {
  // MSF 00:02:00 = LBA 150 (pregap) -> 0 data
  EXPECT_EQ(CdromController::msfToLba(0, 2, 0), 150u);
  EXPECT_EQ(CdromController::msfToLba(0, 0, 0), 0u);
  EXPECT_EQ(CdromController::msfToLba(1, 0, 0), 4500u); // 60*75

  uint8_t m, s, f;
  CdromController::lbaToMsf(150, m, s, f);
  EXPECT_EQ(m, 0);
  EXPECT_EQ(s, 2);
  EXPECT_EQ(f, 0);
}

TEST_F(CdromControllerTest, BcdConversion) {
  EXPECT_EQ(CdromController::toBcd(0), 0x00);
  EXPECT_EQ(CdromController::toBcd(10), 0x10);
  EXPECT_EQ(CdromController::toBcd(59), 0x59);
  EXPECT_EQ(CdromController::fromBcd(0x59), 59);
  EXPECT_EQ(CdromController::fromBcd(0x10), 10);
}

// Sector hand-off
//
// `tick` can replace the controller's sector buffer while consumers are
// active, so callers copy through `takeSectorPayload` under the controller
// lock. A PS1 sector may be consumed by more than one DMA burst, therefore the
// controller also keeps a read cursor until the current INT1 is acknowledged.

namespace {
// Serves sector N filled with the byte N, so a spliced read is visible as a
// buffer that is not uniform.
class CountingFs : public ps1::cdrom::VirtualFs {
public:
  std::optional<ps1::cdrom::Sector> readSector(uint32_t lba) override {
    ps1::cdrom::Sector s{};
    std::memset(s.raw, static_cast<int>(lba & 0xFF), ps1::cdrom::SECTOR_SIZE_RAW);
    ++served;
    return s;
  }
  int served = 0;
};

// Drive the controller until one sector is buffered.
void readOneSector(ps1::cdrom::CdromController &cdrom) {
  cdrom.writeRegister(0x1F801800, 0x00);
  cdrom.writeRegister(0x1F801802, 0x00); // minute
  cdrom.writeRegister(0x1F801802, 0x02); // second
  cdrom.writeRegister(0x1F801802, 0x00); // sector
  cdrom.writeRegister(0x1F801801, 0x02); // SetLoc
  cdrom.tick(100000);
  cdrom.writeRegister(0x1F801801, 0x06); // ReadN
  for (int i = 0; i < 40 && !cdrom.hasSectorReady(); ++i)
    cdrom.tick(100000);
}
} // namespace

TEST_F(CdromControllerTest, TakeSectorPayloadReturnsZeroWhenNoneReady) {
  uint8_t buf[16] = {0xAA};
  EXPECT_EQ(cdrom.takeSectorPayload(buf, sizeof buf), 0u);
  EXPECT_EQ(buf[0], 0xAA) << "must not touch the destination";
}

TEST_F(CdromControllerTest, TakeSectorPayloadCopiesUserDataAndConsumesTheSector) {
  CountingFs fs;
  cdrom.attachVirtualFs(&fs);
  readOneSector(cdrom);
  ASSERT_TRUE(cdrom.hasSectorReady());

  std::array<uint8_t, ps1::cdrom::SECTOR_SIZE_RAW> payload{};
  const uint32_t got = cdrom.takeSectorPayload(payload.data(), payload.size());

  // Default mode is 2048-byte sectors: user data starts 24 bytes in.
  EXPECT_EQ(got, ps1::cdrom::SECTOR_SIZE_RAW - 24u);
  for (uint32_t i = 0; i < got; ++i)
    ASSERT_EQ(payload[i], payload[0]) << "byte " << i << " came from elsewhere";

  EXPECT_FALSE(cdrom.hasSectorReady()) << "the take must consume the sector";
  EXPECT_EQ(cdrom.takeSectorPayload(payload.data(), payload.size()), 0u);
}

TEST_F(CdromControllerTest, TakeSectorPayloadSupportsMultipleReadsOfOneSector) {
  CountingFs fs;
  cdrom.attachVirtualFs(&fs);
  readOneSector(cdrom);
  ASSERT_TRUE(cdrom.hasSectorReady());

  uint8_t first[12] = {};
  uint8_t second[64] = {};
  EXPECT_EQ(cdrom.takeSectorPayload(first, sizeof first), sizeof first);
  EXPECT_TRUE(cdrom.hasSectorReady())
      << "a short DMA must not discard the rest of the current sector";
  EXPECT_EQ(cdrom.takeSectorPayload(second, sizeof second), sizeof second);
  EXPECT_TRUE(cdrom.hasSectorReady());

  // CountingFs fills every byte identically, but two successful reads prove
  // the payload cursor remained live across DMA bursts.
  for (uint8_t b : first)
    EXPECT_EQ(b, first[0]);
  for (uint8_t b : second)
    EXPECT_EQ(b, first[0]);

  // The BIOS acknowledges INT1 after the callback has finished. That releases
  // any unread tail so the next sector can replace the buffer.
  cdrom.clearWaitingForAck();
  EXPECT_FALSE(cdrom.hasSectorReady());
  EXPECT_EQ(cdrom.takeSectorPayload(second, sizeof second), 0u);
}
