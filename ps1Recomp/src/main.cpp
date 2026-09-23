// ps1Recomp -- PS1 Static Recompiler
// Translates MIPS I instructions to C++ code (1:1 literal translation)

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <map>
#include <set>
#include <ps1recomp/dispatch_emitter.h>
#include <ps1recomp/elf_parser.h>
#include <ps1recomp/hle_emitter.h>
#include <ps1recomp/instruction_emitter.h>
#include <ps1recomp/jump_analysis.h>
#include <ps1recomp/mips_decoder.h>
#include <ps1recomp/overlay_handler.h>
#include <string>
#include <toml.hpp>
#include <vector>

using namespace ps1recomp;

// Jump Table Auto-Detection
//
// Detects the canonical MIPS switch table pattern:
//   SLTIU $rchk, $ridx, N     ; bounds check against the entry count
//   BEQZ  $rchk, default      ; out-of-range -> default case
//   SLL   $rscale, $ridx, 2   ; scale index to a word offset (delay slot)
//   LUI   $rbase, hi          ; rbase = upper address
//   ADDIU $rbase, $rbase, lo  ; rbase = table_base
//   ADDU  $rtbl, $rbase, $rscale ; rtbl = table_base + index*4
//   LW    $rtgt, N($rtbl)     ; rtgt = table[index]
//   JR    $rtgt               ; indirect jump
//
// When found, reads the target addresses from the ELF data section
// and stores them in a JumpTableEntry on the RecompFunction.
//
// The entry count comes from the SLTIU/SLTI bounds check when one is present.
// Without it there is nothing in the code stream that marks where the table
// ends, so we fall back to a conservative fixed window: a table is only
// distinguishable from the data that follows it by that bound.
//
// Returns a vector of target addresses (empty if pattern not found).

static std::vector<uint32_t>
detectJumpTable(const std::vector<uint32_t> &instrs, size_t jr_idx,
                const ElfParser &parser) {
  if (jr_idx == 0)
    return {};

  Instruction jr_inst = MipsDecoder::decode(instrs[jr_idx]);
  uint8_t target_reg = jr_inst.rs; // register that holds the jump target

  // Step 1: Find LW $target_reg, N($base_reg) within 6 instructions back
  int lw_idx = -1;
  uint8_t lw_base_reg = 0;
  int16_t lw_imm = 0;
  for (int j = (int)jr_idx - 1; j >= 0 && j >= (int)jr_idx - 6; j--) {
    Instruction inst = MipsDecoder::decode(instrs[j]);
    if (inst.id == InstrId::LW && inst.rt == target_reg) {
      lw_idx = j;
      lw_base_reg = inst.rs;
      lw_imm = inst.imm16;
      break;
    }
  }
  if (lw_idx < 0)
    return {};

  // Step 2: Find ADDU $lw_base_reg, $rconst, $rscale within 8 instructions back.
  // One of rs/rt is the constant table pointer, the other the scaled index.
  int addu_idx = -1;
  uint8_t addu_rs = 0, addu_rt = 0;
  for (int j = lw_idx - 1; j >= 0 && j >= (int)jr_idx - 12; j--) {
    Instruction inst = MipsDecoder::decode(instrs[j]);
    if (inst.id == InstrId::ADDU && inst.rd == lw_base_reg) {
      addu_idx = j;
      addu_rs = inst.rs;
      addu_rt = inst.rt;
      break;
    }
  }
  if (addu_idx < 0)
    return {};

  // Step 3: Track a candidate register back to LUI (+ optional ADDIU).
  // Whichever ADDU operand resolves to a LUI is the table base; the other
  // one is the scaled index.
  uint32_t lui_val = 0;
  int16_t addiu_imm = 0;
  auto traceToLui = [&](uint8_t reg) {
    lui_val = 0;
    addiu_imm = 0;
    for (int j = lw_idx - 1; j >= 0 && j >= (int)jr_idx - 20; j--) {
      Instruction inst = MipsDecoder::decode(instrs[j]);
      if (inst.id == InstrId::LUI && inst.rt == reg) {
        lui_val = static_cast<uint32_t>(static_cast<uint16_t>(inst.imm16)) << 16;
        return true;
      }
      if ((inst.id == InstrId::ADDIU || inst.id == InstrId::ADDI) &&
          inst.rt == reg && inst.rs == reg) {
        addiu_imm = inst.imm16;
      }
    }
    return false;
  };

  uint8_t scaled_reg = addu_rt;
  if (!traceToLui(addu_rs)) {
    scaled_reg = addu_rs;
    if (!traceToLui(addu_rt))
      return {};
  }

  uint32_t table_addr = lui_val + static_cast<uint32_t>(addiu_imm) +
                        static_cast<uint32_t>(lw_imm);

  // Step 4: Recover the entry count from the bounds check that guards the
  // table. SLL $scaled_reg, $ridx, 2 gives the raw index register, and the
  // SLTIU/SLTI against that register carries the number of entries.
  uint32_t max_entries = kJumpTableFallbackEntries;
  int sll_idx = -1;
  uint8_t index_reg = 0;
  for (int j = addu_idx - 1; j >= 0 && j >= (int)jr_idx - 24; j--) {
    Instruction inst = MipsDecoder::decode(instrs[j]);
    if (inst.id == InstrId::SLL && inst.rd == scaled_reg && inst.shamt == 2) {
      sll_idx = j;
      index_reg = inst.rt;
      break;
    }
  }
  if (sll_idx >= 0) {
    for (int j = sll_idx - 1; j >= 0 && j >= (int)jr_idx - 28; j--) {
      Instruction inst = MipsDecoder::decode(instrs[j]);
      if ((inst.id == InstrId::SLTIU || inst.id == InstrId::SLTI) &&
          inst.rs == index_reg) {
        uint32_t bound = static_cast<uint32_t>(static_cast<uint16_t>(inst.imm16));
        if (bound > 0 && bound <= kJumpTableMaxEntries)
          max_entries = bound;
        break;
      }
    }
  }

  // Step 5: Read table entries from ELF data section
  const Section *section = parser.findSectionByAddress(table_addr);
  if (!section || section->data == nullptr)
    return {};

  uint32_t offset = table_addr - section->vaddr;
  std::vector<uint32_t> targets;
  for (uint32_t i = 0; i < max_entries; i++) {
    uint32_t entry_offset = offset + i * 4;
    if (entry_offset + 4 > section->size)
      break;
    const uint8_t *ptr = section->data + entry_offset;
    uint32_t entry = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
    // Valid PS1 KSEG0 address range: stop at anything outside it
    if (entry < 0x80000000u || entry > 0x80400000u)
      break;
    targets.push_back(entry);
  }
  return targets;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fmt::print("Usage: ps1Recomp <config.toml> [output_file.cpp]\n");
    return 1;
  }

  std::string config_path = argv[1];
  std::string output_path = (argc >= 3) ? argv[2] : "recompiled_out.cpp";

  try {
    const auto config = toml::parse(config_path);

    // binary path is relative to the config file ideally, but for now we assume
    // it builds from project root
    std::string elf_path = toml::find<std::string>(config, "binary", "path");
    uint32_t entry_point = std::stoul(
        toml::find<std::string>(config, "binary", "entry_point"), nullptr, 16);

    fmt::print("Loading binary: {}\n", elf_path);
    ps1recomp::ElfParser parser;
    if (!parser.load(elf_path)) {
      fmt::print(stderr, "Failed to load ELF: {}\n", parser.getError());
      return 1;
    }

    // HLE overrides: replace a function body with a named HLE stub
    // Config section: [hle_overrides]
    //   "0x801AA484" = "hle_DrawSync"   (calls ps1::psyq::hle_DrawSync)
    //   "0x801B954C" = "hle_VSync"      (calls ps1::psyq::hle_VSync)
    std::map<uint32_t, std::string> hleOverrides;
    if (config.contains("hle_overrides")) {
      const auto &tbl = toml::find(config, "hle_overrides").as_table();
      for (const auto &[key, val] : tbl) {
        uint32_t addr = std::stoul(key, nullptr, 16);
        hleOverrides[addr] = val.as_string();
      }
      fmt::print("HLE overrides: {} address(es)\n", hleOverrides.size());
    }

    // Missing functions: emit a literal C++ body for dead-code stubs
    // Config section: [missing_functions]
    //   "0x8019F848" = "recomp_dispatch(rdram, ctx, 0x8019F85C);"
    std::map<uint32_t, std::string> missingFunctions;
    if (config.contains("missing_functions")) {
      const auto &tbl = toml::find(config, "missing_functions").as_table();
      for (const auto &[key, val] : tbl) {
        uint32_t addr = std::stoul(key, nullptr, 16);
        missingFunctions[addr] = val.as_string();
      }
      fmt::print("Missing functions: {} address(es)\n", missingFunctions.size());
    }

    // HLE functions: hash-detected PsyQ stubs from ps1Analyzer Sessao 0.4
    // Config section: [[hle_functions]]
    //   address = "0x801ABCDE"
    //   hle     = true
    //   name    = "libgpu_PutDispEnv"  (the <library>_<basename> identifier)
    //
    // The recompiler skips MIPS translation for these and emits a stub that
    // calls `psyq_dispatch("<name>", ctx)` -- resolved at runtime by the PsyQ
    // registry in ps1Runtime/src/psyq/psyq_registry.cpp.
    std::map<uint32_t, std::string> hleFunctions;
    if (config.contains("hle_functions")) {
      const auto &arr =
          toml::find<std::vector<toml::value>>(config, "hle_functions");
      for (const auto &h : arr) {
        bool hle = h.contains("hle") ? toml::find<bool>(h, "hle") : false;
        if (!hle)
          continue;
        uint32_t addr =
            std::stoul(toml::find<std::string>(h, "address"), nullptr, 16);
        std::string name = toml::find<std::string>(h, "name");
        hleFunctions[addr] = name;
      }
      fmt::print("HLE functions ([[hle_functions]]): {}\n", hleFunctions.size());
    }

    std::string result_cpp = "// Generated by ps1Recomp\n";
    result_cpp += "#include <cstdint>\n";
    result_cpp += "#include <cstdio>\n";
    // The fatal path in recomp_dispatch prints a host stack, because the
    // guest RA is stale whenever the call came through a direct JAL.
    result_cpp += "#ifndef __SWITCH__\n#include <execinfo.h>\n#endif\n";
    result_cpp += "#ifndef __SWITCH__\n#include <fmt/format.h>\n#endif\n";
    result_cpp += "#ifdef __SWITCH__\n";
    result_cpp += "namespace fmt {\n";
    result_cpp += "template <typename... Args> inline void print(const char* s, Args&&...) { ::fputs(s, stdout); }\n";
    result_cpp += "template <typename Stream, typename... Args> inline void print(Stream* f, const char* s, Args&&...) { ::fputs(s, f); }\n";
    result_cpp += "}\n#endif\n";
    result_cpp += "#include <runtime/ps1_runtime_macros.h>\n";
    result_cpp += "#include <runtime/cpu_context.h>\n";
    result_cpp += "#include <runtime/gte.h>\n";
    result_cpp += "#include <runtime/bios/bios.h>\n";
    result_cpp += "#include <runtime/psyq/psyq_hle.h>\n\n";
    result_cpp += "// Forward declaration for OOB dispatch\n";
    result_cpp += "void recomp_dispatch(uint8_t* rdram, recomp_context* ctx, "
                  "uint32_t addr);\n\n";

    const auto &functions =
        toml::find<std::vector<toml::value>>(config, "functions");

    std::map<uint32_t, std::string> addr_to_name;
    std::vector<uint32_t> known_funcs;
    for (const auto &func : functions) {
      std::string name = toml::find<std::string>(func, "name");
      if (name == "main")
        name = "func_main";
      for (char &c : name)
        if (c == '.')
          c = '_';
      uint32_t addr =
          std::stoul(toml::find<std::string>(func, "address"), nullptr, 16);
      known_funcs.push_back(addr);
      addr_to_name[addr] = name;
      result_cpp +=
          fmt::format("void {}(uint8_t* rdram, recomp_context* ctx);\n", name);
    }

    // Forward-declare HLE overrides and missing functions so they are
    // visible to the dispatch table and to other functions that call them.
    for (const auto &[addr, stub] : hleOverrides) {
      if (!addr_to_name.count(addr)) {
        addr_to_name[addr] = fmt::format("func_{:08X}", addr);
        known_funcs.push_back(addr);
        result_cpp += fmt::format(
            "void func_{:08X}(uint8_t* rdram, recomp_context* ctx);\n", addr);
      }
    }
    for (const auto &[addr, body] : missingFunctions) {
      if (!addr_to_name.count(addr)) {
        addr_to_name[addr] = fmt::format("func_{:08X}", addr);
        known_funcs.push_back(addr);
        result_cpp += fmt::format(
            "void func_{:08X}(uint8_t* rdram, recomp_context* ctx);\n", addr);
      }
    }

    // Forward-declare the runtime PsyQ dispatcher once (resolution by name
    // happens at runtime via psyq_registry), and ensure each HLE'd address
    // has a translation-unit-local symbol name.
    if (!hleFunctions.empty()) {
      result_cpp += ps1recomp::emitHleForwardDecl(ps1recomp::HleStub{});
      for (const auto &[addr, hleName] : hleFunctions) {
        if (!addr_to_name.count(addr)) {
          addr_to_name[addr] = fmt::format("func_{:08X}", addr);
          known_funcs.push_back(addr);
          result_cpp += fmt::format(
              "void func_{:08X}(uint8_t* rdram, recomp_context* ctx);\n", addr);
        }
      }
    }

    // Also include stubs in known functions if they exist.
    // [[hle_functions]] takes precedence: when the same address appears in
    // both, the HLE body is emitted under func_<addr> and we must not let
    // the legacy stub overwrite the dispatch entry.
    if (config.contains("stubs")) {
      const auto &stubs = toml::find<std::vector<toml::value>>(config, "stubs");
      for (const auto &s : stubs) {
        std::string name = toml::find<std::string>(s, "name");
        if (name == "main")
          name = "func_main";
        for (char &c : name)
          if (c == '.')
            c = '_';
        uint32_t addr =
            std::stoul(toml::find<std::string>(s, "address"), nullptr, 16);
        if (hleFunctions.count(addr))
          continue;
        known_funcs.push_back(addr);
        addr_to_name[addr] = name;
        result_cpp += fmt::format(
            "void {}(uint8_t* rdram, recomp_context* ctx);\n", name);
      }
    }

    result_cpp += "\n";

    InstructionEmitter emitter;
    emitter.setFuncResolver([&addr_to_name](uint32_t addr) {
      if (addr_to_name.count(addr))
        return addr_to_name[addr];
      return fmt::format("func_{:08X}", addr);
    });

    fmt::print("Recompiling {} functions...\n", functions.size());

    // Tracks HLE addresses for which we've already emitted a body, so the
    // post-loop sweep below doesn't double-emit any entry that overlapped
    // with [[functions]].
    std::set<uint32_t> emittedHleAddrs;

    for (const auto &func : functions) {
      std::string name = toml::find<std::string>(func, "name");
      if (name == "main")
        name = "func_main";
      for (char &c : name)
        if (c == '.')
          c = '_';
      uint32_t addr =
          std::stoul(toml::find<std::string>(func, "address"), nullptr, 16);
      uint32_t size = toml::find<int64_t>(func, "size");

      // Extract instructions
      auto *section = parser.findSectionByAddress(addr);
      if (!section || section->type != SectionType::Text)
        continue;

      uint32_t offset = addr - section->vaddr;
      uint32_t num_instrs = size / 4;

      RecompFunction rfunc;
      rfunc.name = name;
      rfunc.address = addr;
      rfunc.size = size;
      rfunc.instructions.resize(num_instrs);
      rfunc.isLabelTarget.resize(num_instrs, false);

      for (uint32_t i = 0; i < num_instrs; ++i) {
        const uint8_t *ptr = section->data + offset + (i * 4);
        rfunc.instructions[i] =
            ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);

        Instruction inst = MipsDecoder::decode(rfunc.instructions[i]);
        if (inst.category == InstrCategory::Branch) {
          uint32_t pc = addr + (i * 4);
          uint32_t target = inst.branchTarget(pc);
          if (target >= addr && target < addr + size) {
            uint32_t target_idx = (target - addr) / 4;
            if (target_idx < num_instrs)
              rfunc.isLabelTarget[target_idx] = true;
          }
        } else if (inst.category == InstrCategory::Jump &&
                   (inst.id == InstrId::J || inst.id == InstrId::JAL)) {
          uint32_t pc = addr + (i * 4);
          uint32_t target = inst.jumpTarget(pc);
          if (target >= addr && target < addr + size) {
            uint32_t target_idx = (target - addr) / 4;
            if (target_idx < num_instrs)
              rfunc.isLabelTarget[target_idx] = true;
          }
          if (inst.id == InstrId::JAL) {
            bool found = false;
            for (auto k : known_funcs) {
              if (k == target) {
                found = true;
                break;
              }
            }
            if (!found) {
              known_funcs.push_back(target);
              result_cpp += fmt::format(
                  "void func_{:08X}(uint8_t* rdram, recomp_context* ctx) {{ /* "
                  "STUB generated by JAL */ }}\n",
                  target);
            }
          }
        } else if (inst.id == InstrId::JR && inst.rs != 31) {
          // Jump table auto-detection
          // Try to detect the LUI+ADDIU+ADDU+LW+JR pattern and resolve
          // the table entries from the ELF binary.
          auto targets = detectJumpTable(rfunc.instructions, i, parser);
          const char *how = "table entries detected";
          if (targets.empty()) {
            // No table in memory: the target may be computed from a code
            // address inside this function (Duff's device).
            targets = ps1recomp::detectComputedCodeJump(rfunc.instructions, i, addr);
            how = "computed in-function targets";
          }
          if (!targets.empty()) {
            JumpTableEntry jt;
            jt.jrInstrIdx = i;
            jt.targets = std::move(targets);
            fmt::print("  [jump table] func 0x{:08X}+{}: {} targets ({})\n",
                       addr, i * 4, jt.targets.size(), how);
            rfunc.jumpTables.push_back(std::move(jt));
          }
        }
      }

      // HLE function (hash-detected): delegate to extern "C" hle_<name>
      if (hleFunctions.count(addr)) {
        ps1recomp::HleStub stub{addr, name, hleFunctions[addr]};
        result_cpp += ps1recomp::emitHleStub(stub);
        result_cpp += "\n";
        emittedHleAddrs.insert(addr);
        continue;
      }

      // HLE override: emit a named stub instead of translating MIPS
      if (hleOverrides.count(addr)) {
        const std::string &stub = hleOverrides[addr];
        result_cpp += fmt::format(
            "// HLE override for 0x{:08X} (replaces MIPS translation)\n"
            "void {}(uint8_t* rdram, recomp_context* ctx) {{\n"
            "    ps1::psyq::{}(ctx);\n"
            "}}\n\n",
            addr, name, stub);
        continue;
      }

      // Missing function: emit literal body
      if (missingFunctions.count(addr)) {
        const std::string &body = missingFunctions[addr];
        result_cpp += fmt::format(
            "// Missing function stub for 0x{:08X}\n"
            "void {}(uint8_t* rdram, recomp_context* ctx) {{\n"
            "    {}\n"
            "}}\n\n",
            addr, name, body);
        continue;
      }

      result_cpp += emitter.emitFunction(rfunc);
      result_cpp += "\n";
    }

    // HLE stub bodies for [[hle_functions]] not covered by [[functions]]
    // The analyzer emits a [[hle_functions]] entry for every hash match but
    // also strips those addresses from [[functions]] (PsyQ functions don't
    // get MIPS bodies). So most HLE entries land here -- bodies are emitted
    // under the `func_<addr>` symbol the dispatch table maps to.
    for (const auto &[addr, hleName] : hleFunctions) {
      if (emittedHleAddrs.count(addr))
        continue;
      // [hle_overrides] / [missing_functions] take precedence: they emit
      // their own body under the same `func_<addr>` symbol below.
      if (hleOverrides.count(addr) || missingFunctions.count(addr))
        continue;
      std::string fname = addr_to_name.count(addr)
                              ? addr_to_name[addr]
                              : fmt::format("func_{:08X}", addr);
      ps1recomp::HleStub stub{addr, fname, hleName};
      result_cpp += ps1recomp::emitHleStub(stub);
      result_cpp += "\n";
      emittedHleAddrs.insert(addr);
    }

    // Emit stub implementations for PsyQ stubs.
    // For well-known PsyQ SDK function names, delegate to the psyq_hle layer
    // so that VSync/DrawSync/etc. have proper behaviour.
    // Unknown names fall back to a safe no-op body.
    if (config.contains("stubs")) {
      const auto &stubs = toml::find<std::vector<toml::value>>(config, "stubs");
      for (const auto &s : stubs) {
        std::string name = toml::find<std::string>(s, "name");
        for (char &c : name)
          if (c == '.')
            c = '_';
        uint32_t stubAddr =
            std::stoul(toml::find<std::string>(s, "address"), nullptr, 16);
        if (hleFunctions.count(stubAddr))
          continue;

        std::string body;
        if (name == "VSync") {
          body = "ps1::psyq::hle_VSync(ctx);";
        } else if (name == "DrawSync") {
          body = "ps1::psyq::hle_DrawSync(ctx);";
        } else if (name == "ResetGraph") {
          body = "ps1::psyq::hle_ResetGraph(ctx);";
        } else if (name == "ClearOTag") {
          body = "ps1::psyq::hle_ClearOTag(ctx);";
        } else if (name == "ClearOTagR") {
          body = "ps1::psyq::hle_ClearOTagR(ctx);";
        } else if (name == "DrawOTag") {
          body = "ps1::psyq::hle_DrawOTag(ctx);";
        } else if (name == "SetDefDispEnv") {
          body = "ps1::psyq::hle_SetDefDispEnv(ctx);";
        } else if (name == "PutDispEnv") {
          body = "ps1::psyq::hle_PutDispEnv(ctx);";
        } else if (name == "SetDefDrawEnv") {
          body = "ps1::psyq::hle_SetDefDrawEnv(ctx);";
        } else if (name == "PutDrawEnv") {
          body = "ps1::psyq::hle_PutDrawEnv(ctx);";
        } else {
          body = "/* PsyQ Stub */";
        }

        result_cpp += fmt::format(
            "void {}(uint8_t* rdram, recomp_context* ctx) {{ {} }}\n",
            name, body);
      }
    }

    // Overlay support
    // Parse [[overlays]] from config. Each overlay is a separate code segment
    // loaded from disc into RAM at a specific address. Functions are prefixed
    // with overlay name to avoid collisions when multiple overlays share RAM.
    OverlayHandler overlayHandler;
    if (config.contains("overlays")) {
      const auto &overlays =
          toml::find<std::vector<toml::value>>(config, "overlays");

      fmt::print("Processing {} overlay(s)...\n", overlays.size());

      for (const auto &ov : overlays) {
        std::string ovl_name = toml::find<std::string>(ov, "name");
        uint32_t ovl_ramBase =
            static_cast<uint32_t>(toml::find<int64_t>(ov, "ram_base"));
        uint32_t ovl_size =
            static_cast<uint32_t>(toml::find<int64_t>(ov, "size"));

        // Load overlay binary data
        std::string ovl_path;
        if (ov.contains("binary_path")) {
          ovl_path = toml::find<std::string>(ov, "binary_path");
        } else if (ov.contains("disc_path")) {
          ovl_path = toml::find<std::string>(ov, "disc_path");
        }

        std::vector<uint8_t> ovl_data;
        uint32_t ovl_codeOffset = 0;
        uint32_t ovl_codeSize = ovl_size;

        if (!ovl_path.empty()) {
          // Resolve path relative to config file directory
          std::filesystem::path cfgDir =
              std::filesystem::path(config_path).parent_path();
          std::filesystem::path fullPath = cfgDir / ovl_path;
          if (!std::filesystem::exists(fullPath))
            fullPath = ovl_path; // try as absolute

          std::ifstream ovlFile(fullPath, std::ios::binary);
          if (ovlFile) {
            ovlFile.seekg(0, std::ios::end);
            auto fileSize = ovlFile.tellg();
            ovlFile.seekg(0, std::ios::beg);
            ovl_data.resize(fileSize);
            ovlFile.read(reinterpret_cast<char *>(ovl_data.data()), fileSize);

            // Check for PS-X EXE header
            if (ovl_data.size() >= 2048 &&
                std::memcmp(ovl_data.data(), "PS-X EXE", 8) == 0) {
              ovl_codeOffset = 2048;
              // Read tAddr/tSize from header
              uint32_t tAddr, tSize;
              std::memcpy(&tAddr, ovl_data.data() + 24, 4);
              std::memcpy(&tSize, ovl_data.data() + 28, 4);
              ovl_ramBase = tAddr; // Use header's own address
              ovl_codeSize = tSize;
              fmt::print("  [{}] PS-X EXE: ram=0x{:08X}, code={}B\n", ovl_name,
                         ovl_ramBase, ovl_codeSize);
            }
          } else {
            fmt::print(stderr, "  Warning: Could not open overlay binary: {}\n",
                       fullPath.string());
            continue;
          }
        }

        // Get function list -- from config or scan for prologues
        std::vector<uint32_t> ovl_funcs;
        if (ov.contains("functions")) {
          auto funcs = toml::find<std::vector<int64_t>>(ov, "functions");
          for (auto f : funcs)
            ovl_funcs.push_back(static_cast<uint32_t>(f));
        } else if (!ovl_data.empty() &&
                   ovl_codeOffset + ovl_codeSize <= ovl_data.size()) {
          // Auto-detect function prologues
          const uint8_t *code = ovl_data.data() + ovl_codeOffset;
          size_t codeLen = ovl_codeSize;
          for (size_t i = 0; i + 3 < codeLen; i += 4) {
            uint32_t word = code[i] | (code[i + 1] << 8) |
                            (code[i + 2] << 16) | (code[i + 3] << 24);
            // ADDIU $sp, $sp, -N (stack frame allocation = prologue)
            uint8_t op = (word >> 26) & 0x3F;
            uint8_t rs = (word >> 21) & 0x1F;
            uint8_t rt = (word >> 16) & 0x1F;
            int16_t imm = static_cast<int16_t>(word & 0xFFFF);
            if (op == 0x09 && rs == 29 && rt == 29 && imm < 0) {
              ovl_funcs.push_back(ovl_ramBase + static_cast<uint32_t>(i));
            }
          }
          fmt::print("  [{}] Auto-detected {} functions\n", ovl_name,
                     ovl_funcs.size());
        }

        // Determine function sizes (distance to next function)
        std::sort(ovl_funcs.begin(), ovl_funcs.end());

        // Forward-declare overlay functions
        for (size_t fi = 0; fi < ovl_funcs.size(); ++fi) {
          uint32_t fAddr = ovl_funcs[fi];
          std::string fName =
              fmt::format("overlay_{}__{:08X}", ovl_name, fAddr);
          addr_to_name[fAddr] = fName;
          known_funcs.push_back(fAddr);
          result_cpp += fmt::format(
              "void {}(uint8_t* rdram, recomp_context* ctx);\n", fName);
        }

        // Emit overlay functions
        if (!ovl_data.empty() &&
            ovl_codeOffset + ovl_codeSize <= ovl_data.size()) {
          const uint8_t *code = ovl_data.data() + ovl_codeOffset;

          for (size_t fi = 0; fi < ovl_funcs.size(); ++fi) {
            uint32_t fAddr = ovl_funcs[fi];
            uint32_t fEnd = (fi + 1 < ovl_funcs.size())
                                ? ovl_funcs[fi + 1]
                                : (ovl_ramBase + ovl_codeSize);
            uint32_t fSize = fEnd - fAddr;

            uint32_t localOff = fAddr - ovl_ramBase;
            if (localOff + fSize > ovl_codeSize)
              continue;

            uint32_t numInstrs = fSize / 4;
            RecompFunction rfunc;
            rfunc.name = fmt::format("overlay_{}__{:08X}", ovl_name, fAddr);
            rfunc.address = fAddr;
            rfunc.size = fSize;
            rfunc.instructions.resize(numInstrs);
            rfunc.isLabelTarget.resize(numInstrs, false);

            for (uint32_t ii = 0; ii < numInstrs; ++ii) {
              const uint8_t *ptr = code + localOff + (ii * 4);
              rfunc.instructions[ii] =
                  ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);

              Instruction inst = MipsDecoder::decode(rfunc.instructions[ii]);
              if (inst.category == InstrCategory::Branch) {
                uint32_t pc = fAddr + (ii * 4);
                uint32_t target = inst.branchTarget(pc);
                if (target >= fAddr && target < fAddr + fSize) {
                  uint32_t idx = (target - fAddr) / 4;
                  if (idx < numInstrs)
                    rfunc.isLabelTarget[idx] = true;
                }
              } else if (inst.category == InstrCategory::Jump &&
                         (inst.id == InstrId::J || inst.id == InstrId::JAL)) {
                uint32_t pc = fAddr + (ii * 4);
                uint32_t target = inst.jumpTarget(pc);
                if (target >= fAddr && target < fAddr + fSize) {
                  uint32_t idx = (target - fAddr) / 4;
                  if (idx < numInstrs)
                    rfunc.isLabelTarget[idx] = true;
                }
              }
            }

            result_cpp += emitter.emitFunction(rfunc);
            result_cpp += "\n";
          }
        } else if (ovl_data.empty()) {
          // No binary data -- emit stubs for declared functions
          for (auto fAddr : ovl_funcs) {
            std::string fName =
                fmt::format("overlay_{}__{:08X}", ovl_name, fAddr);
            result_cpp +=
                fmt::format("void {}(uint8_t* rdram, recomp_context* ctx) "
                            "{{ /* Overlay stub -- no binary data */ }}\n",
                            fName);
          }
        }

        // Register overlay section for tracking
        OverlaySection sec;
        sec.name = ovl_name;
        sec.ramBase = ovl_ramBase;
        sec.size = ovl_codeSize;
        sec.functions = ovl_funcs;
        overlayHandler.addOverlay(std::move(sec));

        fmt::print("  Overlay [{}]: ram=0x{:08X}, size={}, {} functions\n",
                   ovl_name, ovl_ramBase, ovl_codeSize, ovl_funcs.size());
      }
    }

    // Emit overlay table
    // Runtime uses this to know which overlays exist and their address ranges
    if (overlayHandler.overlayCount() > 0) {
      result_cpp += "\n// Overlay Table\n";
      result_cpp += "#include <vector>\n";
      result_cpp += "#include <string>\n\n";
      result_cpp += "struct OverlayInfo {\n";
      result_cpp += "    std::string name;\n";
      result_cpp += "    uint32_t ramBase;\n";
      result_cpp += "    uint32_t size;\n";
      result_cpp += "    int index;\n";
      result_cpp += "};\n\n";
      result_cpp += "static std::vector<OverlayInfo> recomp_overlay_table = {\n";
      int idx = 0;
      for (const auto &ovl : overlayHandler.overlays()) {
        result_cpp += fmt::format(
            "    {{ \"{}\", 0x{:08X}, {}, {} }},\n", ovl.name, ovl.ramBase,
            ovl.size, idx++);
      }
      result_cpp += "};\n\n";
    }

    // Emit dispatch table
    // This allows CALL_INDIRECT / JUMP_INDIRECT to resolve
    // addresses to recompiled functions at runtime.
    // Uses std::unordered_map for O(1) average-case lookup.
    result_cpp += "\n// Dispatch Table\n";
    result_cpp += "// Maps PS1 addresses to recompiled function pointers\n";
    result_cpp += "// Populated once at startup, queried on every indirect call/jump\n\n";
    result_cpp += "#include <unordered_map>\n";
    result_cpp += "#include <cstdlib>\n"; // getenv, abort
    result_cpp += "#include <cstring>\n"; // strcmp
    result_cpp += "#include <cstdio>\n\n"; // fflush
    result_cpp +=
        "typedef void (*recomp_func_t)(uint8_t*, recomp_context*);\n\n";

    // Global dispatch table (unordered_map)
    result_cpp += "static std::unordered_map<uint32_t, recomp_func_t> recomp_func_table;\n";
    result_cpp += "static bool recomp_table_ready = false;\n\n";

    // Init function: populate the map once
    result_cpp += "void recomp_init_dispatch_table() {\n";
    result_cpp += fmt::format("    recomp_func_table.reserve({});\n",
                              addr_to_name.size() + 16);
    for (const auto &[addr, name] : addr_to_name) {
      result_cpp += fmt::format("    recomp_func_table[0x{:08X}] = {};\n", addr, name);
    }
    result_cpp += "    recomp_table_ready = true;\n";
    result_cpp += "}\n\n";

    // Lookup function
    result_cpp += "recomp_func_t recomp_lookup(uint32_t addr) {\n";
    result_cpp += "    auto it = recomp_func_table.find(addr);\n";
    result_cpp += "    return (it != recomp_func_table.end()) ? it->second : nullptr;\n";
    result_cpp += "}\n\n";

    // Main dispatch function -- body lives in dispatch_emitter.cpp so the
    // unmapped-target policy it encodes can be pinned by tests.
    result_cpp += ps1recomp::emitDispatchBody();

    std::ofstream out(output_path);
    if (!out) {
      fmt::print(stderr, "Failed to write output to {}\n", output_path);
      return 1;
    }
    out << result_cpp;
    fmt::print("Success! Emitted Recompiled source to: {}\n", output_path);
    fmt::print("  Dispatch table: {} entries\n", addr_to_name.size());

  } catch (const std::exception &e) {
    fmt::print(stderr, "Error parsing config: {}\n", e.what());
    return 1;
  }

  return 0;
}
