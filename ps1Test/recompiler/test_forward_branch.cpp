// Regressao: alvo de branch DENTRO da funcao virando despacho.
//
// Medido em Crash Bandicoot (SCUS-94900), func_8003A144 (size 1576):
//   0x8003A1E0: b 0x8003A264   -- alvo no indice 72 de 394, bem dentro do corpo
// O emissor produzia dois `goto L_8003A264` e nenhuma definicao local, e o
// pos-passo entao sintetizava `L_8003A264: recomp_dispatch(...)`. Com o
// dispatch fatal isso aborta o jogo; antes dele, era um no-op silencioso.

#include "ps1recomp/instruction_emitter.h"
#include <cstdio>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace ps1recomp;

namespace {

constexpr const char *kExe =
    "../../test_roms/Crash Bandicoot /Crash Bandicoot (USA).bin.boot.exe";
constexpr uint32_t kLoadAddr = 0x80010000u;
constexpr uint32_t kHeader = 0x800u;

// Le `count` palavras a partir do endereco virtual `va`.
bool readWords(uint32_t va, size_t count, std::vector<uint32_t> &out) {
  std::FILE *f = std::fopen(kExe, "rb");
  if (!f)
    return false;
  const long off = static_cast<long>(va - kLoadAddr + kHeader);
  if (std::fseek(f, off, SEEK_SET) != 0) {
    std::fclose(f);
    return false;
  }
  out.resize(count);
  const size_t got = std::fread(out.data(), 4, count, f);
  std::fclose(f);
  return got == count;
}

TEST(EmitterForwardBranch, CrashFunc8003A144KeepsItsForwardTargetLocal) {
  std::vector<uint32_t> words;
  if (!readWords(0x8003A144u, 1576 / 4, words))
    GTEST_SKIP() << "Crash boot exe not available; skipping.";

  RecompFunction f;
  f.name = "func_8003A144";
  f.address = 0x8003A144u;
  f.size = 1576;
  f.instructions = words;
  f.isLabelTarget.assign(words.size(), false);

  InstructionEmitter em;
  const std::string out = em.emitFunction(f);

  EXPECT_NE(out.find("goto L_8003A264"), std::string::npos)
      << "o branch para frente deveria virar goto";
  EXPECT_EQ(out.find("recomp_dispatch(rdram, ctx, 0x8003A264)"),
            std::string::npos)
      << "0x8003A264 esta a 288 bytes do inicio de uma funcao de 1576 -- "
         "e alvo interno, nao pode virar despacho";
}

} // namespace
