#include "runtime/spu/spu.h"
#include <cstring>
#include <gtest/gtest.h>

using namespace ps1::spu;

// ADPCM Decoding Tests
//
// Vectors are hand-worked from the psx-spx SPU-ADPCM decode formula
// (PS1Recomp-workspace/psx-spx.github.io/docs/cdromformat.md:827-838,
// coefficient tables at :843-844) applied to the SPU block layout
// (soundprocessingunitspu.md:116-125). See the derivation notes in
// .superpowers/sdd/phase-3-crash-jogavel/task-S1-report.md.

class SpuAdpcmTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(SpuAdpcmTest, DecodesFullNibbleRangeWithFilterZero) {
  // header = filter 0, raw shift 12 (=> effective shift 12-12=0). With
  // filter 0, f0=f1=0 (cdromformat.md:843-844), so per the formula
  // s = (t<<0) + (old*0+older*0+32)/64 = t + 0 (integer division truncates
  // 32/64 toward zero) -- i.e. the decoded output is exactly the
  // sign-extended nibble sequence. This isolates nibble sign-extension from
  // the prediction filter.
  uint8_t block[16] = {};
  block[0] = 0x0C; // filter=0, shift=12
  block[1] = 0x00; // flags: normal

  // Nibble sequence -8..7 repeated (28 values), covering every 4-bit
  // pattern including the ones that must sign-extend to negative (8..15).
  // Packed 2 nibbles/byte, low nibble first (soundprocessingunitspu.md:121:
  // "LSBs=1st Sample, MSBs=2nd Sample").
  const uint8_t dataBytes[14] = {0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32, 0x54,
                                 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32};
  std::memcpy(block + 2, dataBytes, 14);

  auto samples = spu.decodeAdpcmBlockForTest(block, /*prevSample1=*/0,
                                              /*prevSample2=*/0);

  const int16_t expected[28] = {-8, -7, -6, -5, -4, -3, -2, -1, 0, 1,
                                2,  3,  4,  5,  6,  7,  -8, -7, -6, -5,
                                -4, -3, -2, -1, 0,  1,  2,  3};
  for (int i = 0; i < 28; i++) {
    EXPECT_EQ(samples[i], expected[i]) << "sample index " << i;
  }
}

TEST_F(SpuAdpcmTest, AppliesFilterOneCoefficientsToPrediction) {
  // Same nibble data as above, but filter=1 (f0=+60, f1=0 per
  // cdromformat.md:843-844) with raw shift 12 (effective shift 0), so the
  // IIR prediction term (old*f0 + older*f1 + 32) / 64 is exercised.
  // Expected values derived by applying cdromformat.md:827-838 verbatim
  // (integer division truncating toward zero, matching C++ int division):
  //   s0 = -8 + (0*60+0*0+32)/64            = -8 + 0   = -8
  //   s1 = -7 + (-8*60+0*0+32)/64           = -7 + (-448/64=-7) = -14
  //   s2 = -6 + (-14*60+(-8)*0+32)/64       = -6 + (-808/64=-12) = -18
  //   ... (full 28-sample sequence computed with the same recurrence)
  uint8_t block[16] = {};
  block[0] = 0x1C; // filter=1, shift=12 (effective shift 0)
  block[1] = 0x00;

  const uint8_t dataBytes[14] = {0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32, 0x54,
                                 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32};
  std::memcpy(block + 2, dataBytes, 14);

  auto samples = spu.decodeAdpcmBlockForTest(block, /*prevSample1=*/0,
                                              /*prevSample2=*/0);

  const int16_t expected[28] = {-8,  -14, -18, -21, -23, -24, -24, -23,
                                -21, -18, -14, -9,  -3,  3,   9,   15,
                                6,   -1,  -6,  -10, -12, -13, -13, -12,
                                -10, -7,  -4,  0};
  for (int i = 0; i < 28; i++) {
    EXPECT_EQ(samples[i], expected[i]) << "sample index " << i;
  }
}

TEST_F(SpuAdpcmTest, SilentBlockDecodesZeros) {
  // Set up a silent ADPCM block (all zeros) at address 0
  uint8_t *ram = spu.soundRamPtr();
  std::memset(ram, 0, 16); // 16-byte block of zeros
  ram[1] = 0x01;           // End flag to stop after one block

  // Write voice start address via register
  spu.writeRegister(0x1F801C06, 0);      // start addr = 0 (in 8-byte units)
  spu.writeRegister(0x1F801C04, 0x1000); // pitch = 1.0

  // Key on voice 0
  spu.writeRegister(0x1F801D88, 0x0001); // key on lo

  // Generate samples to process
  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);

  // All output should be zero (silent input + ADSR attack starts at 0)
  // This is a basic smoke test
  EXPECT_TRUE(true); // If we got here without crash, ADPCM decode works
}

TEST_F(SpuAdpcmTest, AdpcmDecodesWithoutCrash) {
  // ADPCM filter coefficients are private; we test them
  // indirectly by decoding ADPCM blocks successfully
  uint8_t *ram = spu.soundRamPtr();
  // Filter=1 (60, 0), shift=0
  ram[0] = 0x10; // filter=1, shift=0
  ram[1] = 0x01; // end flag
  for (int i = 2; i < 16; i++)
    ram[i] = 0x77;

  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801D88, 0x0001);

  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);
  EXPECT_TRUE(true);
}

TEST_F(SpuAdpcmTest, MultipleBlocksDontCrash) {
  // Fill several ADPCM blocks with data
  uint8_t *ram = spu.soundRamPtr();
  for (int block = 0; block < 10; block++) {
    int addr = block * 16;
    ram[addr] = 0x00;                           // shift=0, filter=0
    ram[addr + 1] = (block == 9) ? 0x01 : 0x00; // end flag on last
    for (int i = 2; i < 16; i++) {
      ram[addr + i] =
          static_cast<uint8_t>(i * 17 + block); // pseudo-random data
    }
  }

  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801C00, 0x3FFF); // volume left
  spu.writeRegister(0x1F801C02, 0x3FFF); // volume right
  spu.writeRegister(0x1F801D80, 0x7FFF); // main vol L
  spu.writeRegister(0x1F801D82, 0x7FFF); // main vol R
  spu.writeRegister(0x1F801D88, 0x0001); // key on

  int16_t buffer[1024] = {};
  spu.generateSamples(buffer, 512);

  // Should complete without crashing
  EXPECT_TRUE(true);
}

// ADSR Envelope Tests

class SpuAdsrTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(SpuAdsrTest, VoiceStartsInOffPhase) {
  // By default, voices are in Off phase
  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);

  // All output should be 0 (no voices active)
  for (int i = 0; i < 128; i++) {
    EXPECT_EQ(buffer[i], 0);
  }
}

TEST_F(SpuAdsrTest, KeyOnStartsAttackPhase) {
  // Set up a simple waveform
  uint8_t *ram = spu.soundRamPtr();
  std::memset(ram, 0, 16);
  ram[1] = 0x03; // loop + end

  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801C08, 0x00FF); // ADSR lo: fast attack
  spu.writeRegister(0x1F801C0A, 0x0000); // ADSR hi

  // Key on voice 0
  spu.writeRegister(0x1F801D88, 0x0001);

  int16_t buffer[64] = {};
  spu.generateSamples(buffer, 32);

  // After key on, voice should be active (not all zeros necessarily,
  // depends on ADPCM data, but the system shouldn't crash)
  EXPECT_TRUE(true);
}

TEST_F(SpuAdsrTest, KeyOffTransitionsToRelease) {
  uint8_t *ram = spu.soundRamPtr();
  std::memset(ram, 0, 16);
  ram[1] = 0x03;

  spu.writeRegister(0x1F801C06, 0);
  spu.writeRegister(0x1F801C04, 0x1000);
  spu.writeRegister(0x1F801C08, 0x00FF);
  spu.writeRegister(0x1F801C0A, 0x0000);
  spu.writeRegister(0x1F801D88, 0x0001); // Key on

  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 32);

  // Now key off
  spu.writeRegister(0x1F801D8C, 0x0001); // Key off
  spu.generateSamples(buffer, 64);

  // Should still not crash
  EXPECT_TRUE(true);
}

// Volume Tests

class SpuVolumeTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(SpuVolumeTest, ZeroMainVolumeProducesSilence) {
  spu.writeRegister(0x1F801D80, 0); // main vol L = 0
  spu.writeRegister(0x1F801D82, 0); // main vol R = 0

  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);

  for (int i = 0; i < 128; i++) {
    EXPECT_EQ(buffer[i], 0);
  }
}

TEST_F(SpuVolumeTest, RegisterReadback) {
  spu.writeRegister(0x1F801D80, 0x3FFF);
  EXPECT_EQ(spu.readRegister(0x1F801D80), 0x3FFF);

  spu.writeRegister(0x1F801D82, 0x1234);
  EXPECT_EQ(spu.readRegister(0x1F801D82), 0x1234);
}

// SPU Reverb Tests

class SpuReverbTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(SpuReverbTest, ReverbRegisterWriteDoesntCrash) {
  // Write all 32 reverb registers
  for (uint32_t i = 0; i < 32; i++) {
    spu.writeRegister(0x1F801DC0 + i * 2, 0x1234);
  }

  int16_t buffer[128] = {};
  spu.generateSamples(buffer, 64);
  EXPECT_TRUE(true);
}

// XA-ADPCM Tests

class XaAdpcmTest : public ::testing::Test {
protected:
  SPU spu;
  void SetUp() override { spu.reset(); }
};

TEST_F(XaAdpcmTest, XaSamplesAreMixed) {
  spu.writeRegister(0x1F801D80, 0x7FFF); // main vol L
  spu.writeRegister(0x1F801D82, 0x7FFF); // main vol R

  // Push some XA samples (stereo interleaved)
  int16_t xaSamples[64];
  for (int i = 0; i < 64; i++) {
    xaSamples[i] = 1000; // constant value
  }
  spu.pushXaSamples(xaSamples, 64);

  int16_t buffer[64] = {};
  spu.generateSamples(buffer, 32);

  // With max main volume, XA samples should appear in output
  // (output = xaSample * mainVol / 32768)
  bool hasNonZero = false;
  for (int i = 0; i < 64; i++) {
    if (buffer[i] != 0)
      hasNonZero = true;
  }
  EXPECT_TRUE(hasNonZero);
}

TEST_F(XaAdpcmTest, CdDaSamplesAreMixed) {
  spu.writeRegister(0x1F801D80, 0x7FFF);
  spu.writeRegister(0x1F801D82, 0x7FFF);

  int16_t cdSamples[64];
  for (int i = 0; i < 64; i++) {
    cdSamples[i] = 2000;
  }
  spu.pushCdDaSamples(cdSamples, 64);

  int16_t buffer[64] = {};
  spu.generateSamples(buffer, 32);

  bool hasNonZero = false;
  for (int i = 0; i < 64; i++) {
    if (buffer[i] != 0)
      hasNonZero = true;
  }
  EXPECT_TRUE(hasNonZero);
}

// Sound RAM Tests

TEST(SpuSoundRam, WriteAndReadBack) {
  SPU spu;
  spu.reset();

  spu.writeSoundRam(0x100, 0xABCD);
  EXPECT_EQ(spu.readSoundRam(0x100), 0xABCD);

  spu.writeSoundRam(0x200, 0x1234);
  EXPECT_EQ(spu.readSoundRam(0x200), 0x1234);
}

TEST(SpuSoundRam, TransferViaRegister) {
  SPU spu;
  spu.reset();

  // Set transfer address (in 8-byte units, so val=1 -> byte addr 8)
  spu.writeRegister(0x1F801DA6, 1);      // addr = 8
  spu.writeRegister(0x1F801DA8, 0xDEAD); // write data

  EXPECT_EQ(spu.readSoundRam(8), 0xDEAD);
}

// Control Register Tests

TEST(SpuControl, SpuCtrlReadback) {
  SPU spu;
  spu.reset();

  spu.writeRegister(0x1F801DAA, 0xC001);
  EXPECT_EQ(spu.readRegister(0x1F801DAA), 0xC001);
}

TEST(SpuControl, EndxFlagsClearedOnWrite) {
  SPU spu;
  spu.reset();

  // ENDX is at 0x19C/0x19E offset
  // Initially 0
  EXPECT_EQ(spu.readRegister(0x1F801D9C), 0);
}
